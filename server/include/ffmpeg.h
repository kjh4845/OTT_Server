#ifndef FFMPEG_H
#define FFMPEG_H

#include <stddef.h>

#include "server.h"

int ffmpeg_initialize(server_ctx_t *server);
int ffmpeg_ensure_thumbnail(server_ctx_t *server, int video_id,
                            const char *video_path, char *thumb_path,
                            size_t thumb_path_len);

#endif
