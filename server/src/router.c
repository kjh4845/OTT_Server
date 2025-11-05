#include "router.h"

#include <stdio.h>
#include <string.h>

#include "http.h"

static const route_entry_t *g_routes = NULL;
static size_t g_route_count = 0;

void router_set_routes(const route_entry_t *routes, size_t count) {
    g_routes = routes;
    g_route_count = count;
}

const char *router_get_param(const request_ctx_t *ctx, const char *name) {
    for (size_t i = 0; i < ctx->param_count; ++i) {
        if (strcmp(ctx->params[i].key, name) == 0) {
            return ctx->params[i].value;
        }
    }
    return NULL;
}

static int segment_match(const char *route_seg, const char *path_seg, request_ctx_t *ctx) {
    if (route_seg[0] == ':' && ctx) {
        if (ctx->param_count < sizeof(ctx->params) / sizeof(ctx->params[0])) {
            strncpy(ctx->params[ctx->param_count].key, route_seg + 1,
                    sizeof(ctx->params[ctx->param_count].key) - 1);
            strncpy(ctx->params[ctx->param_count].value, path_seg,
                    sizeof(ctx->params[ctx->param_count].value) - 1);
            ctx->param_count++;
            return 1;
        }
        return 0;
    }
    return strcmp(route_seg, path_seg) == 0;
}

static int path_matches(const char *route_path, const char *request_path, request_ctx_t *ctx) {
    char route_buf[512];
    char path_buf[512];
    strncpy(route_buf, route_path, sizeof(route_buf) - 1);
    route_buf[sizeof(route_buf) - 1] = '\0';
    strncpy(path_buf, request_path, sizeof(path_buf) - 1);
    path_buf[sizeof(path_buf) - 1] = '\0';

    char *route_save = NULL;
    char *path_save = NULL;
    char *route_seg = strtok_r(route_buf, "/", &route_save);
    char *path_seg = strtok_r(path_buf, "/", &path_save);
    while (route_seg || path_seg) {
        if (!route_seg || !path_seg) {
            return 0;
        }
        if (!segment_match(route_seg, path_seg, ctx)) {
            return 0;
        }
        route_seg = strtok_r(NULL, "/", &route_save);
        path_seg = strtok_r(NULL, "/", &path_save);
    }
    return 1;
}

void router_handle(request_ctx_t *ctx) {
    ctx->param_count = 0;
    for (size_t i = 0; i < g_route_count; ++i) {
        request_ctx_t temp_ctx = *ctx;
        temp_ctx.param_count = 0;
        if (g_routes[i].method == ctx->request->method &&
            path_matches(g_routes[i].path, ctx->request->path, &temp_ctx)) {
            *ctx = temp_ctx;
            g_routes[i].handler(ctx);
            return;
        }
    }
    const char body[] = "{\"error\":\"Not Found\"}";
    http_send_response(ctx->client_fd, 404, "Not Found", "application/json",
                       body, sizeof(body) - 1, ctx->server->security_headers);
}

int router_send_json(request_ctx_t *ctx, int status, const char *json_body, const char *extra_headers) {
    if (!json_body) {
        json_body = "{}";
    }
    size_t json_len = strlen(json_body);
    const char *base = ctx->server->security_headers[0] ? ctx->server->security_headers : "";
    const char *headers = base;
    char combined[1024];
    if (extra_headers && *extra_headers) {
        if (snprintf(combined, sizeof(combined), "%s%s", base, extra_headers) >= (int)sizeof(combined)) {
            return -1;
        }
        headers = combined;
    }
    return http_send_response(ctx->client_fd, status, http_status_text(status), "application/json",
                              json_body, json_len, headers);
}

int router_send_json_error(request_ctx_t *ctx, int status, const char *message) {
    char body[512];
    if (!message) {
        message = "error";
    }
    if (snprintf(body, sizeof(body), "{\"error\":\"%s\"}", message) >= (int)sizeof(body)) {
        return -1;
    }
    return router_send_json(ctx, status, body, NULL);
}
