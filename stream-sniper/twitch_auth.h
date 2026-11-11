/*
 * twitch_auth.h - Twitch app-access-token management.
 *
 * Exchanges TWITCH_CLIENT_ID + TWITCH_CLIENT_SECRET for a bearer token via
 * the client_credentials grant, caches it, and re-mints on demand.
 */

#ifndef TWITCH_AUTH_H
#define TWITCH_AUTH_H

#include <stddef.h>

/*
 * Returns a valid bearer token (cached), or NULL if Twitch probing should be
 * disabled (no credentials, or a mint failure). The returned pointer stays
 * valid until the next twitch_auth_invalidate().
 */
const char *twitch_auth_token(void);

/* Drop the cached token so the next call re-mints (used after HTTP 401). */
void twitch_auth_invalidate(void);

/* Read credentials from the environment. Safe to call once at startup. */
void twitch_auth_init_from_env(void);

/* The configured client id ("" if unset); probe.c uses it for the API header. */
const char *twitch_auth_client_id(void);

#endif /* TWITCH_AUTH_H */