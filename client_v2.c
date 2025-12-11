#include <ao/ao.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <libgen.h>
#include <mpg123.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vlc/vlc.h>

#include "client_v2.h"

int sock;
char myUsername[NAME_LENGTH];
pthread_mutex_t sockMutex = PTHREAD_MUTEX_INITIALIZER;
DownloadSession downloadSession = {0};
pthread_mutex_t downloadMutex = PTHREAD_MUTEX_INITIALIZER;
MediaPlayer mediaPlayer = {0};
pthread_mutex_t mediaMutex = PTHREAD_MUTEX_INITIALIZER;

// mp3/mp4 확장자인지 확인
int isMediaFile(const char *filename) {
  return isMP3File(filename) || isMP4File(filename);
}

// mp3 확장자인지 확인
int isMP3File(const char *filename) {
  const char *dot = strrchr(filename, '.');
  if (!dot) return 0;
  
  char ext[16];
  strncpy(ext, dot + 1, sizeof(ext) - 1);
  ext[sizeof(ext) - 1] = '\0';
  
  for (int i = 0; ext[i]; i++) {
    ext[i] = tolower(ext[i]);
  }
  
  return (strcmp(ext, "mp3") == 0);
}

// mp4 확장자인지 확인
int isMP4File(const char *filename) {
  const char *dot = strrchr(filename, '.');
  if (!dot) return 0;
  
  char ext[16];
  strncpy(ext, dot + 1, sizeof(ext) - 1);
  ext[sizeof(ext) - 1] = '\0';
  
  for (int i = 0; ext[i]; i++) {
    ext[i] = tolower(ext[i]);
  }
  
  return (strcmp(ext, "mp4") == 0);
}

// MP3 재생 스레드
void *playMP3Thread(void *arg) {
  char *filepath = (char *)arg;
  mpg123_handle *mh = NULL;
  ao_device *dev = NULL;
  int err;
  
  // mpg123 초기화
  mpg123_init();
  mh = mpg123_new(NULL, &err);
  if (!mh) {
    printf("\n[Error] Failed to create mpg123 handle\n> ");
    fflush(stdout);
    goto cleanup;
  }
  
  if (mpg123_open(mh, filepath) != MPG123_OK) {
    printf("\n[Error] Cannot open file: %s\n> ", filepath);
    fflush(stdout);
    goto cleanup;
  }
  
  // 오디오 포맷 가져오기
  long rate;
  int channels, encoding;
  if (mpg123_getformat(mh, &rate, &channels, &encoding) != MPG123_OK) {
    printf("\n[Error] Cannot get audio format\n> ");
    fflush(stdout);
    goto cleanup;
  }
  
  // 포맷 고정
  mpg123_format_none(mh);
  mpg123_format(mh, rate, channels, encoding);
  
  // ao 초기화
  ao_initialize();
  
  ao_sample_format format;
  memset(&format, 0, sizeof(format));
  format.bits = mpg123_encsize(encoding) * 8;
  format.rate = rate;
  format.channels = channels;
  format.byte_format = AO_FMT_NATIVE;
  
  int driver = ao_default_driver_id();
  dev = ao_open_live(driver, &format, NULL);
  if (!dev) {
    printf("\n[Error] Cannot open audio device\n> ");
    fflush(stdout);
    goto cleanup;
  }
  
  // 재생 시작 메시지
  pthread_mutex_lock(&mediaMutex);
  mediaPlayer.playing = 1;
  pthread_mutex_unlock(&mediaMutex);
  
  printf("\n[Playing] %s\n> ", mediaPlayer.filename);
  fflush(stdout);
  
  // 재생 루프
  size_t buffer_size = mpg123_outblock(mh);
  unsigned char *buffer = malloc(buffer_size);
  size_t done;
  
  while (1) {
    pthread_mutex_lock(&mediaMutex);
    int should_stop = mediaPlayer.stop_requested;
    pthread_mutex_unlock(&mediaMutex);
    
    if (should_stop) break;
    
    int result = mpg123_read(mh, buffer, buffer_size, &done);
    if (result == MPG123_DONE || done == 0) break;
    if (result != MPG123_OK && result != MPG123_NEW_FORMAT) break;
    
    ao_play(dev, (char *)buffer, done);
  }
  
  free(buffer);
  
cleanup:
  if (dev) ao_close(dev);
  if (mh) {
    mpg123_close(mh);
    mpg123_delete(mh);
  }
  mpg123_exit();
  ao_shutdown();
  
  pthread_mutex_lock(&mediaMutex);
  int was_stopped = mediaPlayer.stop_requested;
  mediaPlayer.playing = 0;
  mediaPlayer.stop_requested = 0;
  mediaPlayer.filename[0] = '\0';
  pthread_mutex_unlock(&mediaMutex);
  
  if (was_stopped) {
    printf("\n[Stopped]\n> ");
  } else {
    printf("\n[Finished] Playback complete\n> ");
  }
  fflush(stdout);
  
  free(filepath);
  return NULL;
}

// MP4 재생 스레드
void *playMP4Thread(void *arg) {
  char *filepath = (char *)arg;
  
  libvlc_instance_t *inst = libvlc_new(0, NULL);
  if (!inst) {
    printf("\n[Error] Failed to create VLC instance\n> ");
    fflush(stdout);
    free(filepath);
    return NULL;
  }
  
  libvlc_media_t *media = libvlc_media_new_path(inst, filepath);
  if (!media) {
    printf("\n[Error] Cannot open file: %s\n> ", filepath);
    fflush(stdout);
    libvlc_release(inst);
    free(filepath);
    return NULL;
  }
  
  libvlc_media_player_t *mp = libvlc_media_player_new_from_media(media);
  libvlc_media_release(media);
  
  if (!mp) {
    printf("\n[Error] Failed to create media player\n> ");
    fflush(stdout);
    libvlc_release(inst);
    free(filepath);
    return NULL;
  }
  
  // VLC 인스턴스 저장
  pthread_mutex_lock(&mediaMutex);
  mediaPlayer.vlc_inst = inst;
  mediaPlayer.vlc_player = mp;
  mediaPlayer.playing = 1;
  pthread_mutex_unlock(&mediaMutex);
  
  printf("\n[Playing] %s (VLC window)\n> ", mediaPlayer.filename);
  fflush(stdout);
  
  // 재생 시작
  libvlc_media_player_play(mp);
  
  // 재생 완료 또는 stop 요청까지 대기
  while (1) {
    pthread_mutex_lock(&mediaMutex);
    int should_stop = mediaPlayer.stop_requested;
    pthread_mutex_unlock(&mediaMutex);
    
    if (should_stop) break;
    
    libvlc_state_t state = libvlc_media_player_get_state(mp);
    if (state == libvlc_Ended || state == libvlc_Error || state == libvlc_Stopped) {
      break;
    }
    
    usleep(100000);  // 100ms
  }
  
  // 정리
  libvlc_media_player_stop(mp);
  libvlc_media_player_release(mp);
  libvlc_release(inst);
  
  pthread_mutex_lock(&mediaMutex);
  int was_stopped = mediaPlayer.stop_requested;
  mediaPlayer.playing = 0;
  mediaPlayer.stop_requested = 0;
  mediaPlayer.vlc_inst = NULL;
  mediaPlayer.vlc_player = NULL;
  mediaPlayer.filename[0] = '\0';
  pthread_mutex_unlock(&mediaMutex);
  
  if (was_stopped) {
    printf("\n[Stopped]\n> ");
  } else {
    printf("\n[Finished] Playback complete\n> ");
  }
  fflush(stdout);
  
  free(filepath);
  return NULL;
}

// 미디어 재생 시작
void playMedia(const char *filepath) {
  pthread_mutex_lock(&mediaMutex);
  if (mediaPlayer.playing) {
    pthread_mutex_unlock(&mediaMutex);
    printf("Already playing. Use /stop first.\n");
    return;
  }
  
  // 파일명 저장
  const char *filename = strrchr(filepath, '/');
  if (filename) filename++;
  else filename = filepath;
  strncpy(mediaPlayer.filename, filename, FILE_NAME_LENGTH - 1);
  mediaPlayer.filename[FILE_NAME_LENGTH - 1] = '\0';
  
  mediaPlayer.stop_requested = 0;
  pthread_mutex_unlock(&mediaMutex);
  
  // filepath 복사 (스레드에서 사용)
  char *path_copy = strdup(filepath);
  
  if (isMP3File(filepath)) {
    pthread_create(&mediaPlayer.thread, NULL, playMP3Thread, path_copy);
    pthread_detach(mediaPlayer.thread);
  } else if (isMP4File(filepath)) {
    pthread_create(&mediaPlayer.thread, NULL, playMP4Thread, path_copy);
    pthread_detach(mediaPlayer.thread);
  } else {
    free(path_copy);
    printf("Unsupported format\n");
  }
}

// 미디어 재생 중단
void stopMedia(void) {
  pthread_mutex_lock(&mediaMutex);
  if (!mediaPlayer.playing) {
    pthread_mutex_unlock(&mediaMutex);
    printf("Nothing is playing.\n");
    return;
  }
  
  mediaPlayer.stop_requested = 1;
  
  // VLC인 경우 즉시 중단
  if (mediaPlayer.vlc_player) {
    libvlc_media_player_stop((libvlc_media_player_t *)mediaPlayer.vlc_player);
  }
  
  pthread_mutex_unlock(&mediaMutex);
}

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

void sendFileRequest(const char *filename, int play_flag) {
  pthread_mutex_lock(&downloadMutex);
  if (downloadSession.active) {
    pthread_mutex_unlock(&downloadMutex);
    printf("Download already in progress\n");
    return;
  }
  downloadSession.play_after_download = play_flag;
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
    
    // 미디어 재생 플래그가 설정된 경우 재생 시작
    if (downloadSession.play_after_download) {
      char filepath[512];
      snprintf(filepath, sizeof(filepath), "%s%s", DOWNLOAD_DIR, downloadSession.filename);
      playMedia(filepath);
      downloadSession.play_after_download = 0;
    }
    
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
  printf("  /play <file>       - Download & play media (mp3/mp4)\n");
  printf("  /stop              - Stop current playback\n");
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

  printf("=== Connected to chat server (V2 - Media Player) ===\n");
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
      stopMedia();  // 재생 중이면 중단
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
      if (*name) sendFileRequest(name, 0);
      else printf("Usage: /download <filename>\n");
    } else if (strncmp(input, "/play ", 6) == 0) {
      char *name = input + 6;
      while (*name == ' ') name++;
      if (*name) {
        if (isMediaFile(name)) {
          sendFileRequest(name, 1);
        } else {
          printf("Only mp3/mp4 files can be played. Use /download instead.\n");
        }
      } else {
        printf("Usage: /play <filename.mp3|mp4>\n");
      }
    } else if (strcmp(input, "/stop") == 0) {
      stopMedia();
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
