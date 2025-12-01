// 범용 유틸 함수 모음 (로깅, 파일 IO, 문자열 빌더 등)
#include "utils.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>

// fgets 등으로 읽은 문자열 끝의 \n/\r 을 제거한다.
void trim_trailing_newline(char *s) {
    if (!s) {
        return;
    }
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[len - 1] = '\0';
        len--;
    }
}

// UTC 기준 ISO-8601 타임스탬프를 만든다.
void get_iso8601(char *buf, size_t len, time_t ts) {
    struct tm tm;
    gmtime_r(&ts, &tm);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

// 디렉터리가 없으면 생성하고, 파일이 있으면 에러를 돌려준다.
int ensure_directory(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return 0;
        }
        errno = ENOTDIR;
        return -1;
    }
    if (mkdir(path, 0755) == -1 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

// 전체 파일을 한 번에 읽어 널 종료된 버퍼로 반환한다.
char *read_file(const char *path, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long size = ftell(fp);
    if (size < 0) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);
    char *data = malloc((size_t)size + 1);
    if (!data) {
        fclose(fp);
        return NULL;
    }
    if (fread(data, 1, (size_t)size, fp) != (size_t)size) {
        free(data);
        fclose(fp);
        return NULL;
    }
    data[size] = '\0';
    if (out_len) {
        *out_len = (size_t)size;
    }
    fclose(fp);
    return data;
}

// URL-safe Base64 인코딩 (패딩 제거) 구현
int base64url_encode(const uint8_t *input, size_t input_len, char *output, size_t output_len) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t out_index = 0;
    for (size_t i = 0; i < input_len; i += 3) {
        uint32_t chunk = input[i] << 16;
        if (i + 1 < input_len) chunk |= input[i + 1] << 8;
        if (i + 2 < input_len) chunk |= input[i + 2];

        if (out_index + 4 > output_len) {
            return -1;
        }
        output[out_index++] = table[(chunk >> 18) & 0x3F];
        output[out_index++] = table[(chunk >> 12) & 0x3F];
        output[out_index++] = (i + 1 < input_len) ? table[(chunk >> 6) & 0x3F] : '=';
        output[out_index++] = (i + 2 < input_len) ? table[chunk & 0x3F] : '=';
    }
    while (out_index > 0 && output[out_index - 1] == '=') {
        out_index--;
    }
    if (out_index >= output_len) {
        return -1;
    }
    output[out_index] = '\0';
    return (int)out_index;
}

// 시스템 uptime을 밀리초 단위로 반환한다.
uint64_t get_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// 공통 포맷터: STDERR로 로그를 출력한다.
static void vlog_at_level(const char *level, const char *fmt, va_list ap) {
    fprintf(stderr, "[%s] ", level);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

void log_info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog_at_level("INFO", fmt, ap);
    va_end(ap);
}

void log_warn(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog_at_level("WARN", fmt, ap);
    va_end(ap);
}

void log_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog_at_level("ERROR", fmt, ap);
    va_end(ap);
}

// string_builder 초기화 및 버퍼 할당
int sb_init(string_builder_t *sb, size_t initial_capacity) {
    if (!sb) return -1;
    if (initial_capacity == 0) {
        initial_capacity = 256;
    }
    sb->data = malloc(initial_capacity);
    if (!sb->data) {
        return -1;
    }
    sb->length = 0;
    sb->capacity = initial_capacity;
    sb->data[0] = '\0';
    return 0;
}

// printf 스타일 포맷으로 문자열을 이어 붙인다.
int sb_append(string_builder_t *sb, const char *fmt, ...) {
    if (!sb || !fmt) return -1;
    va_list ap;
    va_start(ap, fmt);
    va_list ap_copy;
    va_copy(ap_copy, ap);
    int needed = vsnprintf(NULL, 0, fmt, ap_copy);
    va_end(ap_copy);
    if (needed < 0) {
        va_end(ap);
        return -1;
    }
    size_t required = sb->length + (size_t)needed + 1;
    if (required > sb->capacity) {
        size_t new_cap = sb->capacity;
        while (new_cap < required) {
            new_cap *= 2;
        }
        char *new_data = realloc(sb->data, new_cap);
        if (!new_data) {
            va_end(ap);
            return -1;
        }
        sb->data = new_data;
        sb->capacity = new_cap;
    }
    int written = vsnprintf(sb->data + sb->length, sb->capacity - sb->length, fmt, ap);
    va_end(ap);
    if (written < 0) {
        return -1;
    }
    sb->length += (size_t)written;
    return 0;
}

void sb_free(string_builder_t *sb) {
    if (!sb) return;
    free(sb->data);
    sb->data = NULL;
    sb->length = 0;
    sb->capacity = 0;
}

// JSON 특수 문자를 escape 처리하며 문자열을 추가한다.
int sb_append_json_string(string_builder_t *sb, const char *value) {
    if (!sb) return -1;
    if (!value) {
        value = "";
    }
    if (sb_append(sb, "\"") != 0) {
        return -1;
    }
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        switch (*p) {
            case '"':
                if (sb_append(sb, "\\\"") != 0) return -1;
                break;
            case '\\':
                if (sb_append(sb, "\\\\") != 0) return -1;
                break;
            case '\n':
                if (sb_append(sb, "\\n") != 0) return -1;
                break;
            case '\r':
                if (sb_append(sb, "\\r") != 0) return -1;
                break;
            case '\t':
                if (sb_append(sb, "\\t") != 0) return -1;
                break;
            default:
                if (*p < 0x20) {
                    if (sb_append(sb, "\\u%04x", *p) != 0) return -1;
                } else {
                    if (sb_append(sb, "%c", *p) != 0) return -1;
                }
                break;
        }
    }
    if (sb_append(sb, "\"") != 0) {
        return -1;
    }
    return 0;
}

// 매우 단순한 문자열 파서: "key":"value" 패턴만 처리한다.
int json_get_string(const char *json, const char *key, char *out, size_t out_len) {
    if (!json || !key || !out || out_len == 0) {
        return -1;
    }
    char pattern[64];
    if (snprintf(pattern, sizeof(pattern), "\"%s\"", key) >= (int)sizeof(pattern)) {
        return -1;
    }
    const char *pos = strstr(json, pattern);
    if (!pos) {
        return -1;
    }
    pos += strlen(pattern);
    while (*pos && (isspace((unsigned char)*pos) || *pos == ':')) {
        pos++;
    }
    if (*pos != '"') {
        return -1;
    }
    pos++;
    size_t i = 0;
    while (*pos && *pos != '"') {
        if (*pos == '\\' && pos[1]) {
            pos++;
        }
        if (i + 1 >= out_len) {
            return -1;
        }
        out[i++] = *pos++;
    }
    if (*pos != '"') {
        return -1;
    }
    out[i] = '\0';
    return 0;
}

// 숫자 값도 동일한 패턴으로 추출한다.
int json_get_double(const char *json, const char *key, double *out) {
    if (!json || !key || !out) {
        return -1;
    }
    char pattern[64];
    if (snprintf(pattern, sizeof(pattern), "\"%s\"", key) >= (int)sizeof(pattern)) {
        return -1;
    }
    const char *pos = strstr(json, pattern);
    if (!pos) {
        return -1;
    }
    pos += strlen(pattern);
    while (*pos && (isspace((unsigned char)*pos) || *pos == ':')) {
        pos++;
    }
    char *end = NULL;
    double value = strtod(pos, &end);
    if (end == pos) {
        return -1;
    }
    *out = value;
    return 0;
}
