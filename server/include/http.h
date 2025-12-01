#ifndef HTTP_H
#define HTTP_H

// HTTP 파싱/직접 응답 송신을 위한 자료구조와 함수 정의

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef enum {
    HTTP_GET,
    HTTP_POST,
    HTTP_PUT,
    HTTP_DELETE,
    HTTP_OPTIONS,
    HTTP_UNKNOWN
} http_method_t;

typedef struct {
    char *name;
    char *value;
} http_header_t;

typedef struct {
    http_method_t method;
    char path[512];
    char query[512];
    char http_version[16];
    http_header_t headers[32];
    size_t header_count;
    char *body;
    size_t body_length;
    char *raw_data;   // 전체 요청 버퍼 (헤더+본문)
    size_t raw_length;
    int owns_raw_data; // 내부에서 malloc했다면 1
} http_request_t;

int http_parse_request(int fd, http_request_t *req, char *buffer, size_t bufsize);
const char *http_get_header(const http_request_t *req, const char *name);
int http_send_response(int fd, int status, const char *status_text,
                       const char *content_type, const void *body, size_t length,
                       const char *extra_headers);
int http_send_file_response(int fd, int status, const char *status_text,
                            const char *content_type, const char *file_path,
                            off_t offset, size_t length, int sendfile_enabled,
                            const char *extra_headers);
void http_free_request(http_request_t *req);
http_method_t http_method_from_string(const char *method);
const char *http_status_text(int status);

#endif
