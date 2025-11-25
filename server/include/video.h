#ifndef VIDEO_H
#define VIDEO_H

// 비디오 목록/스트리밍/썸네일 관련 엔드포인트 선언

#include "router.h"

int video_initialize(server_ctx_t *server);
void video_handle_list(request_ctx_t *ctx);
void video_handle_stream(request_ctx_t *ctx);
void video_handle_thumbnail(request_ctx_t *ctx);
void video_shutdown(void);

#endif
