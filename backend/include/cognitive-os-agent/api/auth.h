/* auth.h — API key / bearer-token authentication.
 * A auth context holds a set of accepted keys/tokens. Verification uses
 * constant-time comparison to avoid timing side channels. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct auth auth;

auth *auth_new(void);
void auth_free(auth *a);

/* Register an accepted key/token (copied). */
void auth_add_key(auth *a, const char *key);
int auth_count(auth *a);

/* Return 1 if `token` matches a registered key (constant-time), else 0. */
int auth_check(auth *a, const char *token);

/* Parse an Authorization header ("Bearer <token>" or a bare token) and verify.
 * Returns 1 if valid, 0 otherwise. */
int auth_check_header(auth *a, const char *authorization);

/* Fill out[0..2*bytes+1) with a hex token of `bytes` bytes of entropy. */
void auth_generate_token(char *out, size_t bytes);

#ifdef __cplusplus
}
#endif
