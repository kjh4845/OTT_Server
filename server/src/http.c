#include "http.h"
#include "utils.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/sendfile.h>
#define HAVE_SENDFILE 1
#else
#define HAVE_SENDFILE 0
#endif
#define HTTP_INITIAL_BUFFER 8192
#define HTTP_MAX_BUFFER (8 * 1024 * 1024)

static void request_init(http_request_t *req) {
    memset(req, 0, sizeof(*req));
    req->method = HTTP_UNKNOWN;
}

http_method_t http_method_from_string(const char *method) {
    if (strcmp(method, "GET") == 0) return HTTP_GET;
    if (strcmp(method, "POST") == 0) return HTTP_POST;
    if (strcmp(method, "PUT") == 0) return HTTP_PUT;
    if (strcmp(method, "DELETE") == 0) return HTTP_DELETE;
    if (strcmp(method, "OPTIONS") == 0) return HTTP_OPTIONS;
    return HTTP_UNKNOWN;
}

static int ensure_capacity(http_request_t *req, char **buffer, size_t *capacity, size_t required) {
    if (required <= *capacity) {
        return 0;
    }
    if (!req->owns_raw_data) {
        return -1;
    }
    size_t new_cap = *capacity * 2;
    while (new_cap < required) {
        new_cap *= 2;
        if (new_cap > HTTP_MAX_BUFFER) {
            return -1;
        }
    }
    if (new_cap > HTTP_MAX_BUFFER) {
        return -1;
    }
    char *new_buf = realloc(*buffer, new_cap);
    if (!new_buf) {
        return -1;
    }
    *buffer = new_buf;
    *capacity = new_cap;
    return 0;
}

int http_parse_request(int fd, http_request_t *req, char *buffer, size_t bufsize) {
    request_init(req);
    size_t capacity = buffer ? bufsize : HTTP_INITIAL_BUFFER;
    char *raw = buffer;
    if (!raw) {
        raw = malloc(capacity);
        if (!raw) {
            return -1;
        }
        req->owns_raw_data = 1;
    } else {
        req->owns_raw_data = 0;
    }
    req->raw_data = raw;

    size_t total = 0;
    char *header_end = NULL;
    while (!header_end) {
        if (total + 1 >= capacity) {
            if (ensure_capacity(req, &raw, &capacity, total + 1) != 0) {
                goto fail;
            }
            req->raw_data = raw;
        }
        ssize_t n = recv(fd, raw + total, capacity - total - 1, 0);
        if (n <= 0) {
            goto fail;
        }
        total += (size_t)n;
        raw[total] = '\0';
        header_end = strstr(raw, "\r\n\r\n");
        if (header_end) {
            break;
        }
        if (total >= HTTP_MAX_BUFFER) {
            goto fail;
        }
    }

    size_t header_len = (size_t)(header_end - raw) + 4; // include CRLFCRLF

    char *line = raw;
    char *line_end = strstr(line, "\r\n");
    if (!line_end) {
        goto fail;
    }
    *line_end = '\0';

    char method[16] = {0};
    char url[512] = {0};
    char version[16] = {0};
    if (sscanf(line, "%15s %511s %15s", method, url, version) != 3) {
        goto fail;
    }
    req->method = http_method_from_string(method);
    strncpy(req->http_version, version, sizeof(req->http_version) - 1);

    char *qmark = strchr(url, '?');
    if (qmark) {
        *qmark = '\0';
        strncpy(req->query, qmark + 1, sizeof(req->query) - 1);
    }
    strncpy(req->path, url, sizeof(req->path) - 1);

    req->header_count = 0;
    char *cursor = line_end + 2; // move past CRLF
    while (cursor < raw + header_len) {
        char *next = strstr(cursor, "\r\n");
        if (!next) {
            break;
        }
        if (next == cursor) {
            break; // empty line
        }
        *next = '\0';
        char *colon = strchr(cursor, ':');
        if (colon) {
            *colon = '\0';
            char *value = colon + 1;
            while (*value && isspace((unsigned char)*value)) {
                value++;
            }
            if (req->header_count < ARRAY_SIZE(req->headers)) {
                req->headers[req->header_count].name = cursor;
                req->headers[req->header_count].value = value;
                req->header_count++;
            }
        }
        cursor = next + 2;
    }

    const char *cl_value = http_get_header(req, "Content-Length");
    size_t content_length = 0;
    if (cl_value) {
        content_length = (size_t)strtoull(cl_value, NULL, 10);
    }

    size_t available_body = total - header_len;
    if (content_length > HTTP_MAX_BUFFER) {
        goto fail;
    }
    if (content_length > available_body) {
        size_t missing = content_length - available_body;
        if (ensure_capacity(req, &raw, &capacity, header_len + content_length + 1) != 0) {
            goto fail;
        }
        req->raw_data = raw;
        while (missing > 0) {
            ssize_t n = recv(fd, raw + total, capacity - total - 1, 0);
            if (n <= 0) {
                goto fail;
            }
            total += (size_t)n;
            size_t consumed = (size_t)n;
            if (consumed > missing) {
                consumed = missing;
            }
            missing -= consumed;
        }
    }

    req->body = raw + header_len;
    req->body_length = content_length;
    req->raw_length = header_len + content_length;
    if (total < capacity) {
        raw[total] = '\0';
    }
    return 0;

fail:
    if (req->owns_raw_data && raw) {
        free(raw);
    }
    request_init(req);
    return -1;
}

const char *http_get_header(const http_request_t *req, const char *name) {
    for (size_t i = 0; i < req->header_count; ++i) {
        if (strcasecmp(req->headers[i].name, name) == 0) {
            return req->headers[i].value;
        }
    }
    return NULL;
}

static int send_all(int fd, const void *buf, size_t len) {
    const unsigned char *p = buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, 0);
        if (n <= 0) {
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

int http_send_response(int fd, int status, const char *status_text,
                       const char *content_type, const void *body, size_t length,
                       const char *extra_headers) {
    char header[2048];
    int offset = snprintf(header, sizeof(header),
                          "HTTP/1.1 %d %s\r\n"
                          "Connection: close\r\n"
                          "Content-Length: %zu\r\n",
                          status, status_text, length);
    if (offset < 0 || (size_t)offset >= sizeof(header)) {
        return -1;
    }
    if (content_type && *content_type) {
        int n = snprintf(header + offset, sizeof(header) - offset,
                         "Content-Type: %s\r\n", content_type);
        if (n < 0 || (size_t)n >= sizeof(header) - offset) {
            return -1;
        }
        offset += n;
    }
    if (extra_headers && *extra_headers) {
        int n = snprintf(header + offset, sizeof(header) - offset, "%s", extra_headers);
        if (n < 0 || (size_t)n >= sizeof(header) - offset) {
            return -1;
        }
        offset += n;
    }
    if (offset + 2 >= (int)sizeof(header)) {
        return -1;
    }
    header[offset++] = '\r';
    header[offset++] = '\n';

    if (send_all(fd, header, (size_t)offset) != 0) {
        return -1;
    }
    if (body && length > 0) {
        if (send_all(fd, body, length) != 0) {
            return -1;
        }
    }
    return 0;
}

int http_send_file_response(int fd, int status, const char *status_text,
                            const char *content_type, const char *file_path,
                            off_t offset, size_t length, int sendfile_enabled,
                            const char *extra_headers) {
    struct stat st;
    if (stat(file_path, &st) != 0) {
        return -1;
    }
    if (offset > st.st_size) {
        return -1;
    }
    if (length == 0 || (off_t)length > st.st_size - offset) {
        length = (size_t)(st.st_size - offset);
    }

    char header[2048];
    int hdr_off = snprintf(header, sizeof(header),
                           "HTTP/1.1 %d %s\r\n"
                           "Connection: close\r\n"
                           "Content-Length: %zu\r\n"
                           "Content-Type: %s\r\n",
                           status, status_text, length,
                           content_type ? content_type : "application/octet-stream");
    if (hdr_off < 0 || (size_t)hdr_off >= sizeof(header)) {
        return -1;
    }
    if (extra_headers && *extra_headers) {
        int n = snprintf(header + hdr_off, sizeof(header) - hdr_off, "%s", extra_headers);
        if (n < 0 || (size_t)n >= sizeof(header) - hdr_off) {
            return -1;
        }
        hdr_off += n;
    }
    if (hdr_off + 2 >= (int)sizeof(header)) {
        return -1;
    }
    header[hdr_off++] = '\r';
    header[hdr_off++] = '\n';

    if (send_all(fd, header, (size_t)hdr_off) != 0) {
        return -1;
    }

    int file_fd = open(file_path, O_RDONLY);
    if (file_fd < 0) {
        return -1;
    }
    int rc = 0;
    if (offset > 0) {
        if (lseek(file_fd, offset, SEEK_SET) < 0) {
            rc = -1;
            goto done;
        }
    }
#if HAVE_SENDFILE
    if (sendfile_enabled) {
        size_t remaining = length;
        while (remaining > 0) {
            ssize_t sent = sendfile(fd, file_fd, NULL, remaining);
            if (sent <= 0) {
                rc = -1;
                break;
            }
            remaining -= (size_t)sent;
        }
    } else
#else
    (void)sendfile_enabled;
#endif
    {
        char buf[8192];
        size_t remaining = length;
        while (remaining > 0) {
            size_t to_read = remaining < sizeof(buf) ? remaining : sizeof(buf);
            ssize_t r = read(file_fd, buf, to_read);
            if (r <= 0) {
                rc = -1;
                break;
            }
            if (send_all(fd, buf, (size_t)r) != 0) {
                rc = -1;
                break;
            }
            remaining -= (size_t)r;
        }
    }

done:
    close(file_fd);
    return rc;
}

void http_free_request(http_request_t *req) {
    if (req->owns_raw_data && req->raw_data) {
        free(req->raw_data);
    }
    request_init(req);
}

const char *http_status_text(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 206: return "Partial Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 416: return "Range Not Satisfiable";
        default: return status >= 500 ? "Internal Server Error" : "OK";
    }
}
