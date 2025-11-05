#ifndef AUTH_H
#define AUTH_H

#include <stddef.h>

#include "router.h"

int auth_initialize(server_ctx_t *server);
void auth_handle_login(request_ctx_t *ctx);
void auth_handle_logout(request_ctx_t *ctx);
void auth_handle_me(request_ctx_t *ctx);
int auth_authenticate_request(request_ctx_t *ctx);
int auth_hash_password(const char *password, unsigned char *salt_out, size_t salt_len,
                       unsigned char *hash_out, size_t hash_len);
int auth_verify_password(const char *password, const unsigned char *salt, size_t salt_len,
                         const unsigned char *expected_hash, size_t hash_len);
int auth_generate_session_token(char *buffer, size_t len);

#endif
