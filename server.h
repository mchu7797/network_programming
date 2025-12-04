#ifndef SERVER_H
#define SERVER_H

#include "protocol.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_CLIENTS 10000
#define PORT 8888
#define THREAD_POOL_SIZE 8
#define TASK_QUEUE_SIZE 1024
#define MAX_EVENTS 100
#define READ_BUFFER_SIZE 8192

// 클라이언트 세션 상태
typedef enum {
  STATE_READING_HEADER,
  STATE_READING_BODY,
  STATE_PROCESSING
} SessionState;

// 클라이언트 세션 정보
typedef struct {
  int sock;
  char username[NAME_LENGTH];
  int active;

  // 읽기 버퍼 관리
  SessionState state;
  char readBuffer[READ_BUFFER_SIZE];
  int readOffset;
  int expectedBytes;

  // 패킷 정보
  struct PacketHeader currentHeader;
  char *bodyBuffer;
} ClientSession;

// 태스크 타입
typedef enum {
  TASK_READ,  // 소켓에서 데이터 읽기
  TASK_WRITE, // 소켓에 데이터 쓰기
  TASK_CLOSE  // 연결 종료
} TaskType;

// 작업 큐의 태스크
typedef struct {
  TaskType type;
  int clientSock;
} Task;

// 작업 큐
typedef struct {
  Task tasks[TASK_QUEUE_SIZE];
  int head;
  int tail;
  int count;
  pthread_mutex_t mutex;
  pthread_cond_t notEmpty;
  pthread_cond_t notFull;
} TaskQueue;

// epoll + 스레드 풀 함수
void initTaskQueue(TaskQueue *queue);
void pushTask(TaskQueue *queue, Task task);
Task popTask(TaskQueue *queue);
void *workerThread(void *arg);

// 세션 관리
ClientSession *createSession(int sock);
void destroySession(ClientSession *session);
ClientSession *findSessionBySocket(int sock);
ClientSession *findSessionByUsername(const char *username);
void removeSession(int sock);

// 패킷 처리
void handlePacket(ClientSession *session);
int processReadEvent(ClientSession *session);

// 메시지 전송 (epoll 환경용)
void broadcastMessage(const char *sender, const char *message, int senderSock);
int sendWhisper(const char *target, const char *sender, const char *message);
void sendOnlineList(int sock);
void sendPacketToClient(int sock, const char *packet, int length);

// 유틸리티
int setNonBlocking(int sock);

#endif // SERVER_H