#include "server.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static ServerContext *g_ctx = NULL;
static volatile int g_running = 1;

static void signal_handler(int sig) {
  (void)sig;
  g_running = 0;
  printf("\nServer shutting down...\n");
}

//==============================================================================
// Message Queue
//==============================================================================

MessageQueue *queue_create(int capacity) {
  MessageQueue *q = malloc(sizeof(MessageQueue));
  if (!q) return NULL;

  q->messages = malloc(sizeof(Message) * capacity);
  if (!q->messages) { free(q); return NULL; }

  q->capacity = capacity;
  q->head = q->tail = q->count = q->shutdown = 0;
  pthread_mutex_init(&q->mutex, NULL);
  pthread_cond_init(&q->not_empty, NULL);
  pthread_cond_init(&q->not_full, NULL);
  return q;
}

void queue_destroy(MessageQueue *q) {
  if (!q) return;
  pthread_mutex_lock(&q->mutex);
  while (q->count > 0) {
    if (q->messages[q->head].data) free(q->messages[q->head].data);
    q->head = (q->head + 1) % q->capacity;
    q->count--;
  }
  pthread_mutex_unlock(&q->mutex);
  pthread_mutex_destroy(&q->mutex);
  pthread_cond_destroy(&q->not_empty);
  pthread_cond_destroy(&q->not_full);
  free(q->messages);
  free(q);
}

int queue_push(MessageQueue *q, Message *msg) {
  pthread_mutex_lock(&q->mutex);
  while (q->count >= q->capacity && !q->shutdown)
    pthread_cond_wait(&q->not_full, &q->mutex);
  if (q->shutdown) { pthread_mutex_unlock(&q->mutex); return -1; }

  q->messages[q->tail] = *msg;
  q->tail = (q->tail + 1) % q->capacity;
  q->count++;
  pthread_cond_signal(&q->not_empty);
  pthread_mutex_unlock(&q->mutex);
  return 0;
}

int queue_pop(MessageQueue *q, Message *msg) {
  pthread_mutex_lock(&q->mutex);
  while (q->count == 0 && !q->shutdown)
    pthread_cond_wait(&q->not_empty, &q->mutex);
  if (q->shutdown && q->count == 0) { pthread_mutex_unlock(&q->mutex); return -1; }

  *msg = q->messages[q->head];
  q->head = (q->head + 1) % q->capacity;
  q->count--;
  pthread_cond_signal(&q->not_full);
  pthread_mutex_unlock(&q->mutex);
  return 0;
}

void queue_shutdown(MessageQueue *q) {
  pthread_mutex_lock(&q->mutex);
  q->shutdown = 1;
  pthread_cond_broadcast(&q->not_empty);
  pthread_cond_broadcast(&q->not_full);
  pthread_mutex_unlock(&q->mutex);
}

//==============================================================================
// File Task Queue
//==============================================================================

FileTaskQueue *file_queue_create(int capacity) {
  FileTaskQueue *q = malloc(sizeof(FileTaskQueue));
  if (!q) return NULL;

  q->tasks = malloc(sizeof(FileTask) * capacity);
  if (!q->tasks) { free(q); return NULL; }

  q->capacity = capacity;
  q->head = q->tail = q->count = q->shutdown = 0;
  pthread_mutex_init(&q->mutex, NULL);
  pthread_cond_init(&q->not_empty, NULL);
  pthread_cond_init(&q->not_full, NULL);
  return q;
}

void file_queue_destroy(FileTaskQueue *q) {
  if (!q) return;
  pthread_mutex_lock(&q->mutex);
  while (q->count > 0) {
    if (q->tasks[q->head].data) free(q->tasks[q->head].data);
    q->head = (q->head + 1) % q->capacity;
    q->count--;
  }
  pthread_mutex_unlock(&q->mutex);
  pthread_mutex_destroy(&q->mutex);
  pthread_cond_destroy(&q->not_empty);
  pthread_cond_destroy(&q->not_full);
  free(q->tasks);
  free(q);
}

int file_queue_push(FileTaskQueue *q, FileTask *task) {
  pthread_mutex_lock(&q->mutex);
  while (q->count >= q->capacity && !q->shutdown)
    pthread_cond_wait(&q->not_full, &q->mutex);
  if (q->shutdown) { pthread_mutex_unlock(&q->mutex); return -1; }

  q->tasks[q->tail] = *task;
  q->tail = (q->tail + 1) % q->capacity;
  q->count++;
  pthread_cond_signal(&q->not_empty);
  pthread_mutex_unlock(&q->mutex);
  return 0;
}

int file_queue_pop(FileTaskQueue *q, FileTask *task) {
  pthread_mutex_lock(&q->mutex);
  while (q->count == 0 && !q->shutdown)
    pthread_cond_wait(&q->not_empty, &q->mutex);
  if (q->shutdown && q->count == 0) { pthread_mutex_unlock(&q->mutex); return -1; }

  *task = q->tasks[q->head];
  q->head = (q->head + 1) % q->capacity;
  q->count--;
  pthread_cond_signal(&q->not_full);
  pthread_mutex_unlock(&q->mutex);
  return 0;
}

void file_queue_shutdown(FileTaskQueue *q) {
  pthread_mutex_lock(&q->mutex);
  q->shutdown = 1;
  pthread_cond_broadcast(&q->not_empty);
  pthread_cond_broadcast(&q->not_full);
  pthread_mutex_unlock(&q->mutex);
}

//==============================================================================
// Client Management
//==============================================================================

Client *find_client_by_fd(ServerContext *ctx, int fd) {
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (ctx->clients[i].fd == fd && ctx->clients[i].state != CLIENT_STATE_DISCONNECTED)
      return &ctx->clients[i];
  }
  return NULL;
}

Client *find_client_by_username(ServerContext *ctx, const char *username) {
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (ctx->clients[i].state == CLIENT_STATE_JOINED &&
        strcmp(ctx->clients[i].username, username) == 0)
      return &ctx->clients[i];
  }
  return NULL;
}

int add_client(ServerContext *ctx, int fd) {
  pthread_mutex_lock(&ctx->clients_mutex);
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (ctx->clients[i].state == CLIENT_STATE_DISCONNECTED) {
      ctx->clients[i].fd = fd;
      ctx->clients[i].state = CLIENT_STATE_CONNECTED;
      ctx->clients[i].username[0] = '\0';
      ctx->clients[i].recv_len = 0;
      ctx->client_count++;
      pthread_mutex_unlock(&ctx->clients_mutex);
      return 0;
    }
  }
  pthread_mutex_unlock(&ctx->clients_mutex);
  return -1;
}

void remove_client(ServerContext *ctx, int fd) {
  pthread_mutex_lock(&ctx->clients_mutex);
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (ctx->clients[i].fd == fd) {
      ctx->clients[i].state = CLIENT_STATE_DISCONNECTED;
      ctx->clients[i].fd = -1;
      ctx->client_count--;
      break;
    }
  }
  pthread_mutex_unlock(&ctx->clients_mutex);
}

void get_online_list(ServerContext *ctx, char *buffer, int *len) {
  pthread_mutex_lock(&ctx->clients_mutex);
  *len = 0;
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (ctx->clients[i].state == CLIENT_STATE_JOINED) {
      int name_len = strlen(ctx->clients[i].username) + 1;
      memcpy(buffer + *len, ctx->clients[i].username, name_len);
      *len += name_len;
    }
  }
  pthread_mutex_unlock(&ctx->clients_mutex);
}

//==============================================================================
// File Slot Management
//==============================================================================

int find_file_slot(ServerContext *ctx, const char *filename) {
  for (int i = 0; i < ctx->file_count; i++)
    if (strcmp(ctx->file_slots[i].filename, filename) == 0) return i;
  return -1;
}

int add_file_slot(ServerContext *ctx, const char *filename, int uploader_fd) {
  pthread_rwlock_wrlock(&ctx->files_rwlock);
  if (find_file_slot(ctx, filename) >= 0) {
    pthread_rwlock_unlock(&ctx->files_rwlock);
    return -1;
  }
  if (ctx->file_count >= MAX_FILES) {
    pthread_rwlock_unlock(&ctx->files_rwlock);
    return -2;
  }

  FileSlot *slot = &ctx->file_slots[ctx->file_count++];
  strncpy(slot->filename, filename, FILE_NAME_LENGTH - 1);
  slot->filename[FILE_NAME_LENGTH - 1] = '\0';
  slot->uploading = 1;
  slot->uploader_fd = uploader_fd;
  slot->filesize = 0;
  pthread_rwlock_unlock(&ctx->files_rwlock);
  return 0;
}

void complete_file_slot(ServerContext *ctx, const char *filename, long long size) {
  pthread_rwlock_wrlock(&ctx->files_rwlock);
  int idx = find_file_slot(ctx, filename);
  if (idx >= 0) {
    ctx->file_slots[idx].uploading = 0;
    ctx->file_slots[idx].uploader_fd = -1;
    ctx->file_slots[idx].filesize = size;
  }
  pthread_rwlock_unlock(&ctx->files_rwlock);
}

void remove_file_slot_by_uploader(ServerContext *ctx, int uploader_fd) {
  pthread_rwlock_wrlock(&ctx->files_rwlock);
  for (int i = 0; i < ctx->file_count; i++) {
    if (ctx->file_slots[i].uploading && ctx->file_slots[i].uploader_fd == uploader_fd) {
      char filepath[512];
      snprintf(filepath, sizeof(filepath), "%s%s", UPLOAD_DIR, ctx->file_slots[i].filename);
      unlink(filepath);
      if (i < ctx->file_count - 1)
        ctx->file_slots[i] = ctx->file_slots[ctx->file_count - 1];
      ctx->file_count--;
      break;
    }
  }
  pthread_rwlock_unlock(&ctx->files_rwlock);
}

void get_file_list(ServerContext *ctx, char *buffer, int *len) {
  pthread_rwlock_rdlock(&ctx->files_rwlock);
  *len = 0;
  for (int i = 0; i < ctx->file_count; i++) {
    if (!ctx->file_slots[i].uploading) {
      *len += snprintf(buffer + *len, 512, "%s (%lld bytes)\n",
                       ctx->file_slots[i].filename, ctx->file_slots[i].filesize);
    }
  }
  if (*len == 0) { strcpy(buffer, "(no files)"); *len = strlen(buffer); }
  pthread_rwlock_unlock(&ctx->files_rwlock);
}

//==============================================================================
// Upload Session Management
//==============================================================================

UploadSession *find_upload_session(ServerContext *ctx, int client_fd) {
  pthread_mutex_lock(&ctx->sessions_mutex);
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (ctx->upload_sessions[i].active && ctx->upload_sessions[i].client_fd == client_fd) {
      pthread_mutex_unlock(&ctx->sessions_mutex);
      return &ctx->upload_sessions[i];
    }
  }
  pthread_mutex_unlock(&ctx->sessions_mutex);
  return NULL;
}

UploadSession *create_upload_session(ServerContext *ctx, int client_fd) {
  pthread_mutex_lock(&ctx->sessions_mutex);
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (!ctx->upload_sessions[i].active) {
      UploadSession *s = &ctx->upload_sessions[i];
      s->active = 1;
      s->client_fd = client_fd;
      s->fp = NULL;
      s->filename[0] = '\0';
      s->total_size = s->received = 0;
      pthread_mutex_unlock(&ctx->sessions_mutex);
      return s;
    }
  }
  pthread_mutex_unlock(&ctx->sessions_mutex);
  return NULL;
}

void close_upload_session(ServerContext *ctx, int client_fd) {
  pthread_mutex_lock(&ctx->sessions_mutex);
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (ctx->upload_sessions[i].active && ctx->upload_sessions[i].client_fd == client_fd) {
      if (ctx->upload_sessions[i].fp) fclose(ctx->upload_sessions[i].fp);
      ctx->upload_sessions[i].fp = NULL;
      ctx->upload_sessions[i].active = 0;
      break;
    }
  }
  pthread_mutex_unlock(&ctx->sessions_mutex);
}

//==============================================================================
// Network Utilities
//==============================================================================

int set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  return (flags == -1) ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int create_server_socket(int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd == -1) { perror("socket"); return -1; }

  int opt = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);

  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
    perror("bind"); close(fd); return -1;
  }
  if (listen(fd, SOMAXCONN) == -1) {
    perror("listen"); close(fd); return -1;
  }
  return fd;
}

//==============================================================================
// Packet Helper Functions
//==============================================================================

static char *make_chat_packet(const char *sender, const char *message, int *out_len) {
  int msg_len = strlen(message) + 1;
  int total = sizeof(struct PacketHeader) + sizeof(struct BodyChat) + msg_len;
  char *pkt = malloc(total);

  struct PacketHeader *h = (struct PacketHeader *)pkt;
  h->action_ = PROTOCOL_ACTION_CHAT;
  h->body_length_ = sizeof(struct BodyChat) + msg_len;

  struct BodyChat *b = (struct BodyChat *)(pkt + sizeof(struct PacketHeader));
  strncpy(b->sender_, sender, NAME_LENGTH - 1);
  b->sender_[NAME_LENGTH - 1] = '\0';
  memcpy(pkt + sizeof(struct PacketHeader) + sizeof(struct BodyChat), message, msg_len);

  *out_len = total;
  return pkt;
}

//==============================================================================
// Message Sending Helpers
//==============================================================================

void enqueue_broadcast(ServerContext *ctx, char *data, int len, int exclude_fd) {
  Message msg;
  msg.type = (exclude_fd >= 0) ? MSG_TYPE_EXCLUDE_ONE : MSG_TYPE_BROADCAST;
  msg.target_fd = exclude_fd;
  msg.data = malloc(len);
  msg.data_len = len;
  memcpy(msg.data, data, len);
  queue_push(ctx->send_queue, &msg);
}

void enqueue_unicast(ServerContext *ctx, int target_fd, char *data, int len) {
  Message msg;
  msg.type = MSG_TYPE_UNICAST;
  msg.target_fd = target_fd;
  msg.data = malloc(len);
  msg.data_len = len;
  memcpy(msg.data, data, len);
  queue_push(ctx->send_queue, &msg);
}

void send_error_message(ServerContext *ctx, int client_fd, const char *error_msg) {
  int len;
  char *pkt = make_chat_packet("Server", error_msg, &len);
  enqueue_unicast(ctx, client_fd, pkt, len);
  free(pkt);
}

void enqueue_file_task(ServerContext *ctx, FileTask *task) {
  file_queue_push(ctx->file_queues[task->client_fd % FILE_WORKER_COUNT], task);
}

//==============================================================================
// Worker Thread (Message Sending)
//==============================================================================

void *worker_thread(void *arg) {
  ServerContext *ctx = arg;
  Message msg;

  printf("[Worker] Message thread started\n");

  while (queue_pop(ctx->send_queue, &msg) == 0) {
    pthread_mutex_lock(&ctx->clients_mutex);

    for (int i = 0; i < MAX_CLIENTS; i++) {
      Client *c = &ctx->clients[i];
      int should_send = 0;

      switch (msg.type) {
      case MSG_TYPE_BROADCAST:
        should_send = (c->state == CLIENT_STATE_JOINED);
        break;
      case MSG_TYPE_UNICAST:
        should_send = (c->fd == msg.target_fd && c->state != CLIENT_STATE_DISCONNECTED);
        break;
      case MSG_TYPE_EXCLUDE_ONE:
        should_send = (c->state == CLIENT_STATE_JOINED && c->fd != msg.target_fd);
        break;
      }

      if (should_send)
        send(c->fd, msg.data, msg.data_len, MSG_NOSIGNAL);

      if (msg.type == MSG_TYPE_UNICAST && should_send) break;
    }

    pthread_mutex_unlock(&ctx->clients_mutex);
    free(msg.data);
  }

  printf("[Worker] Message thread stopped\n");
  return NULL;
}

//==============================================================================
// File Worker Thread
//==============================================================================

typedef struct { ServerContext *ctx; int id; } FileWorkerArg;

void *file_worker_thread(void *arg) {
  FileWorkerArg *wa = arg;
  ServerContext *ctx = wa->ctx;
  int id = wa->id;
  free(wa);

  FileTask task;
  printf("[FileWorker %d] Started\n", id);

  while (file_queue_pop(ctx->file_queues[id], &task) == 0) {
    switch (task.type) {
    case FILE_TASK_UPLOAD_START:
      break;

    case FILE_TASK_UPLOAD_DATA: {
      UploadSession *s = find_upload_session(ctx, task.client_fd);
      if (!s || !s->fp) {
        printf("[FileWorker %d] No session fd=%d\n", id, task.client_fd);
        free(task.data);
        break;
      }

      fwrite(task.data, 1, task.data_len, s->fp);
      s->received += task.data_len;

      if (s->received >= s->total_size) {
        fclose(s->fp);
        s->fp = NULL;
        printf("[FileWorker %d] Upload complete: %s\n", id, s->filename);
        complete_file_slot(ctx, s->filename, s->total_size);

        char msg[512];
        snprintf(msg, sizeof(msg), "[Notice] File '%s' upload complete", s->filename);
        send_error_message(ctx, task.client_fd, msg);
        close_upload_session(ctx, task.client_fd);
      }
      free(task.data);
      break;
    }

    case FILE_TASK_UPLOAD_DONE:
      break;

    case FILE_TASK_DOWNLOAD: {
      char filepath[512];
      snprintf(filepath, sizeof(filepath), "%s%s", UPLOAD_DIR, task.filename);

      FILE *fp = fopen(filepath, "rb");
      if (!fp) {
        send_error_message(ctx, task.client_fd, "[Error] File not found.");
        break;
      }

      fseek(fp, 0, SEEK_END);
      long long filesize = ftell(fp);
      fseek(fp, 0, SEEK_SET);

      printf("[FileWorker %d] Download: %s (%lld bytes) -> fd=%d\n",
             id, task.filename, filesize, task.client_fd);

      // FILE_START packet
      {
        int len = sizeof(struct PacketHeader) + sizeof(struct BodyFileStart);
        char *pkt = malloc(len);
        struct PacketHeader *h = (struct PacketHeader *)pkt;
        h->action_ = PROTOCOL_ACTION_FILE_START;
        h->body_length_ = sizeof(struct BodyFileStart);

        struct BodyFileStart *b = (struct BodyFileStart *)(pkt + sizeof(struct PacketHeader));
        strncpy(b->sender_, "Server", NAME_LENGTH - 1);
        strncpy(b->filename_, task.filename, FILE_NAME_LENGTH - 1);
        b->filesize_ = filesize;

        enqueue_unicast(ctx, task.client_fd, pkt, len);
        free(pkt);
      }

      // FILE_DATA chunks
      char buf[CHUNK_SIZE];
      int seq = 0;
      size_t n;
      while ((n = fread(buf, 1, CHUNK_SIZE, fp)) > 0) {
        int len = sizeof(struct PacketHeader) + sizeof(struct BodyFileData) + n;
        char *pkt = malloc(len);

        struct PacketHeader *h = (struct PacketHeader *)pkt;
        h->action_ = PROTOCOL_ACTION_FILE_DATA;
        h->body_length_ = sizeof(struct BodyFileData) + n;

        struct BodyFileData *b = (struct BodyFileData *)(pkt + sizeof(struct PacketHeader));
        b->sequence_ = seq++;
        b->data_length_ = n;
        memcpy(pkt + sizeof(struct PacketHeader) + sizeof(struct BodyFileData), buf, n);

        enqueue_unicast(ctx, task.client_fd, pkt, len);
        free(pkt);
      }
      fclose(fp);
      printf("[FileWorker %d] Download complete: %s\n", id, task.filename);
      break;
    }
    }
  }

  printf("[FileWorker %d] Stopped\n", id);
  return NULL;
}

//==============================================================================
// Packet Processing
//==============================================================================

void process_packet(ServerContext *ctx, Client *client,
                    struct PacketHeader *header, char *body) {
  switch (header->action_) {
  case PROTOCOL_ACTION_JOIN: {
    if (!body || header->body_length_ < (int)sizeof(struct BodyJoin)) break;
    struct BodyJoin *j = (struct BodyJoin *)body;

    pthread_mutex_lock(&ctx->clients_mutex);
    strncpy(client->username, j->username_, NAME_LENGTH - 1);
    client->username[NAME_LENGTH - 1] = '\0';
    client->state = CLIENT_STATE_JOINED;
    pthread_mutex_unlock(&ctx->clients_mutex);

    printf("[Server] '%s' joined (fd=%d)\n", client->username, client->fd);

    char msg[256];
    snprintf(msg, sizeof(msg), "[Notice] '%s' has joined.", client->username);
    int len;
    char *pkt = make_chat_packet("Server", msg, &len);
    enqueue_broadcast(ctx, pkt, len, client->fd);
    free(pkt);
    break;
  }

  case PROTOCOL_ACTION_CHAT: {
    if (!body || client->state != CLIENT_STATE_JOINED) break;
    struct BodyChat *c = (struct BodyChat *)body;
    char *message = body + sizeof(struct BodyChat);

    printf("[Chat] %s: %s\n", client->username, message);
    strncpy(c->sender_, client->username, NAME_LENGTH - 1);

    int total = sizeof(struct PacketHeader) + header->body_length_;
    char *pkt = malloc(total);
    memcpy(pkt, header, sizeof(struct PacketHeader));
    memcpy(pkt + sizeof(struct PacketHeader), body, header->body_length_);
    enqueue_broadcast(ctx, pkt, total, client->fd);
    free(pkt);
    break;
  }

  case PROTOCOL_ACTION_WHISPER: {
    if (!body || client->state != CLIENT_STATE_JOINED) break;
    struct BodyWhisper *w = (struct BodyWhisper *)body;
    char *message = body + sizeof(struct BodyWhisper);

    printf("[Whisper] %s -> %s: %s\n", client->username, w->target_, message);
    strncpy(w->sender_, client->username, NAME_LENGTH - 1);

    pthread_mutex_lock(&ctx->clients_mutex);
    Client *target = find_client_by_username(ctx, w->target_);
    int target_fd = target ? target->fd : -1;
    pthread_mutex_unlock(&ctx->clients_mutex);

    if (target_fd >= 0) {
      int total = sizeof(struct PacketHeader) + header->body_length_;
      char *pkt = malloc(total);
      memcpy(pkt, header, sizeof(struct PacketHeader));
      memcpy(pkt + sizeof(struct PacketHeader), body, header->body_length_);
      enqueue_unicast(ctx, target_fd, pkt, total);
      free(pkt);
    } else {
      send_error_message(ctx, client->fd, "User not found.");
    }
    break;
  }

  case PROTOCOL_ACTION_LIST_ONLINE: {
    if (client->state != CLIENT_STATE_JOINED) break;
    char list[BUF_SIZE], response[BUF_SIZE];
    int list_len = 0;
    get_online_list(ctx, list, &list_len);

    strcpy(response, "[Online] ");
    int off = strlen(response), pos = 0, cnt = 0;
    while (pos < list_len) {
      if (cnt++ > 0) { response[off++] = ','; response[off++] = ' '; }
      int n = strlen(list + pos);
      memcpy(response + off, list + pos, n);
      off += n;
      pos += n + 1;
    }
    response[off] = '\0';
    send_error_message(ctx, client->fd, response);
    break;
  }

  case PROTOCOL_ACTION_QUIT: {
    printf("[Server] '%s' left (fd=%d)\n", client->username, client->fd);
    if (client->state == CLIENT_STATE_JOINED) {
      char msg[256];
      snprintf(msg, sizeof(msg), "[Notice] '%s' has left.", client->username);
      int len;
      char *pkt = make_chat_packet("Server", msg, &len);
      enqueue_broadcast(ctx, pkt, len, client->fd);
      free(pkt);
    }
    break;
  }

  case PROTOCOL_ACTION_LIST_FILE: {
    if (client->state != CLIENT_STATE_JOINED) break;
    char list[BUF_SIZE], response[BUF_SIZE];
    int list_len = 0;
    get_file_list(ctx, list, &list_len);
    snprintf(response, sizeof(response), "[Files]\n%s", list);
    send_error_message(ctx, client->fd, response);
    break;
  }

  case PROTOCOL_ACTION_FILE_START: {
    if (!body || client->state != CLIENT_STATE_JOINED) break;
    struct BodyFileStart *fs = (struct BodyFileStart *)body;

    printf("[Server] Upload request: %s (%lld bytes) from %s (fd=%d)\n",
           fs->filename_, fs->filesize_, client->username, client->fd);

    if (fs->filesize_ > MAX_FILE_SIZE) {
      char err[256];
      snprintf(err, sizeof(err), "[Error] File too large. (max %dMB)", MAX_FILE_SIZE / 1024 / 1024);
      send_error_message(ctx, client->fd, err);
      break;
    }

    UploadSession *existing = find_upload_session(ctx, client->fd);
    if (existing && existing->active) {
      send_error_message(ctx, client->fd, "[Error] Upload already in progress.");
      break;
    }

    int res = add_file_slot(ctx, fs->filename_, client->fd);
    if (res == -1) { send_error_message(ctx, client->fd, "[Error] File already exists."); break; }
    if (res == -2) { send_error_message(ctx, client->fd, "[Error] Server storage full."); break; }

    UploadSession *s = create_upload_session(ctx, client->fd);
    if (!s) {
      send_error_message(ctx, client->fd, "[Error] Cannot create upload session.");
      remove_file_slot_by_uploader(ctx, client->fd);
      break;
    }

    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s%s", UPLOAD_DIR, fs->filename_);
    FILE *fp = fopen(filepath, "wb");
    if (!fp) {
      send_error_message(ctx, client->fd, "[Error] Cannot create file.");
      remove_file_slot_by_uploader(ctx, client->fd);
      close_upload_session(ctx, client->fd);
      break;
    }

    pthread_mutex_lock(&ctx->sessions_mutex);
    strncpy(s->filename, fs->filename_, FILE_NAME_LENGTH - 1);
    s->filename[FILE_NAME_LENGTH - 1] = '\0';
    s->total_size = fs->filesize_;
    s->received = 0;
    s->fp = fp;
    pthread_mutex_unlock(&ctx->sessions_mutex);

    printf("[Server] Upload session ready: %s (%lld bytes) fd=%d\n", s->filename, s->total_size, client->fd);
    break;
  }

  case PROTOCOL_ACTION_FILE_DATA: {
    if (!body || client->state != CLIENT_STATE_JOINED) break;
    struct BodyFileData *fd_data = (struct BodyFileData *)body;
    char *data = body + sizeof(struct BodyFileData);

    if (fd_data->data_length_ <= 0 || fd_data->data_length_ > CHUNK_SIZE) {
      printf("[Server] Invalid file data length: %d\n", fd_data->data_length_);
      break;
    }

    FileTask task;
    task.type = FILE_TASK_UPLOAD_DATA;
    task.client_fd = client->fd;
    task.seq = fd_data->sequence_;
    task.data = malloc(fd_data->data_length_);
    task.data_len = fd_data->data_length_;
    if (!task.data) { printf("[Server] Memory allocation failed\n"); break; }
    memcpy(task.data, data, fd_data->data_length_);
    enqueue_file_task(ctx, &task);
    break;
  }

  case PROTOCOL_ACTION_REQUEST_FILE: {
    if (!body || client->state != CLIENT_STATE_JOINED) break;
    struct BodyRequestFile *req = (struct BodyRequestFile *)body;

    printf("[Server] Download request: %s from %s\n", req->filename_, client->username);

    pthread_rwlock_rdlock(&ctx->files_rwlock);
    int idx = find_file_slot(ctx, req->filename_);
    int uploading = (idx >= 0) ? ctx->file_slots[idx].uploading : 0;
    pthread_rwlock_unlock(&ctx->files_rwlock);

    if (idx < 0) { send_error_message(ctx, client->fd, "[Error] File not found."); break; }
    if (uploading) { send_error_message(ctx, client->fd, "[Error] File still uploading."); break; }

    FileTask task;
    task.type = FILE_TASK_DOWNLOAD;
    task.client_fd = client->fd;
    task.data = NULL;
    task.data_len = 0;
    strncpy(task.filename, req->filename_, FILE_NAME_LENGTH - 1);
    enqueue_file_task(ctx, &task);
    break;
  }

  default:
    printf("[Server] Unknown action: %d\n", header->action_);
    break;
  }
}

//==============================================================================
// Main Function
//==============================================================================

int main(int argc, char *argv[]) {
  if (argc != 2) {
    printf("Usage: %s <Port>\n", argv[0]);
    return 1;
  }

  int port = atoi(argv[1]);

  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);
  signal(SIGPIPE, SIG_IGN);

  mkdir(UPLOAD_DIR, 0755);

  static ServerContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  pthread_mutex_init(&ctx.clients_mutex, NULL);
  pthread_mutex_init(&ctx.sessions_mutex, NULL);
  pthread_rwlock_init(&ctx.files_rwlock, NULL);

  for (int i = 0; i < MAX_CLIENTS; i++) {
    ctx.clients[i].fd = -1;
    ctx.clients[i].state = CLIENT_STATE_DISCONNECTED;
    ctx.upload_sessions[i].active = 0;
  }
  g_ctx = &ctx;

  ctx.send_queue = queue_create(MESSAGE_QUEUE_SIZE);
  for (int i = 0; i < FILE_WORKER_COUNT; i++)
    ctx.file_queues[i] = file_queue_create(FILE_QUEUE_SIZE);

  ctx.server_fd = create_server_socket(port);
  if (ctx.server_fd < 0) return 1;
  set_nonblocking(ctx.server_fd);

  ctx.epoll_fd = epoll_create1(0);
  struct epoll_event ev;
  ev.events = EPOLLIN;
  ev.data.fd = ctx.server_fd;
  epoll_ctl(ctx.epoll_fd, EPOLL_CTL_ADD, ctx.server_fd, &ev);

  pthread_t worker_tid;
  pthread_create(&worker_tid, NULL, worker_thread, &ctx);

  for (int i = 0; i < FILE_WORKER_COUNT; i++) {
    FileWorkerArg *arg = malloc(sizeof(FileWorkerArg));
    arg->ctx = &ctx;
    arg->id = i;
    pthread_create(&ctx.file_workers[i], NULL, file_worker_thread, arg);
  }

  printf("=== Chat Server Started (port: %d) ===\n", port);
  printf("Message Worker: 1, File Workers: %d\n", FILE_WORKER_COUNT);
  printf("Max file size: %dMB, Upload dir: %s\n", MAX_FILE_SIZE / 1024 / 1024, UPLOAD_DIR);
  printf("Press Ctrl+C to stop\n\n");

  struct epoll_event events[MAX_EVENTS];

  while (g_running) {
    int nfds = epoll_wait(ctx.epoll_fd, events, MAX_EVENTS, 1000);
    if (nfds == -1) {
      if (errno == EINTR) continue;
      perror("epoll_wait");
      break;
    }

    for (int i = 0; i < nfds; i++) {
      if (events[i].data.fd == ctx.server_fd) {
        // New connection
        struct sockaddr_in addr;
        socklen_t len = sizeof(addr);
        int cfd = accept(ctx.server_fd, (struct sockaddr *)&addr, &len);
        if (cfd == -1) continue;

        set_nonblocking(cfd);
        if (add_client(&ctx, cfd) < 0) {
          printf("[Server] Max clients reached\n");
          close(cfd);
          continue;
        }

        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = cfd;
        epoll_ctl(ctx.epoll_fd, EPOLL_CTL_ADD, cfd, &ev);
        printf("[Server] New connection: %s:%d (fd=%d)\n",
               inet_ntoa(addr.sin_addr), ntohs(addr.sin_port), cfd);

      } else {
        // Client data
        int cfd = events[i].data.fd;
        Client *client = find_client_by_fd(&ctx, cfd);
        if (!client) continue;

        while (1) {
          int space = BUF_SIZE - client->recv_len;
          if (space <= 0) {
            printf("[Server] Buffer full fd=%d\n", cfd);
            goto disconnect;
          }

          int n = recv(cfd, client->recv_buffer + client->recv_len, space, 0);
          if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            goto disconnect;
          }
          if (n == 0) goto disconnect;

          client->recv_len += n;

          // Parse packets
          while (client->recv_len >= (int)sizeof(struct PacketHeader)) {
            struct PacketHeader *h = (struct PacketHeader *)client->recv_buffer;
            int pkt_len = sizeof(struct PacketHeader) + h->body_length_;

            if (pkt_len > BUF_SIZE) {
              printf("[Server] Packet too large fd=%d\n", cfd);
              goto disconnect;
            }
            if (client->recv_len < pkt_len) break;

            char *body = (h->body_length_ > 0) ? client->recv_buffer + sizeof(struct PacketHeader) : NULL;
            int action = h->action_;
            process_packet(&ctx, client, h, body);

            int rem = client->recv_len - pkt_len;
            if (rem > 0) memmove(client->recv_buffer, client->recv_buffer + pkt_len, rem);
            client->recv_len = rem;

            if (action == PROTOCOL_ACTION_QUIT) goto disconnect;
          }
        }
        continue;

      disconnect:
        printf("[Server] Disconnected: %s (fd=%d)\n",
               client->username[0] ? client->username : "anonymous", cfd);
        remove_file_slot_by_uploader(&ctx, cfd);
        close_upload_session(&ctx, cfd);

        if (client->state == CLIENT_STATE_JOINED) {
          char msg[256];
          snprintf(msg, sizeof(msg), "[Notice] '%s' has left.", client->username);
          int len;
          char *pkt = make_chat_packet("Server", msg, &len);
          enqueue_broadcast(&ctx, pkt, len, cfd);
          free(pkt);
        }

        epoll_ctl(ctx.epoll_fd, EPOLL_CTL_DEL, cfd, NULL);
        close(cfd);
        remove_client(&ctx, cfd);
      }
    }
  }

  // Cleanup
  printf("\n[Server] Shutting down...\n");
  queue_shutdown(ctx.send_queue);
  pthread_join(worker_tid, NULL);

  for (int i = 0; i < FILE_WORKER_COUNT; i++)
    file_queue_shutdown(ctx.file_queues[i]);
  for (int i = 0; i < FILE_WORKER_COUNT; i++) {
    pthread_join(ctx.file_workers[i], NULL);
    file_queue_destroy(ctx.file_queues[i]);
  }

  for (int i = 0; i < MAX_CLIENTS; i++)
    if (ctx.clients[i].fd >= 0) close(ctx.clients[i].fd);

  queue_destroy(ctx.send_queue);
  close(ctx.epoll_fd);
  close(ctx.server_fd);
  pthread_mutex_destroy(&ctx.clients_mutex);
  pthread_mutex_destroy(&ctx.sessions_mutex);
  pthread_rwlock_destroy(&ctx.files_rwlock);

  printf("[Server] Shutdown complete\n");
  return 0;
}
