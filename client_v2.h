#ifndef CLIENT_V2_H
#define CLIENT_V2_H

#include "protocol.h"
#include <pthread.h>
#include <stdio.h>

#define BUF_SIZE 4096
#define DOWNLOAD_DIR "./downloads/"
#define MAX_FILE_SIZE (50 * 1024 * 1024)  // 50MB

// 다운로드 세션
typedef struct {
  int active;
  char filename[FILE_NAME_LENGTH];
  FILE *fp;
  long long total_size;
  long long received;
  int play_after_download;  // 다운로드 완료 후 미디어 재생 여부
} DownloadSession;

// 미디어 플레이어 상태
typedef struct {
  int playing;              // 재생 중 여부
  int stop_requested;       // 중단 요청 플래그
  char filename[FILE_NAME_LENGTH];  // 현재 재생 중인 파일
  pthread_t thread;         // 재생 스레드
  void *vlc_inst;           // libvlc_instance_t*
  void *vlc_player;         // libvlc_media_player_t*
} MediaPlayer;

// 메시지 수신 스레드
void *receiveThread(void *arg);

// 네트워크 유틸리티 함수
int recvAll(int sock, void *buffer, int length);
int sendAll(int sock, void *buffer, int length);

// 패킷 전송 함수
void sendJoin(const char *username);
void sendChat(const char *message);
void sendWhisper(const char *target, const char *message);
void sendListOnline(void);
void sendQuit(void);

// 파일 관련 패킷 전송 함수
void sendListFile(void);
void sendFileUpload(const char *filepath);
void sendFileRequest(const char *filename, int play_flag);

// 파일 수신 처리 함수
void handleFileStart(struct BodyFileStart *body);
void handleFileData(struct BodyFileData *body, int data_len);

// 미디어 파일 관련 함수
int isMediaFile(const char *filename);
int isMP3File(const char *filename);
int isMP4File(const char *filename);

// 미디어 재생 함수
void playMedia(const char *filepath);
void stopMedia(void);
void *playMP3Thread(void *arg);
void *playMP4Thread(void *arg);

// UI 함수
void printHelp(void);

#endif // CLIENT_V2_H
