#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <errno.h>
#include "server.h"

// 전역 변수
ClientSession* sessions[MAX_CLIENTS];
int sessionCount = 0;
pthread_mutex_t sessionsMutex = PTHREAD_MUTEX_INITIALIZER;

TaskQueue taskQueue;
int epollFd;
int running = 1;

// ==================== Task Queue 관리 ====================

void initTaskQueue(TaskQueue* queue) {
    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;
    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->notEmpty, NULL);
    pthread_cond_init(&queue->notFull, NULL);
}

void pushTask(TaskQueue* queue, Task task) {
    pthread_mutex_lock(&queue->mutex);
    
    while (queue->count >= TASK_QUEUE_SIZE) {
        pthread_cond_wait(&queue->notFull, &queue->mutex);
    }
    
    queue->tasks[queue->tail] = task;
    queue->tail = (queue->tail + 1) % TASK_QUEUE_SIZE;
    queue->count++;
    
    pthread_cond_signal(&queue->notEmpty);
    pthread_mutex_unlock(&queue->mutex);
}

Task popTask(TaskQueue* queue) {
    pthread_mutex_lock(&queue->mutex);
    
    while (queue->count == 0 && running) {
        pthread_cond_wait(&queue->notEmpty, &queue->mutex);
    }
    
    Task task = {0};
    if (queue->count > 0) {
        task = queue->tasks[queue->head];
        queue->head = (queue->head + 1) % TASK_QUEUE_SIZE;
        queue->count--;
        pthread_cond_signal(&queue->notFull);
    }
    
    pthread_mutex_unlock(&queue->mutex);
    return task;
}

// ==================== Session 관리 ====================

ClientSession* createSession(int sock) {
    ClientSession* session = (ClientSession*)malloc(sizeof(ClientSession));
    if (!session) return NULL;
    
    memset(session, 0, sizeof(ClientSession));
    session->sock = sock;
    session->active = 1;
    session->state = STATE_READING_HEADER;
    session->readOffset = 0;
    session->expectedBytes = sizeof(struct PacketHeader);
    session->bodyBuffer = NULL;
    
    pthread_mutex_lock(&sessionsMutex);
    if (sessionCount < MAX_CLIENTS) {
        sessions[sessionCount++] = session;
    }
    pthread_mutex_unlock(&sessionsMutex);
    
    return session;
}

void destroySession(ClientSession* session) {
    if (session) {
        if (session->bodyBuffer) {
            free(session->bodyBuffer);
        }
        free(session);
    }
}

ClientSession* findSessionBySocket(int sock) {
    ClientSession* result = NULL;
    pthread_mutex_lock(&sessionsMutex);
    for (int i = 0; i < sessionCount; i++) {
        if (sessions[i]->sock == sock) {
            result = sessions[i];
            break;
        }
    }
    pthread_mutex_unlock(&sessionsMutex);
    return result;
}

ClientSession* findSessionByUsername(const char* username) {
    ClientSession* result = NULL;
    pthread_mutex_lock(&sessionsMutex);
    for (int i = 0; i < sessionCount; i++) {
        if (sessions[i]->active && strcmp(sessions[i]->username, username) == 0) {
            result = sessions[i];
            break;
        }
    }
    pthread_mutex_unlock(&sessionsMutex);
    return result;
}

void removeSession(int sock) {
    pthread_mutex_lock(&sessionsMutex);
    for (int i = 0; i < sessionCount; i++) {
        if (sessions[i]->sock == sock) {
            destroySession(sessions[i]);
            sessions[i] = sessions[sessionCount - 1];
            sessionCount--;
            break;
        }
    }
    pthread_mutex_unlock(&sessionsMutex);
}

// ==================== 패킷 처리 ====================

int processReadEvent(ClientSession* session) {
    while (1) {
        int needBytes = session->expectedBytes - session->readOffset;
        if (needBytes <= 0) {
            return 1; // 완료
        }
        
        int n = recv(session->sock, 
                     session->readBuffer + session->readOffset, 
                     needBytes, 
                     0);
        
        if (n > 0) {
            session->readOffset += n;
            
            if (session->readOffset >= session->expectedBytes) {
                if (session->state == STATE_READING_HEADER) {
                    // 헤더 읽기 완료
                    memcpy(&session->currentHeader, session->readBuffer, sizeof(struct PacketHeader));
                    
                    if (session->currentHeader.body_length_ > 0) {
                        // 바디 읽기 준비
                        session->state = STATE_READING_BODY;
                        session->bodyBuffer = (char*)malloc(session->currentHeader.body_length_);
                        session->expectedBytes = session->currentHeader.body_length_;
                        session->readOffset = 0;
                        memset(session->readBuffer, 0, READ_BUFFER_SIZE);
                    } else {
                        // 바디 없음, 바로 처리
                        session->state = STATE_PROCESSING;
                        return 1;
                    }
                } else if (session->state == STATE_READING_BODY) {
                    // 바디 읽기 완료
                    memcpy(session->bodyBuffer, session->readBuffer, session->currentHeader.body_length_);
                    session->state = STATE_PROCESSING;
                    return 1;
                }
            }
        } else if (n == 0) {
            return -1; // 연결 종료
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0; // 더 읽을 데이터 없음, 나중에 다시
            }
            return -1; // 에러
        }
    }
}

void handlePacket(ClientSession* session) {
    struct PacketHeader* header = &session->currentHeader;
    char* body = session->bodyBuffer;
    
    switch (header->action_) {
        case PROTOCOL_ACTION_JOIN: {
            if (body) {
                struct BodyJoin* join = (struct BodyJoin*)body;
                strncpy(session->username, join->username_, NAME_LENGTH - 1);
                session->username[NAME_LENGTH - 1] = '\0';
                
                printf("[JOIN] %s (소켓: %d)\n", session->username, session->sock);
                
                char joinMsg[256];
                snprintf(joinMsg, sizeof(joinMsg), "%s님이 입장하셨습니다.", session->username);
                broadcastMessage("SERVER", joinMsg, -1);
            }
            break;
        }
        
        case PROTOCOL_ACTION_CHAT: {
            if (body) {
                struct BodyChat* chat = (struct BodyChat*)body;
                char* message = body + sizeof(struct BodyChat);
                printf("[CHAT] %s: %s\n", chat->sender_, message);
                broadcastMessage(chat->sender_, message, session->sock);
            }
            break;
        }
        
        case PROTOCOL_ACTION_WHISPER: {
            if (body) {
                struct BodyWhisper* whisper = (struct BodyWhisper*)body;
                char* message = body + sizeof(struct BodyWhisper);
                printf("[WHISPER] %s -> %s: %s\n", 
                       whisper->sender_, whisper->target_, message);
                
                if (!sendWhisper(whisper->target_, whisper->sender_, message)) {
                    char errorMsg[] = "해당 사용자를 찾을 수 없습니다.";
                    int msgLen = strlen(errorMsg) + 1;
                    int totalLen = sizeof(struct PacketHeader) + sizeof(struct BodyChat) + msgLen;
                    
                    char* packet = (char*)malloc(totalLen);
                    struct PacketHeader* h = (struct PacketHeader*)packet;
                    h->action_ = PROTOCOL_ACTION_CHAT;
                    h->body_length_ = sizeof(struct BodyChat) + msgLen;
                    
                    struct BodyChat* c = (struct BodyChat*)(packet + sizeof(struct PacketHeader));
                    strncpy(c->sender_, "SERVER", NAME_LENGTH);
                    strcpy(packet + sizeof(struct PacketHeader) + sizeof(struct BodyChat), errorMsg);
                    
                    sendPacketToClient(session->sock, packet, totalLen);
                    free(packet);
                }
            }
            break;
        }
        
        case PROTOCOL_ACTION_LIST_ONLINE: {
            printf("[LIST_ONLINE] 요청: %s\n", session->username);
            sendOnlineList(session->sock);
            break;
        }
        
        case PROTOCOL_ACTION_QUIT: {
            printf("[QUIT] %s\n", session->username);
            Task closeTask = {TASK_CLOSE, session->sock};
            pushTask(&taskQueue, closeTask);
            break;
        }
        
        default:
            printf("[알 수 없는 액션] %d\n", header->action_);
            break;
    }
    
    // 처리 완료 후 초기화
    if (session->bodyBuffer) {
        free(session->bodyBuffer);
        session->bodyBuffer = NULL;
    }
    session->state = STATE_READING_HEADER;
    session->readOffset = 0;
    session->expectedBytes = sizeof(struct PacketHeader);
    memset(session->readBuffer, 0, READ_BUFFER_SIZE);
}

// ==================== 워커 스레드 ====================

void* workerThread(void* arg) {
    printf("워커 스레드 시작: %lu\n", pthread_self());
    
    while (running) {
        Task task = popTask(&taskQueue);
        
        if (!running) break;
        
        ClientSession* session = findSessionBySocket(task.clientSock);
        if (!session) continue;
        
        switch (task.type) {
            case TASK_READ: {
                int result = processReadEvent(session);
                
                if (result > 0 && session->state == STATE_PROCESSING) {
                    handlePacket(session);
                } else if (result < 0) {
                    // 연결 종료 또는 에러
                    Task closeTask = {TASK_CLOSE, session->sock};
                    pushTask(&taskQueue, closeTask);
                }
                break;
            }
            
            case TASK_CLOSE: {
                printf("클라이언트 연결 종료 (소켓: %d)\n", session->sock);
                
                if (strlen(session->username) > 0) {
                    char quitMsg[256];
                    snprintf(quitMsg, sizeof(quitMsg), "%s님이 퇴장하셨습니다.", session->username);
                    broadcastMessage("SERVER", quitMsg, session->sock);
                }
                
                epoll_ctl(epollFd, EPOLL_CTL_DEL, session->sock, NULL);
                close(session->sock);
                removeSession(session->sock);
                break;
            }
            
            default:
                break;
        }
    }
    
    return NULL;
}

// ==================== 메시지 전송 ====================

void sendPacketToClient(int sock, const char* packet, int length) {
    // Non-blocking 소켓이므로 send가 부분적으로만 성공할 수 있음
    // 실제 프로덕션에서는 send 버퍼를 관리해야 하지만, 여기서는 단순화
    int totalSent = 0;
    while (totalSent < length) {
        int sent = send(sock, packet + totalSent, length - totalSent, 0);
        if (sent > 0) {
            totalSent += sent;
        } else if (sent < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                break; // 에러
            }
        }
    }
}

void broadcastMessage(const char* sender, const char* message, int senderSock) {
    int msgLen = strlen(message) + 1;
    int totalLen = sizeof(struct PacketHeader) + sizeof(struct BodyChat) + msgLen;
    
    char* packet = (char*)malloc(totalLen);
    
    struct PacketHeader* header = (struct PacketHeader*)packet;
    header->action_ = PROTOCOL_ACTION_CHAT;
    header->body_length_ = sizeof(struct BodyChat) + msgLen;
    
    struct BodyChat* body = (struct BodyChat*)(packet + sizeof(struct PacketHeader));
    strncpy(body->sender_, sender, NAME_LENGTH);
    
    strcpy(packet + sizeof(struct PacketHeader) + sizeof(struct BodyChat), message);
    
    pthread_mutex_lock(&sessionsMutex);
    for (int i = 0; i < sessionCount; i++) {
        if (sessions[i]->active && sessions[i]->sock != senderSock) {
            sendPacketToClient(sessions[i]->sock, packet, totalLen);
        }
    }
    pthread_mutex_unlock(&sessionsMutex);
    
    free(packet);
}

int sendWhisper(const char* target, const char* sender, const char* message) {
    ClientSession* targetSession = findSessionByUsername(target);
    if (!targetSession) {
        return 0;
    }
    
    int msgLen = strlen(message) + 1;
    int totalLen = sizeof(struct PacketHeader) + sizeof(struct BodyWhisper) + msgLen;
    
    char* packet = (char*)malloc(totalLen);
    
    struct PacketHeader* header = (struct PacketHeader*)packet;
    header->action_ = PROTOCOL_ACTION_WHISPER;
    header->body_length_ = sizeof(struct BodyWhisper) + msgLen;
    
    struct BodyWhisper* body = (struct BodyWhisper*)(packet + sizeof(struct PacketHeader));
    strncpy(body->sender_, sender, NAME_LENGTH);
    strncpy(body->target_, target, NAME_LENGTH);
    
    strcpy(packet + sizeof(struct PacketHeader) + sizeof(struct BodyWhisper), message);
    
    sendPacketToClient(targetSession->sock, packet, totalLen);
    free(packet);
    
    return 1;
}

void sendOnlineList(int sock) {
    char listBuffer[4096] = "=== 접속자 목록 ===\n";
    
    pthread_mutex_lock(&sessionsMutex);
    for (int i = 0; i < sessionCount; i++) {
        if (sessions[i]->active && strlen(sessions[i]->username) > 0) {
            strcat(listBuffer, sessions[i]->username);
            strcat(listBuffer, "\n");
        }
    }
    pthread_mutex_unlock(&sessionsMutex);
    
    int msgLen = strlen(listBuffer) + 1;
    int totalLen = sizeof(struct PacketHeader) + sizeof(struct BodyChat) + msgLen;
    
    char* packet = (char*)malloc(totalLen);
    
    struct PacketHeader* header = (struct PacketHeader*)packet;
    header->action_ = PROTOCOL_ACTION_CHAT;
    header->body_length_ = sizeof(struct BodyChat) + msgLen;
    
    struct BodyChat* body = (struct BodyChat*)(packet + sizeof(struct PacketHeader));
    strncpy(body->sender_, "SERVER", NAME_LENGTH);
    
    strcpy(packet + sizeof(struct PacketHeader) + sizeof(struct BodyChat), listBuffer);
    
    sendPacketToClient(sock, packet, totalLen);
    free(packet);
}

// ==================== 유틸리티 ====================

int setNonBlocking(int sock) {
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(sock, F_SETFL, flags | O_NONBLOCK);
}

// ==================== 메인 ====================

int main() {
    int serverSock;
    struct sockaddr_in serverAddr;
    pthread_t workers[THREAD_POOL_SIZE];
    
    memset(sessions, 0, sizeof(sessions));
    
    // 소켓 생성
    serverSock = socket(PF_INET, SOCK_STREAM, 0);
    if (serverSock == -1) {
        perror("socket() error");
        exit(1);
    }
    
    int option = 1;
    setsockopt(serverSock, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));
    
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serverAddr.sin_port = htons(PORT);
    
    if (bind(serverSock, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) == -1) {
        perror("bind() error");
        close(serverSock);
        exit(1);
    }
    
    if (listen(serverSock, 128) == -1) {
        perror("listen() error");
        close(serverSock);
        exit(1);
    }
    
    setNonBlocking(serverSock);
    
    // epoll 생성
    epollFd = epoll_create1(0);
    if (epollFd == -1) {
        perror("epoll_create1() error");
        exit(1);
    }
    
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = serverSock;
    
    if (epoll_ctl(epollFd, EPOLL_CTL_ADD, serverSock, &ev) == -1) {
        perror("epoll_ctl() error");
        exit(1);
    }
    
    // Task Queue 초기화
    initTaskQueue(&taskQueue);
    
    // 워커 스레드 풀 생성
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        if (pthread_create(&workers[i], NULL, workerThread, NULL) != 0) {
            perror("pthread_create() error");
            exit(1);
        }
    }
    
    printf("=== epoll + 스레드풀 채팅 서버 시작 (Port: %d, Workers: %d) ===\n", 
           PORT, THREAD_POOL_SIZE);
    
    struct epoll_event events[MAX_EVENTS];
    
    // epoll 이벤트 루프
    while (running) {
        int nfds = epoll_wait(epollFd, events, MAX_EVENTS, -1);
        
        if (nfds == -1) {
            if (errno == EINTR) continue;
            perror("epoll_wait() error");
            break;
        }
        
        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == serverSock) {
                // 새 연결 수락
                while (1) {
                    struct sockaddr_in clientAddr;
                    socklen_t clientAddrSize = sizeof(clientAddr);
                    int clientSock = accept(serverSock, (struct sockaddr*)&clientAddr, &clientAddrSize);
                    
                    if (clientSock == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break; // 더 이상 accept할 연결 없음
                        } else {
                            perror("accept() error");
                            break;
                        }
                    }
                    
                    setNonBlocking(clientSock);
                    
                    ClientSession* session = createSession(clientSock);
                    if (!session) {
                        close(clientSock);
                        continue;
                    }
                    
                    ev.events = EPOLLIN | EPOLLET; // Edge-triggered
                    ev.data.fd = clientSock;
                    
                    if (epoll_ctl(epollFd, EPOLL_CTL_ADD, clientSock, &ev) == -1) {
                        perror("epoll_ctl() error");
                        destroySession(session);
                        close(clientSock);
                        continue;
                    }
                    
                    printf("새 클라이언트 연결: %s:%d (소켓: %d)\n",
                           inet_ntoa(clientAddr.sin_addr),
                           ntohs(clientAddr.sin_port),
                           clientSock);
                }
            } else {
                // 클라이언트 데이터 수신 이벤트
                Task task = {TASK_READ, events[i].data.fd};
                pushTask(&taskQueue, task);
            }
        }
    }
    
    // 정리
    close(serverSock);
    close(epollFd);
    
    return 0;
}