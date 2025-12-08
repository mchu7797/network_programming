#ifndef CLIENT_H
#define CLIENT_H

#include "protocol.h"
#include <stdio.h>

#define BUF_SIZE 4096
#define DOWNLOAD_DIR "./downloads/"
#define MAX_FILE_SIZE (50 * 1024 * 1024)  // 50MB (서버와 동일)

// 다운로드 세션
typedef struct {
  int active;
  char filename[FILE_NAME_LENGTH];
  FILE *fp;
  long long total_size;
  long long received;
} DownloadSession;

// 메시지 수신 스레드
void *receiveThread(void *arg);

// 네트워크 유틸리티 함수
int recvAll(int sock, void *buffer, int length);
int sendAll(int sock, void *buffer, int length);

// 패킷 전송 함수
void sendJoin(const char *username);
void sendChat(const char *message);
void sendWhisper(const char *target, const char *message);
void sendListOnline();
void sendQuit();

// 파일 관련 패킷 전송 함수
void sendListFile();
void sendFileUpload(const char *filepath);
void sendFileRequest(const char *filename);

// 파일 수신 처리 함수
void handleFileStart(struct BodyFileStart *body);
void handleFileData(struct BodyFileData *body, int data_len);

// UI 함수
void printHelp();

#endif // CLIENT_H
