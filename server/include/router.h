#ifndef ROUTER_H
#define ROUTER_H

#include "http.h"
#include "server.h"

typedef struct request_ctx {
    server_ctx_t *server;
    int client_fd;
    http_request_t *request;
    int authenticated;
    int user_id;
    char username[64];
    char session_token[128];
    struct route_param {
        char key[32];
        char value[256];
    } params[8];
    size_t param_count;
} request_ctx_t;

typedef void (*route_handler_fn)(request_ctx_t *ctx);

typedef struct {
    http_method_t method;
    const char *path;
    route_handler_fn handler;
} route_entry_t;

void router_handle(request_ctx_t *ctx);
void router_set_routes(const route_entry_t *routes, size_t count);
const char *router_get_param(const request_ctx_t *ctx, const char *name);
int router_send_json(request_ctx_t *ctx, int status, const char *json_body, const char *extra_headers);
int router_send_json_error(request_ctx_t *ctx, int status, const char *message);

#endif
