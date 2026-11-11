/*
 * recorder.h - thread lifecycle for recordings.
 *
 * Owns the runtime state of every channel in a private rec_slot_t[]
 * indexed like the channel array passed to recorder_init(): an atomic
 * busy flag plus the pid / thread / has_thread of that channel's
 * recorder thread. Callers outside this module must treat those
 * fields as opaque and go through the functions below.
 */

#ifndef RECORDER_H
#define RECORDER_H

#include "urlparse.h"

#include <stdbool.h>
#include <stddef.h>

/*
 * Bind the recorder to the channel array main is about to sweep and
 * remember where recordings land. Call once, after config_load() and
 * before any other recorder function. Returns false if the slot array
 * cannot be allocated.
 */
bool recorder_init(const channel_t* chans, size_t n, const char* out_dir);

/* True if this channel already has an active recording thread. */
bool recorder_is_busy(const channel_t* ch);

/*
 * If the channel is idle, mark its slot busy and spawn a recording
 * thread that records stream_url. Takes ownership of stream_url:
 * it is freed on every path - by this function when the thread
 * cannot be started, and by the recording thread itself once
 * yt-dlp has the URL or has failed before using it. Returns
 * true if a thread was started, false otherwise (unknown
 * channel, already busy, or thread creation failed).
 *
 * Starting a recording marks this channel's current live event as
 * "tried": once the attempt is no longer running, the poll loop will
 * not retry it until the probe reports the channel offline. This stops
 * a channel whose live stream cannot be recorded - a subscriber-only
 * YouTube broadcast, for example - from being hammered on every sweep.
 */
bool recorder_start(const channel_t* ch, char* stream_url);

/* True if this channel already had a recording attempt for its current
 * live event and that attempt is no longer running (the slot is idle).
 * A not-live probe clears it via recorder_reset_live(). */
bool recorder_tried_live(const channel_t* ch);

/* The probe just reported this channel is not live: clear the "tried"
 * flag so a future live event gets a fresh attempt. Safe on an idle or
 * busy slot; a no-op for an unknown channel. */
void recorder_reset_live(const channel_t* ch);

/*
 * Stop everything: SIGTERM all live yt-dlp children, then join every
 * recording thread. Called once at shutdown.
 */
void recorder_shutdown(void);

#endif /* RECORDER_H */
