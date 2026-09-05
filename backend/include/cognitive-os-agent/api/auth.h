/* auth.h — API key / bearer-token authentication.
 * A coa_auth context holds a set of accepted keys/tokens. Verification uses
 * constant-time comparison to avoid timing side channels. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct coa_auth coa_auth;

coa_auth *coa_auth_new(void);
void coa_auth_free(coa_auth *a);

/* Register an accepted key/token (copied). */
void coa_auth_add_key(coa_auth *a, const char *key);
int coa_auth_count(coa_auth *a);

/* Return 1 if `token` matches a registered key (constant-time), else 0. */
int coa_auth_check(coa_auth *a, const char *token);

/* Parse an Authorization header ("Bearer <token>" or a bare token) and verify.
 * Returns 1 if valid, 0 otherwise. */
int coa_auth_check_header(coa_auth *a, const char *authorization);

/* Fill out[0..2*bytes+1) with a hex token of `bytes` bytes of entropy. */
void coa_auth_generate_token(char *out, size_t bytes);

#ifdef __cplusplus
}
#endif
