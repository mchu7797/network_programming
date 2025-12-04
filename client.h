#ifndef CLIENT_H
#define CLIENT_H

#include "protocol.h"

#define BUF_SIZE 4096

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

// UI 함수
void printHelp();

#endif // CLIENT_H