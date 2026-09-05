/* auth.h — API key / bearer-token authentication.
 * A ca_auth context holds a set of accepted keys/tokens. Verification uses
 * constant-time comparison to avoid timing side channels. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ca_auth ca_auth;

ca_auth *ca_auth_new(void);
void ca_auth_free(ca_auth *a);

/* Register an accepted key/token (copied). */
void ca_auth_add_key(ca_auth *a, const char *key);
int ca_auth_count(ca_auth *a);

/* Return 1 if `token` matches a registered key (constant-time), else 0. */
int ca_auth_check(ca_auth *a, const char *token);

/* Parse an Authorization header ("Bearer <token>" or a bare token) and verify.
 * Returns 1 if valid, 0 otherwise. */
int ca_auth_check_header(ca_auth *a, const char *authorization);

/* Fill out[0..2*bytes+1) with a hex token of `bytes` bytes of entropy. */
void ca_auth_generate_token(char *out, size_t bytes);

#ifdef __cplusplus
}
#endif
