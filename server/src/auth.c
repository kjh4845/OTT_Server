#include "auth.h"

#include <ctype.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "db.h"
#include "http.h"
#include "utils.h"

#define SESSION_COOKIE_NAME "ott_session"
#define AUTH_SALT_LEN 16
#define AUTH_HASH_LEN 32
#define AUTH_ITERATIONS 200000

static int parse_cookie(const char *header, const char *name, char *out, size_t len) {
    if (!header || !name || !out || len == 0) {
        return -1;
    }
    size_t name_len = strlen(name);
    const char *p = header;
    while (*p) {
        while (*p == ' ' || *p == ';') {
            p++;
        }
        const char *eq = strchr(p, '=');
        if (!eq) {
            break;
        }
        const char *end = strchr(eq + 1, ';');
        if (!end) {
            end = p + strlen(p);
        }
        size_t current_name_len = (size_t)(eq - p);
        if (current_name_len == name_len && strncmp(p, name, name_len) == 0) {
            size_t value_len = (size_t)(end - (eq + 1));
            if (value_len >= len) {
                value_len = len - 1;
            }
            memcpy(out, eq + 1, value_len);
            while (value_len > 0 && isspace((unsigned char)out[value_len - 1])) {
                value_len--;
            }
            out[value_len] = '\0';
            return 0;
        }
        if (*end == '\0') {
            break;
        }
        p = end + 1;
    }
    return -1;
}

static int clear_session_cookie(request_ctx_t *ctx) {
    char cookie[512];
    if (snprintf(cookie, sizeof(cookie),
                 "Set-Cookie: %s=deleted; HttpOnly; SameSite=Lax; Path=/; Max-Age=0; Expires=Thu, 01 Jan 1970 00:00:00 GMT\r\n",
                 SESSION_COOKIE_NAME) >= (int)sizeof(cookie)) {
        return -1;
    }
    char headers[1024];
    if (snprintf(headers, sizeof(headers), "%s%s", ctx->server->security_headers, cookie) >= (int)sizeof(headers)) {
        return -1;
    }
    return http_send_response(ctx->client_fd, 204, http_status_text(204), NULL, NULL, 0, headers);
}

int auth_hash_password(const char *password, unsigned char *salt_out, size_t salt_len,
                       unsigned char *hash_out, size_t hash_len) {
    if (!password || !salt_out || !hash_out) {
        return -1;
    }
    if (RAND_bytes(salt_out, (int)salt_len) != 1) {
        return -1;
    }
    if (PKCS5_PBKDF2_HMAC(password, (int)strlen(password), salt_out, (int)salt_len,
                          AUTH_ITERATIONS, EVP_sha256(), (int)hash_len, hash_out) != 1) {
        return -1;
    }
    return 0;
}

int auth_verify_password(const char *password, const unsigned char *salt, size_t salt_len,
                         const unsigned char *expected_hash, size_t hash_len) {
    if (!password || !salt || !expected_hash) {
        return -1;
    }
    unsigned char computed[AUTH_HASH_LEN];
    if (hash_len > sizeof(computed)) {
        return -1;
    }
    if (PKCS5_PBKDF2_HMAC(password, (int)strlen(password), salt, (int)salt_len,
                          AUTH_ITERATIONS, EVP_sha256(), (int)hash_len, computed) != 1) {
        return -1;
    }
    if (CRYPTO_memcmp(computed, expected_hash, hash_len) != 0) {
        return -1;
    }
    return 0;
}

int auth_generate_session_token(char *buffer, size_t len) {
    if (!buffer || len < 48) {
        return -1;
    }
    unsigned char random_bytes[32];
    if (RAND_bytes(random_bytes, sizeof(random_bytes)) != 1) {
        return -1;
    }
    int written = base64url_encode(random_bytes, sizeof(random_bytes), buffer, len);
    if (written <= 0) {
        return -1;
    }
    buffer[written] = '\0';
    return 0;
}

static void ensure_default_users(server_ctx_t *server) {
    if (!server) {
        return;
    }
    static const struct {
        const char *username;
        const char *password;
    } default_users[] = {
        {"test", "test1234"},
        {"demo", "demo1234"},
        {"guest", "guestpass"},
        {"sample", "sample1234"},
    };
    unsigned char hash[AUTH_HASH_LEN];
    unsigned char salt[AUTH_SALT_LEN];
    for (size_t i = 0; i < sizeof(default_users) / sizeof(default_users[0]); i++) {
        int user_id = 0;
        if (db_get_user_credentials(&server->db, default_users[i].username, &user_id, hash, sizeof(hash),
                                    salt, sizeof(salt)) == 0) {
            continue;
        }
        if (auth_hash_password(default_users[i].password, salt, sizeof(salt), hash, sizeof(hash)) == 0) {
            if (db_upsert_user(&server->db, default_users[i].username, hash, sizeof(hash), salt, sizeof(salt)) == 0) {
                log_info("Created default user '%s'", default_users[i].username);
            }
        }
    }
}

int auth_initialize(server_ctx_t *server) {
    if (!server) {
        return -1;
    }
    ensure_default_users(server);
    db_purge_expired_sessions(&server->db, time(NULL));
    return 0;
}

static int load_session(server_ctx_t *server, const char *token, request_ctx_t *ctx) {
    time_t expires = 0;
    int user_id = 0;
    if (db_get_session(&server->db, token, &user_id, &expires) != 0) {
        return -1;
    }
    time_t now = time(NULL);
    if (expires <= now) {
        db_delete_session(&server->db, token);
        return -1;
    }
    ctx->authenticated = 1;
    ctx->user_id = user_id;
    snprintf(ctx->session_token, sizeof(ctx->session_token), "%s", token);
    db_get_username_by_id(&server->db, user_id, ctx->username, sizeof(ctx->username));
    return 0;
}

int auth_authenticate_request(request_ctx_t *ctx) {
    const char *cookie_header = http_get_header(ctx->request, "Cookie");
    if (!cookie_header) {
        return 0;
    }
    char token[128];
    if (parse_cookie(cookie_header, SESSION_COOKIE_NAME, token, sizeof(token)) != 0) {
        return 0;
    }
    if (load_session(ctx->server, token, ctx) == 0) {
        return 1;
    }
    return 0;
}

void auth_handle_login(request_ctx_t *ctx) {
    if (!ctx->request->body || ctx->request->body_length == 0) {
        router_send_json_error(ctx, 400, "Missing credentials");
        return;
    }
    char username[128];
    char password[128];
    if (json_get_string(ctx->request->body, "username", username, sizeof(username)) != 0 ||
        json_get_string(ctx->request->body, "password", password, sizeof(password)) != 0) {
        router_send_json_error(ctx, 400, "Invalid payload");
        return;
    }
    unsigned char hash[AUTH_HASH_LEN];
    unsigned char salt[AUTH_SALT_LEN];
    int user_id = 0;
    if (db_get_user_credentials(&ctx->server->db, username, &user_id, hash, sizeof(hash),
                                salt, sizeof(salt)) != 0) {
        router_send_json_error(ctx, 401, "Invalid credentials");
        memset(password, 0, sizeof(password));
        return;
    }
    if (auth_verify_password(password, salt, sizeof(salt), hash, sizeof(hash)) != 0) {
        router_send_json_error(ctx, 401, "Invalid credentials");
        memset(password, 0, sizeof(password));
        return;
    }
    char token[128];
    if (auth_generate_session_token(token, sizeof(token)) != 0) {
        router_send_json_error(ctx, 500, "Failed to generate session");
        return;
    }
    time_t now = time(NULL);
    db_purge_expired_sessions(&ctx->server->db, now);
    int max_age = ctx->server->session_ttl_hours * 3600;
    time_t expires_at = now + max_age;
    if (db_create_session(&ctx->server->db, token, user_id, expires_at) != 0) {
        router_send_json_error(ctx, 500, "Failed to persist session");
        return;
    }
    ctx->authenticated = 1;
    ctx->user_id = user_id;
    snprintf(ctx->username, sizeof(ctx->username), "%s", username);
    snprintf(ctx->session_token, sizeof(ctx->session_token), "%s", token);

    char response[256];
    snprintf(response, sizeof(response), "{\"username\":\"%s\"}", username);

    char cookie[512];
    snprintf(cookie, sizeof(cookie),
             "Set-Cookie: %s=%s; HttpOnly; SameSite=Lax; Path=/; Max-Age=%d\r\n",
             SESSION_COOKIE_NAME, token, max_age);

    router_send_json(ctx, 200, response, cookie);
    memset(password, 0, sizeof(password));
}

void auth_handle_logout(request_ctx_t *ctx) {
    if (ctx->session_token[0]) {
        db_delete_session(&ctx->server->db, ctx->session_token);
    } else {
        const char *cookie_header = http_get_header(ctx->request, "Cookie");
        char token[128];
        if (cookie_header && parse_cookie(cookie_header, SESSION_COOKIE_NAME, token, sizeof(token)) == 0) {
            db_delete_session(&ctx->server->db, token);
        }
    }
    ctx->authenticated = 0;
    ctx->user_id = 0;
    ctx->username[0] = '\0';
    ctx->session_token[0] = '\0';
    (void)clear_session_cookie(ctx);
}

void auth_handle_me(request_ctx_t *ctx) {
    if (!ctx->authenticated) {
        router_send_json_error(ctx, 401, "Unauthorized");
        return;
    }
    char body[256];
    snprintf(body, sizeof(body), "{\"username\":\"%s\",\"userId\":%d}", ctx->username, ctx->user_id);
    router_send_json(ctx, 200, body, NULL);
}
