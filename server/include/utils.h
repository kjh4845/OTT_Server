#ifndef UTILS_H
#define UTILS_H

// 서버 전반에서 사용하는 범용 헬퍼 함수 선언

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

void trim_trailing_newline(char *s);
void get_iso8601(char *buf, size_t len, time_t ts);
int ensure_directory(const char *path);
char *read_file(const char *path, size_t *out_len);
int base64url_encode(const uint8_t *input, size_t input_len, char *output, size_t output_len);
uint64_t get_monotonic_ms(void);
void log_info(const char *fmt, ...);
void log_warn(const char *fmt, ...);
void log_error(const char *fmt, ...);

typedef struct {
    char *data;      // 널 종료된 가변 버퍼
    size_t length;   // 현재 문자열 길이
    size_t capacity; // 할당된 총 크기
} string_builder_t;

int sb_init(string_builder_t *sb, size_t initial_capacity);
int sb_append(string_builder_t *sb, const char *fmt, ...);
void sb_free(string_builder_t *sb);
int sb_append_json_string(string_builder_t *sb, const char *value);
int json_get_string(const char *json, const char *key, char *out, size_t out_len);
int json_get_double(const char *json, const char *key, double *out);

#endif
