#ifndef FFMPEG_H
#define FFMPEG_H

// 외부 ffmpeg 실행을 통한 썸네일 생성 API

#include <stddef.h>

#include "server.h"

int ffmpeg_initialize(server_ctx_t *server);
int ffmpeg_ensure_thumbnail(server_ctx_t *server, int video_id,
                            const char *video_path, char *thumb_path,
                            size_t thumb_path_len);

#endif
