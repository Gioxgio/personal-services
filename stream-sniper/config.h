/*
 * config.h - loads the channel list from a text file.
 *
 * Pure file I/O: owns the g_channels / g_nchannels
 * globals, the line-skipping policy and the growth
 * policy. Normalising each line lives in urlparse.h's
 * channel_parse(), so this module holds no parsing
 * logic of its own.
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>

#include "urlparse.h"

extern channel_t *g_channels;
extern size_t     g_nchannels;

/* Parse the file at path into g_channels. Returns 0 on success, -1 on error. */
int config_load(const char *path);

/* Free every channel and its strings. Safe to call once at shutdown. */
void config_free(void);

#endif /* CONFIG_H */
