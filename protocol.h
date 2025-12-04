#ifndef PROTOCOL_H
#define PROTOCOL_H

#pragma pack(push, 1)

#define NAME_LENGTH 32
#define FILE_NAME_LENGTH 256
#define CHUNK_SIZE 4096

// ACTION_QUIT has no body.
// ACTION_LIST_ONLINE has no body.
// ACTION_LIST_FILE has no body.

#define PROTOCOL_ACTION_JOIN 0
#define PROTOCOL_ACTION_CHAT 1
#define PROTOCOL_ACTION_QUIT 2
#define PROTOCOL_ACTION_LIST_ONLINE 3
#define PROTOCOL_ACTION_WHISPER 4
#define PROTOCOL_ACTION_LIST_FILE 5
#define PROTOCOL_ACTION_REQUEST_FILE 6
#define PROTOCOL_ACTION_FILE_START 7
#define PROTOCOL_ACTION_FILE_DATA 8

struct PacketHeader {
  int action_;
  int body_length_;
};

struct BodyJoin {
  char username_[NAME_LENGTH];
};

struct BodyChat {
  char sender_[NAME_LENGTH];
  char message_[];
};

struct BodyWhisper {
  char sender_[NAME_LENGTH];
  char target_[NAME_LENGTH];
  char message_[];
};

struct BodyRequestFile {
  char filename_[FILE_NAME_LENGTH];
};

struct BodyFileStart {
  char sender_[NAME_LENGTH];
  char filename_[FILE_NAME_LENGTH];
  long long filesize_;
};

struct BodyFileData {
  int sequence_;
};

#pragma pack(pop)

#endif // PROTOCOL_H
