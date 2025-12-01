#ifndef ROUTER_H
#define ROUTER_H

// HTTP 요청 컨텍스트와 라우터 인터페이스 정의

#include "http.h"
#include "server.h"

typedef struct request_ctx {
    server_ctx_t *server;      // 전역 상태 접근
    int client_fd;             // 응답을 돌려줄 소켓 FD
    http_request_t *request;   // 파싱된 HTTP 요청
    int authenticated;         // auth_authenticate_request 결과
    int user_id;
    char username[64];
    char session_token[128];
    struct route_param {
        char key[32];
        char value[256];
    } params[8];               // ":id"와 같은 경로 파라미터 저장소
    size_t param_count;
} request_ctx_t;

typedef void (*route_handler_fn)(request_ctx_t *ctx);

typedef struct {
    http_method_t method;
    const char *path;        // "/api/videos/:id" 형태의 패턴
    route_handler_fn handler;
} route_entry_t;

void router_handle(request_ctx_t *ctx);
void router_set_routes(const route_entry_t *routes, size_t count);
const char *router_get_param(const request_ctx_t *ctx, const char *name);
int router_send_json(request_ctx_t *ctx, int status, const char *json_body, const char *extra_headers);
int router_send_json_error(request_ctx_t *ctx, int status, const char *message);

#endif
