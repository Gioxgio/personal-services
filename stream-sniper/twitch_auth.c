#include "twitch_auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http.h"
#include "jsonlite.h"
#include "log.h"
#include "util.h"

#define TOKEN_MAX 512

static char     g_client_id[128];
static char     g_client_secret[256];
static char     g_token[TOKEN_MAX];
static bool     g_ready;
static bool     g_mint_failed;
static size_t   g_mint_count;

void twitch_auth_init_from_env(void)
{
    const char *id = getenv("TWITCH_CLIENT_ID");
    const char *secret = getenv("TWITCH_CLIENT_SECRET");
    const char *override = getenv("TWITCH_TOKEN");

    if (id)     snprintf(g_client_id, sizeof g_client_id, "%s", id);
    if (secret) snprintf(g_client_secret, sizeof g_client_secret, "%s", secret);

    if (override && *override) {
        snprintf(g_token, sizeof g_token, "%s", override);
        logmsg(LOG_INFO, "twitch: using TWITCH_TOKEN from environment");
    }
    g_ready = true;
}

void twitch_auth_invalidate(void)
{
    if (g_token[0]) {
        logmsg(LOG_DEBUG, "twitch: dropping cached token");
        g_token[0] = '\0';
    }
    g_mint_failed = false;   /* allow one re-mint attempt */
}

/*
 * Exchange the client credentials for an app access token. Returns 0 on
 * success and copies the token into out.
 */
static int mint(char *out, size_t outsz)
{
    char url[1024];
    snprintf(url, sizeof url,
             "https://id.twitch.tv/oauth2/token"
             "?client_id=%s&client_secret=%s&grant_type=client_credentials",
             g_client_id, g_client_secret);

    const char *hdrs[] = { "Content-Type: application/x-www-form-urlencoded", NULL };

    http_buffer body = { 0 };
    if (http_post(url, hdrs, NULL, &body) != 0) {
        logmsg(LOG_ERROR, "twitch: token request failed");
        http_buffer_free(&body);
        return -1;
    }

    /* Response looks like {"access_token":"...","expires_in":N,...} */
    if (json_find_string(body.data, "access_token", out, outsz) == 0) {
        logmsg(LOG_ERROR, "twitch: no access_token in response");
        http_buffer_free(&body);
        return -1;
    }
    http_buffer_free(&body);

    g_mint_count++;
    return 0;
}

const char *twitch_auth_client_id(void)
{
    if (!g_ready)
        twitch_auth_init_from_env();
    return g_client_id;
}

const char *twitch_auth_token(void)
{
    if (!g_ready)
        twitch_auth_init_from_env();

    if (g_token[0])
        return g_token;

    if (g_mint_failed)
        return NULL;

    if (g_client_id[0] == '\0' || g_client_secret[0] == '\0') {
        g_mint_failed = true;
        logmsg(LOG_WARN, "twitch: no TWITCH_CLIENT_ID/TWITCH_CLIENT_SECRET, "
                         "twitch probing disabled");
        return NULL;
    }

    if (mint(g_token, sizeof g_token) != 0) {
        g_mint_failed = true;
        g_token[0] = '\0';
        return NULL;
    }

    logmsg(LOG_INFO, "twitch: acquired access token (mint #%zu)", g_mint_count);
    return g_token;
}