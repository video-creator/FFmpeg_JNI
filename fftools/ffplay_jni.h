/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef FFTOOLS_FFPLAY_JNI_H
#define FFTOOLS_FFPLAY_JNI_H

#include <stdint.h>

/**
 * @file ffplay_jni.h
 * Non-blocking, multi-instance FFplay API.
 *
 * Typical usage (single instance, non-blocking):
 * @code
 *   FFPlayer *p = ffplay_player_create();
 *   ffplay_player_set_url(p, "/path/to/video.mp4");
 *   ffplay_player_set_size(p, 1280, 720);
 *   ffplay_player_start(p);          // returns immediately
 *   // ... do other work ...
 *   ffplay_player_seek(p, 30.0);     // seek to 30 s
 *   ffplay_player_pause(p, 1);       // pause
 *   ffplay_player_destroy(p);        // stop + free everything
 * @endcode
 *
 * Multiple instances can be created from different threads; SDL is
 * initialised once and ref-counted internally.
 */

/** Opaque player handle. */
typedef struct FFPlayer FFPlayer;

/**
 * Loading callback function type.
 * @param player    The player instance
 * @param progress  Loading progress (0.0 to 1.0), or -1.0 if unknown
 * @param buffer_kb Current buffer size in kilobytes
 * @param user_data User-provided pointer
 */
typedef void (*ffplay_loading_callback)(FFPlayer *player, double progress, int buffer_kb, void *user_data);

/* ---- Lifecycle ---- */

/**
 * Allocate a new player instance with default settings.
 * @return  New player handle, or NULL on allocation failure.
 */
FFPlayer *ffplay_player_create(void);

/**
 * Set the media URL or local file path.
 * Must be called before ffplay_player_start().
 */
void ffplay_player_set_url(FFPlayer *player, const char *url);

/**
 * Set the window title (optional).
 */
void ffplay_player_set_title(FFPlayer *player, const char *title);

/**
 * Set the preferred display size in pixels.
 * Pass 0 to keep the default/auto size.
 */
void ffplay_player_set_size(FFPlayer *player, int width, int height);

/**
 * Disable/enable audio output. Must be called before start().
 */
void ffplay_player_set_audio_disabled(FFPlayer *player, int disabled);

/**
 * Disable/enable video display. Must be called before start().
 */
void ffplay_player_set_video_disabled(FFPlayer *player, int disabled);

/**
 * Set the startup volume level [0–100]. Default: 100.
 */
void ffplay_player_set_volume(FFPlayer *player, int volume);

/**
 * Set loop count. 0 = infinite, 1 = play once (default).
 */
void ffplay_player_set_loop(FFPlayer *player, int loop);

/**
 * Set loading callback for network streams.
 * @param callback  Callback function (can be NULL to disable)
 * @param user_data User data passed to callback
 */
void ffplay_player_set_loading_callback(FFPlayer *player, ffplay_loading_callback callback, void *user_data);

/**
 * Start playback.
 *
 * Initialises SDL (shared, ref-counted), creates the window/renderer, opens
 * the stream, launches decode threads and ensures the SDL event thread is
 * running.  Returns immediately (non-blocking).
 *
 * @return 0 on success, negative AVERROR on failure.
 */
int ffplay_player_start(FFPlayer *player);

/**
 * Stop playback and release all resources held by this instance.
 *
 * Safe to call from any thread.  After return, @p player must not be used.
 */
void ffplay_player_destroy(FFPlayer *player);

/* ---- Playback controls (can be called after start) ---- */

/**
 * Pause or resume.
 * @param paused  1 = pause, 0 = resume, -1 = toggle current state.
 */
void ffplay_player_pause(FFPlayer *player, int paused);

/**
 * Seek to an absolute position.
 * @param position_s  Target position in seconds from the beginning.
 */
void ffplay_player_seek(FFPlayer *player, double position_s);

/**
 * Seek relative to the current position.
 * @param delta_s  Offset in seconds (+forward / -backward).
 */
void ffplay_player_seek_relative(FFPlayer *player, double delta_s);

/**
 * Adjust volume while playing [0–100].
 */
void ffplay_player_set_volume_live(FFPlayer *player, int volume);

/**
 * Mute or unmute audio.
 * @param muted  1 = mute, 0 = unmute.
 */
void ffplay_player_set_mute(FFPlayer *player, int muted);

/**
 * Toggle fullscreen mode.
 */
void ffplay_player_toggle_fullscreen(FFPlayer *player);

/* ---- Status queries ---- */

/**
 * Return the current playback position in seconds, or NAN if unknown.
 */
double ffplay_player_get_position(FFPlayer *player);

/**
 * Return the total duration in seconds, or NAN if unknown.
 */
double ffplay_player_get_duration(FFPlayer *player);

/**
 * Return 1 if end-of-file has been reached, 0 otherwise.
 */
int ffplay_player_is_eof(FFPlayer *player);

/**
 * Run SDL event loop for a short period (non-blocking).
 * On macOS, this MUST be called from the main thread.
 * Returns 1 if there are still active players and events should continue.
 * Returns 0 if no active players remain and the event loop can stop.
 */
int ffplay_player_run_event_loop(int timeout_ms);

/* ---- Legacy command-line interface (kept for ffplay binary) ---- */

/**
 * Run ffplay with command-line arguments, blocking until the user closes
 * the window.  This is the entry point used by the ffplay binary and is
 * equivalent to the original ffplay main() behaviour.
 *
 * @return 0 on success, negative AVERROR on failure.
 */
int ffplay(int argc, const char **argv);

#endif /* FFTOOLS_FFPLAY_JNI_H */
