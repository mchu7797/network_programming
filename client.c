#include <arpa/inet.h>
#include <libgen.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "client.h"

int sock;
char myUsername[NAME_LENGTH];
pthread_mutex_t sockMutex = PTHREAD_MUTEX_INITIALIZER;
DownloadSession downloadSession = {0};
pthread_mutex_t downloadMutex = PTHREAD_MUTEX_INITIALIZER;

int recvAll(int sock, void *buffer, int length) {
  int total = 0;
  char *ptr = buffer;
  while (total < length) {
    int n = recv(sock, ptr + total, length - total, 0);
    if (n <= 0) return n;
    total += n;
  }
  return total;
}

int sendAll(int sock, void *buffer, int length) {
  pthread_mutex_lock(&sockMutex);
  int total = 0;
  char *ptr = buffer;
  while (total < length) {
    int n = send(sock, ptr + total, length - total, MSG_NOSIGNAL);
    if (n <= 0) {
      pthread_mutex_unlock(&sockMutex);
      return n;
    }
    total += n;
  }
  pthread_mutex_unlock(&sockMutex);
  return total;
}

void sendJoin(const char *username) {
  int len = sizeof(struct PacketHeader) + sizeof(struct BodyJoin);
  char *pkt = malloc(len);
  struct PacketHeader *h = (struct PacketHeader *)pkt;
  h->action_ = PROTOCOL_ACTION_JOIN;
  h->body_length_ = sizeof(struct BodyJoin);
  struct BodyJoin *b = (struct BodyJoin *)(pkt + sizeof(struct PacketHeader));
  strncpy(b->username_, username, NAME_LENGTH - 1);
  b->username_[NAME_LENGTH - 1] = '\0';
  sendAll(sock, pkt, len);
  free(pkt);
}

void sendChat(const char *message) {
  int msg_len = strlen(message) + 1;
  int len = sizeof(struct PacketHeader) + sizeof(struct BodyChat) + msg_len;
  char *pkt = malloc(len);
  struct PacketHeader *h = (struct PacketHeader *)pkt;
  h->action_ = PROTOCOL_ACTION_CHAT;
  h->body_length_ = sizeof(struct BodyChat) + msg_len;
  struct BodyChat *b = (struct BodyChat *)(pkt + sizeof(struct PacketHeader));
  strncpy(b->sender_, myUsername, NAME_LENGTH - 1);
  b->sender_[NAME_LENGTH - 1] = '\0';
  strcpy(pkt + sizeof(struct PacketHeader) + sizeof(struct BodyChat), message);
  sendAll(sock, pkt, len);
  free(pkt);
}

void sendWhisper(const char *target, const char *message) {
  int msg_len = strlen(message) + 1;
  int len = sizeof(struct PacketHeader) + sizeof(struct BodyWhisper) + msg_len;
  char *pkt = malloc(len);
  struct PacketHeader *h = (struct PacketHeader *)pkt;
  h->action_ = PROTOCOL_ACTION_WHISPER;
  h->body_length_ = sizeof(struct BodyWhisper) + msg_len;
  struct BodyWhisper *b = (struct BodyWhisper *)(pkt + sizeof(struct PacketHeader));
  strncpy(b->sender_, myUsername, NAME_LENGTH - 1);
  b->sender_[NAME_LENGTH - 1] = '\0';
  strncpy(b->target_, target, NAME_LENGTH - 1);
  b->target_[NAME_LENGTH - 1] = '\0';
  strcpy(pkt + sizeof(struct PacketHeader) + sizeof(struct BodyWhisper), message);
  sendAll(sock, pkt, len);
  free(pkt);
}

void sendListOnline(void) {
  struct PacketHeader h;
  h.action_ = PROTOCOL_ACTION_LIST_ONLINE;
  h.body_length_ = 0;
  sendAll(sock, &h, sizeof(h));
}

void sendQuit(void) {
  struct PacketHeader h;
  h.action_ = PROTOCOL_ACTION_QUIT;
  h.body_length_ = 0;
  sendAll(sock, &h, sizeof(h));
}

void sendListFile(void) {
  struct PacketHeader h;
  h.action_ = PROTOCOL_ACTION_LIST_FILE;
  h.body_length_ = 0;
  sendAll(sock, &h, sizeof(h));
}

void sendFileUpload(const char *filepath) {
  FILE *fp = fopen(filepath, "rb");
  if (!fp) {
    printf("Cannot open file: %s\n", filepath);
    return;
  }

  fseek(fp, 0, SEEK_END);
  long long filesize = ftell(fp);
  fseek(fp, 0, SEEK_SET);

  if (filesize > MAX_FILE_SIZE) {
    printf("File too large (max %dMB)\n", MAX_FILE_SIZE / 1024 / 1024);
    fclose(fp);
    return;
  }
  if (filesize == 0) {
    printf("Cannot upload empty file\n");
    fclose(fp);
    return;
  }

  char path_copy[512];
  strncpy(path_copy, filepath, sizeof(path_copy) - 1);
  char *filename = basename(path_copy);
  printf("Upload started: %s (%lld bytes)\n", filename, filesize);

  int start_len = sizeof(struct PacketHeader) + sizeof(struct BodyFileStart);
  char *start_pkt = calloc(1, start_len);
  struct PacketHeader *sh = (struct PacketHeader *)start_pkt;
  sh->action_ = PROTOCOL_ACTION_FILE_START;
  sh->body_length_ = sizeof(struct BodyFileStart);
  struct BodyFileStart *sb = (struct BodyFileStart *)(start_pkt + sizeof(struct PacketHeader));
  strncpy(sb->sender_, myUsername, NAME_LENGTH - 1);
  strncpy(sb->filename_, filename, FILE_NAME_LENGTH - 1);
  sb->filesize_ = filesize;
  sendAll(sock, start_pkt, start_len);
  free(start_pkt);

  char buf[CHUNK_SIZE];
  int seq = 0;
  size_t n;
  long long total_sent = 0;

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
    sendAll(sock, pkt, len);
    free(pkt);
    total_sent += n;
  }

  fclose(fp);
  printf("Upload sent: %s (%lld bytes)\n", filename, total_sent);
}

void sendFileRequest(const char *filename) {
  pthread_mutex_lock(&downloadMutex);
  if (downloadSession.active) {
    pthread_mutex_unlock(&downloadMutex);
    printf("Download already in progress\n");
    return;
  }
  pthread_mutex_unlock(&downloadMutex);

  int len = sizeof(struct PacketHeader) + sizeof(struct BodyRequestFile);
  char *pkt = malloc(len);
  struct PacketHeader *h = (struct PacketHeader *)pkt;
  h->action_ = PROTOCOL_ACTION_REQUEST_FILE;
  h->body_length_ = sizeof(struct BodyRequestFile);
  struct BodyRequestFile *b = (struct BodyRequestFile *)(pkt + sizeof(struct PacketHeader));
  strncpy(b->filename_, filename, FILE_NAME_LENGTH - 1);
  b->filename_[FILE_NAME_LENGTH - 1] = '\0';
  sendAll(sock, pkt, len);
  free(pkt);
  printf("Download requested: %s\n", filename);
}

void handleFileStart(struct BodyFileStart *body) {
  pthread_mutex_lock(&downloadMutex);
  if (downloadSession.active) {
    pthread_mutex_unlock(&downloadMutex);
    printf("\nDownload already in progress\n> ");
    fflush(stdout);
    return;
  }

  char filepath[512];
  snprintf(filepath, sizeof(filepath), "%s%s", DOWNLOAD_DIR, body->filename_);
  downloadSession.fp = fopen(filepath, "wb");
  if (!downloadSession.fp) {
    pthread_mutex_unlock(&downloadMutex);
    printf("\nCannot create file: %s\n> ", filepath);
    fflush(stdout);
    return;
  }

  downloadSession.active = 1;
  strncpy(downloadSession.filename, body->filename_, FILE_NAME_LENGTH - 1);
  downloadSession.total_size = body->filesize_;
  downloadSession.received = 0;
  pthread_mutex_unlock(&downloadMutex);
  printf("\nDownload started: %s (%lld bytes)\n> ", body->filename_, body->filesize_);
  fflush(stdout);
}

void handleFileData(struct BodyFileData *body, int data_len) {
  pthread_mutex_lock(&downloadMutex);
  if (!downloadSession.active || !downloadSession.fp) {
    pthread_mutex_unlock(&downloadMutex);
    return;
  }

  char *data = (char *)body + sizeof(struct BodyFileData);
  fwrite(data, 1, data_len, downloadSession.fp);
  downloadSession.received += data_len;

  if (downloadSession.received >= downloadSession.total_size) {
    fclose(downloadSession.fp);
    downloadSession.fp = NULL;
    printf("\nDownload complete: %s (%lld bytes)\n> ",
           downloadSession.filename, downloadSession.received);
    fflush(stdout);
    downloadSession.active = 0;
  }
  pthread_mutex_unlock(&downloadMutex);
}

void *receiveThread(void *arg) {
  (void)arg;
  struct PacketHeader header;
  char *body = NULL;

  while (1) {
    if (recvAll(sock, &header, sizeof(header)) <= 0) {
      printf("\nDisconnected from server\n");
      exit(0);
    }

    if (header.body_length_ > 0) {
      body = malloc(header.body_length_);
      if (recvAll(sock, body, header.body_length_) <= 0) {
        free(body);
        printf("\nDisconnected from server\n");
        exit(0);
      }
    }

    switch (header.action_) {
    case PROTOCOL_ACTION_CHAT:
      if (body) {
        struct BodyChat *c = (struct BodyChat *)body;
        char *msg = body + sizeof(struct BodyChat);
        printf("\n[%s] %s\n> ", c->sender_, msg);
        fflush(stdout);
      }
      break;
    case PROTOCOL_ACTION_WHISPER:
      if (body) {
        struct BodyWhisper *w = (struct BodyWhisper *)body;
        char *msg = body + sizeof(struct BodyWhisper);
        printf("\n[Whisper from %s] %s\n> ", w->sender_, msg);
        fflush(stdout);
      }
      break;
    case PROTOCOL_ACTION_FILE_START:
      if (body) handleFileStart((struct BodyFileStart *)body);
      break;
    case PROTOCOL_ACTION_FILE_DATA:
      if (body) {
        int data_len = header.body_length_ - sizeof(struct BodyFileData);
        handleFileData((struct BodyFileData *)body, data_len);
      }
      break;
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

void printHelp(void) {
  printf("\n=== Commands ===\n");
  printf("  /help              - Show this help\n");
  printf("  /list              - List online users\n");
  printf("  /w <name> <msg>    - Whisper to user\n");
  printf("  /files             - List server files\n");
  printf("  /upload <path>     - Upload file\n");
  printf("  /download <file>   - Download file\n");
  printf("  /quit              - Exit chat\n");
  printf("  (Other text is sent as chat)\n\n");
}

int main(int argc, char *argv[]) {
  if (argc != 3) {
    printf("Usage: %s <IP> <Port>\n", argv[0]);
    return 1;
  }

  signal(SIGPIPE, SIG_IGN);
  mkdir(DOWNLOAD_DIR, 0755);

  sock = socket(PF_INET, SOCK_STREAM, 0);
  if (sock == -1) {
    perror("socket");
    return 1;
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = inet_addr(argv[1]);
  addr.sin_port = htons(atoi(argv[2]));

  if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
    perror("connect");
    close(sock);
    return 1;
  }

  printf("=== Connected to chat server ===\n");
  printf("Enter username: ");
  fgets(myUsername, NAME_LENGTH, stdin);
  myUsername[strcspn(myUsername, "\n")] = '\0';

  sendJoin(myUsername);

  pthread_t tid;
  pthread_create(&tid, NULL, receiveThread, NULL);
  pthread_detach(tid);

  printHelp();

  char input[BUF_SIZE];
  while (1) {
    printf("> ");
    fflush(stdout);
    if (!fgets(input, BUF_SIZE, stdin)) break;
    input[strcspn(input, "\n")] = '\0';
    if (strlen(input) == 0) continue;

    if (strcmp(input, "/quit") == 0) {
      sendQuit();
      break;
    } else if (strcmp(input, "/help") == 0) {
      printHelp();
    } else if (strcmp(input, "/list") == 0) {
      sendListOnline();
    } else if (strcmp(input, "/files") == 0) {
      sendListFile();
    } else if (strncmp(input, "/upload ", 8) == 0) {
      char *path = input + 8;
      while (*path == ' ') path++;
      if (*path) sendFileUpload(path);
      else printf("Usage: /upload <filepath>\n");
    } else if (strncmp(input, "/download ", 10) == 0) {
      char *name = input + 10;
      while (*name == ' ') name++;
      if (*name) sendFileRequest(name);
      else printf("Usage: /download <filename>\n");
    } else if (strncmp(input, "/w ", 3) == 0) {
      char *target = strtok(input + 3, " ");
      char *msg = strtok(NULL, "");
      if (target && msg) sendWhisper(target, msg);
      else printf("Usage: /w <username> <message>\n");
    } else {
      sendChat(input);
    }
  }

  close(sock);
  printf("Disconnected\n");
  return 0;
}
