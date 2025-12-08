#ifndef SERVER_H
#define SERVER_H

#include "protocol.h"
#include <pthread.h>
#include <stdio.h>
#include <sys/epoll.h>

#define MAX_CLIENTS 1024
#define MAX_EVENTS 64
#define BUF_SIZE 8192      // 4KB recv buffer + margin for packet header
#define MESSAGE_QUEUE_SIZE 1024

// 파일 관련 설정 (사용자 조절 가능)
#define FILE_WORKER_COUNT 4
#define FILE_QUEUE_SIZE 1024
#define MAX_FILES 256
#define MAX_FILE_SIZE (50 * 1024 * 1024)  // 50MB
#define UPLOAD_DIR "./uploads/"

// 클라이언트 상태
typedef enum {
  CLIENT_STATE_CONNECTED,
  CLIENT_STATE_JOINED,
  CLIENT_STATE_DISCONNECTED
} ClientState;

// 클라이언트 정보
typedef struct {
  int fd;
  ClientState state;
  char username[NAME_LENGTH];
  char recv_buffer[BUF_SIZE];
  int recv_len;
} Client;

// 메시지 타입
typedef enum {
  MSG_TYPE_BROADCAST,    // 전체 브로드캐스트
  MSG_TYPE_UNICAST,      // 특정 클라이언트에게 전송
  MSG_TYPE_EXCLUDE_ONE   // 특정 클라이언트 제외 브로드캐스트
} MessageType;

// 메시지 큐 아이템
typedef struct {
  MessageType type;
  int target_fd;         // UNICAST: 대상 fd, EXCLUDE_ONE: 제외할 fd
  char *data;
  int data_len;
} Message;

// 스레드 안전 메시지 큐
typedef struct {
  Message *messages;
  int capacity;
  int head;
  int tail;
  int count;
  pthread_mutex_t mutex;
  pthread_cond_t not_empty;
  pthread_cond_t not_full;
  int shutdown;
} MessageQueue;

//==============================================================================
// 파일 전송 관련 자료구조
//==============================================================================

// 파일 슬롯 (공용 파일 저장소 관리)
typedef struct {
  char filename[FILE_NAME_LENGTH];
  int uploading;           // 업로드 진행 중 여부
  int uploader_fd;         // 업로드 중인 클라이언트 fd
  long long filesize;      // 완료된 파일 크기 (업로드 완료 후 설정)
} FileSlot;

// 업로드 세션 (클라이언트별)
typedef struct {
  int active;
  int client_fd;
  char filename[FILE_NAME_LENGTH];
  FILE *fp;
  long long total_size;
  long long received;
} UploadSession;

// 파일 작업 타입
typedef enum {
  FILE_TASK_UPLOAD_START,   // 업로드 시작 (파일 열기)
  FILE_TASK_UPLOAD_DATA,    // 업로드 데이터 (쓰기)
  FILE_TASK_UPLOAD_DONE,    // 업로드 완료
  FILE_TASK_DOWNLOAD        // 다운로드 요청 (읽기)
} FileTaskType;

// 파일 작업 큐 아이템
typedef struct {
  FileTaskType type;
  int client_fd;
  char filename[FILE_NAME_LENGTH];
  long long filesize;       // UPLOAD_START용
  int seq;                  // UPLOAD_DATA용
  char *data;               // UPLOAD_DATA용
  int data_len;
} FileTask;

// 파일 작업 큐 (스레드 안전)
typedef struct {
  FileTask *tasks;
  int capacity;
  int head;
  int tail;
  int count;
  pthread_mutex_t mutex;
  pthread_cond_t not_empty;
  pthread_cond_t not_full;
  int shutdown;
} FileTaskQueue;

// 서버 컨텍스트
typedef struct {
  int server_fd;
  int epoll_fd;
  Client clients[MAX_CLIENTS];
  int client_count;
  pthread_mutex_t clients_mutex;
  MessageQueue *send_queue;
  
  // 파일 관련
  FileSlot file_slots[MAX_FILES];
  int file_count;
  pthread_rwlock_t files_rwlock;
  
  UploadSession upload_sessions[MAX_CLIENTS];
  pthread_mutex_t sessions_mutex;
  
  FileTaskQueue *file_queues[FILE_WORKER_COUNT];
  pthread_t file_workers[FILE_WORKER_COUNT];
} ServerContext;

// 메시지 큐 함수
MessageQueue *queue_create(int capacity);
void queue_destroy(MessageQueue *queue);
int queue_push(MessageQueue *queue, Message *msg);
int queue_pop(MessageQueue *queue, Message *msg);
void queue_shutdown(MessageQueue *queue);

// 파일 작업 큐 함수
FileTaskQueue *file_queue_create(int capacity);
void file_queue_destroy(FileTaskQueue *queue);
int file_queue_push(FileTaskQueue *queue, FileTask *task);
int file_queue_pop(FileTaskQueue *queue, FileTask *task);
void file_queue_shutdown(FileTaskQueue *queue);

// 클라이언트 관리 함수
Client *find_client_by_fd(ServerContext *ctx, int fd);
Client *find_client_by_username(ServerContext *ctx, const char *username);
int add_client(ServerContext *ctx, int fd);
void remove_client(ServerContext *ctx, int fd);
void get_online_list(ServerContext *ctx, char *buffer, int *len);

// 네트워크 함수
int set_nonblocking(int fd);
int create_server_socket(int port);

// 패킷 처리 함수
void handle_client_data(ServerContext *ctx, Client *client);
void process_packet(ServerContext *ctx, Client *client, 
                    struct PacketHeader *header, char *body);

// Worker 스레드 함수
void *worker_thread(void *arg);
void *file_worker_thread(void *arg);

// 메시지 전송 헬퍼 함수
void enqueue_broadcast(ServerContext *ctx, char *data, int len, int exclude_fd);
void enqueue_unicast(ServerContext *ctx, int target_fd, char *data, int len);

// 파일 관련 함수
int find_file_slot(ServerContext *ctx, const char *filename);
int add_file_slot(ServerContext *ctx, const char *filename, int uploader_fd);
void complete_file_slot(ServerContext *ctx, const char *filename, long long size);
void remove_file_slot_by_uploader(ServerContext *ctx, int uploader_fd);
void get_file_list(ServerContext *ctx, char *buffer, int *len);

// 업로드 세션 관리
UploadSession *find_upload_session(ServerContext *ctx, int client_fd);
UploadSession *create_upload_session(ServerContext *ctx, int client_fd);
void close_upload_session(ServerContext *ctx, int client_fd);

// 파일 작업 큐잉
void enqueue_file_task(ServerContext *ctx, FileTask *task);

// 유틸리티
void send_error_message(ServerContext *ctx, int client_fd, const char *error_msg);

#endif // SERVER_H
