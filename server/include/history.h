#ifndef HISTORY_H
#define HISTORY_H

// 시청 기록 관련 API 핸들러 선언

#include "router.h"

void history_handle_get(request_ctx_t *ctx);
void history_handle_update(request_ctx_t *ctx);

#endif
