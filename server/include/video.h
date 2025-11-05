#ifndef VIDEO_H
#define VIDEO_H

#include "router.h"

int video_initialize(server_ctx_t *server);
void video_handle_list(request_ctx_t *ctx);
void video_handle_stream(request_ctx_t *ctx);
void video_handle_thumbnail(request_ctx_t *ctx);

#endif
