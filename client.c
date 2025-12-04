#include "client.h"
#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

// 전역 변수
int sock;
char myUsername[NAME_LENGTH];
pthread_mutex_t sockMutex = PTHREAD_MUTEX_INITIALIZER;

int main(int argc, char *argv[]) {
  struct sockaddr_in servAddr;
  pthread_t recvThread;
  char input[BUF_SIZE];

  if (argc != 3) {
    printf("사용법: %s <IP> <Port>\n", argv[0]);
    exit(1);
  }

  // 소켓 생성
  sock = socket(PF_INET, SOCK_STREAM, 0);
  if (sock == -1) {
    perror("socket() error");
    exit(1);
  }

  // 서버 주소 설정
  memset(&servAddr, 0, sizeof(servAddr));
  servAddr.sin_family = AF_INET;
  servAddr.sin_addr.s_addr = inet_addr(argv[1]);
  servAddr.sin_port = htons(atoi(argv[2]));

  // 서버 연결
  if (connect(sock, (struct sockaddr *)&servAddr, sizeof(servAddr)) == -1) {
    perror("connect() error");
    close(sock);
    exit(1);
  }

  printf("=== 채팅 서버에 연결되었습니다 ===\n");

  // 사용자명 입력
  printf("사용자명을 입력하세요: ");
  fgets(myUsername, NAME_LENGTH, stdin);
  myUsername[strcspn(myUsername, "\n")] = '\0';

  // JOIN 패킷 전송
  sendJoin(myUsername);

  // 수신 스레드 생성
  if (pthread_create(&recvThread, NULL, receiveThread, NULL) != 0) {
    perror("pthread_create() error");
    close(sock);
    exit(1);
  }
  pthread_detach(recvThread);

  printf("\n명령어:\n");
  printf("  /help     - 도움말\n");
  printf("  /list     - 접속자 목록\n");
  printf("  /w <name> <msg> - 귓속말\n");
  printf("  /quit     - 종료\n");
  printf("  (일반 메시지는 그냥 입력)\n\n");

  // 메시지 입력 루프
  while (1) {
    printf("> ");
    fflush(stdout);

    if (fgets(input, BUF_SIZE, stdin) == NULL) {
      break;
    }

    // 개행 제거
    input[strcspn(input, "\n")] = '\0';

    // 빈 입력 무시
    if (strlen(input) == 0) {
      continue;
    }

    // 명령어 처리
    if (strcmp(input, "/quit") == 0) {
      sendQuit();
      break;
    } else if (strcmp(input, "/help") == 0) {
      printHelp();
    } else if (strcmp(input, "/list") == 0) {
      sendListOnline();
    } else if (strncmp(input, "/w ", 3) == 0) {
      // 귓속말 파싱: /w <target> <message>
      char *target = strtok(input + 3, " ");
      char *message = strtok(NULL, "");

      if (target && message) {
        sendWhisper(target, message);
      } else {
        printf("사용법: /w <사용자명> <메시지>\n");
      }
    } else {
      // 일반 채팅
      sendChat(input);
    }
  }

  close(sock);
  printf("연결이 종료되었습니다.\n");
  return 0;
}

// 메시지 수신 스레드
void *receiveThread(void *arg) {
  struct PacketHeader header;
  char *body = NULL;

  while (1) {
    // 1. 헤더 수신
    if (recvAll(sock, &header, sizeof(struct PacketHeader)) <= 0) {
      printf("\n서버와의 연결이 끊어졌습니다.\n");
      exit(0);
    }

    // 2. 바디 수신
    if (header.body_length_ > 0) {
      body = (char *)malloc(header.body_length_);
      if (recvAll(sock, body, header.body_length_) <= 0) {
        free(body);
        printf("\n서버와의 연결이 끊어졌습니다.\n");
        exit(0);
      }
    }

    // 3. 액션별 처리
    switch (header.action_) {
    case PROTOCOL_ACTION_CHAT: {
      if (body) {
        struct BodyChat *chat = (struct BodyChat *)body;
        char *message = body + sizeof(struct BodyChat);
        printf("\n[%s] %s\n> ", chat->sender_, message);
        fflush(stdout);
      }
      break;
    }

    case PROTOCOL_ACTION_WHISPER: {
      if (body) {
        struct BodyWhisper *whisper = (struct BodyWhisper *)body;
        char *message = body + sizeof(struct BodyWhisper);
        printf("\n[귓속말 from %s] %s\n> ", whisper->sender_, message);
        fflush(stdout);
      }
      break;
    }

    default:
      break;
    }

    if (body) {
      free(body);
      body = NULL;
    }
  }

  return NULL;
}

// 정확히 length 바이트만큼 수신
int recvAll(int sock, void *buffer, int length) {
  int totalReceived = 0;
  char *ptr = (char *)buffer;

  while (totalReceived < length) {
    int received = recv(sock, ptr + totalReceived, length - totalReceived, 0);
    if (received <= 0) {
      return received;
    }
    totalReceived += received;
  }

  return totalReceived;
}

// 정확히 length 바이트만큼 송신 (뮤텍스 보호)
int sendAll(int sock, void *buffer, int length) {
  pthread_mutex_lock(&sockMutex);

  int totalSent = 0;
  char *ptr = (char *)buffer;

  while (totalSent < length) {
    int sent = send(sock, ptr + totalSent, length - totalSent, 0);
    if (sent <= 0) {
      pthread_mutex_unlock(&sockMutex);
      return sent;
    }
    totalSent += sent;
  }

  pthread_mutex_unlock(&sockMutex);
  return totalSent;
}

// JOIN 패킷 전송
void sendJoin(const char *username) {
  int totalLen = sizeof(struct PacketHeader) + sizeof(struct BodyJoin);
  char *packet = (char *)malloc(totalLen);

  // 헤더
  struct PacketHeader *header = (struct PacketHeader *)packet;
  header->action_ = PROTOCOL_ACTION_JOIN;
  header->body_length_ = sizeof(struct BodyJoin);

  // 바디
  struct BodyJoin *body =
      (struct BodyJoin *)(packet + sizeof(struct PacketHeader));
  strncpy(body->username_, username, NAME_LENGTH - 1);
  body->username_[NAME_LENGTH - 1] = '\0';

  sendAll(sock, packet, totalLen);
  free(packet);
}

// CHAT 패킷 전송
void sendChat(const char *message) {
  int msgLen = strlen(message) + 1;
  int totalLen = sizeof(struct PacketHeader) + sizeof(struct BodyChat) + msgLen;

  char *packet = (char *)malloc(totalLen);

  // 헤더
  struct PacketHeader *header = (struct PacketHeader *)packet;
  header->action_ = PROTOCOL_ACTION_CHAT;
  header->body_length_ = sizeof(struct BodyChat) + msgLen;

  // 바디
  struct BodyChat *body =
      (struct BodyChat *)(packet + sizeof(struct PacketHeader));
  strncpy(body->sender_, myUsername, NAME_LENGTH - 1);
  body->sender_[NAME_LENGTH - 1] = '\0';

  // 메시지
  strcpy(packet + sizeof(struct PacketHeader) + sizeof(struct BodyChat),
         message);

  sendAll(sock, packet, totalLen);
  free(packet);
}

// WHISPER 패킷 전송
void sendWhisper(const char *target, const char *message) {
  int msgLen = strlen(message) + 1;
  int totalLen =
      sizeof(struct PacketHeader) + sizeof(struct BodyWhisper) + msgLen;

  char *packet = (char *)malloc(totalLen);

  // 헤더
  struct PacketHeader *header = (struct PacketHeader *)packet;
  header->action_ = PROTOCOL_ACTION_WHISPER;
  header->body_length_ = sizeof(struct BodyWhisper) + msgLen;

  // 바디
  struct BodyWhisper *body =
      (struct BodyWhisper *)(packet + sizeof(struct PacketHeader));
  strncpy(body->sender_, myUsername, NAME_LENGTH - 1);
  body->sender_[NAME_LENGTH - 1] = '\0';
  strncpy(body->target_, target, NAME_LENGTH - 1);
  body->target_[NAME_LENGTH - 1] = '\0';

  // 메시지
  strcpy(packet + sizeof(struct PacketHeader) + sizeof(struct BodyWhisper),
         message);

  sendAll(sock, packet, totalLen);
  free(packet);
}

// LIST_ONLINE 패킷 전송
void sendListOnline() {
  struct PacketHeader header;
  header.action_ = PROTOCOL_ACTION_LIST_ONLINE;
  header.body_length_ = 0;

  sendAll(sock, &header, sizeof(struct PacketHeader));
}

// QUIT 패킷 전송
void sendQuit() {
  struct PacketHeader header;
  header.action_ = PROTOCOL_ACTION_QUIT;
  header.body_length_ = 0;

  sendAll(sock, &header, sizeof(struct PacketHeader));
}

// 도움말 출력
void printHelp() {
  printf("\n=== 명령어 목록 ===\n");
  printf("  /help              - 이 도움말 표시\n");
  printf("  /list              - 현재 접속자 목록 보기\n");
  printf("  /w <name> <msg>    - 특정 사용자에게 귓속말\n");
  printf("  /quit              - 채팅 종료\n");
  printf("  (명령어가 아닌 일반 텍스트는 모두에게 전송됩니다)\n\n");
}