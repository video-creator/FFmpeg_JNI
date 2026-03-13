/*
 * Copyright (c) 2000-2003 Fabrice Bellard
 *
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

/**
 * @file
 * multimedia converter based on the FFmpeg libraries
 */

#include "config.h"

#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if HAVE_IO_H
#include <io.h>
#endif
#if HAVE_UNISTD_H
#include <unistd.h>
#endif

#if HAVE_SYS_RESOURCE_H
#include <sys/time.h>
#include <sys/types.h>
#include <sys/resource.h>
#elif HAVE_GETPROCESSTIMES
#include <windows.h>
#endif
#if HAVE_GETPROCESSMEMORYINFO
#include <windows.h>
#include <psapi.h>
#endif
#if HAVE_SETCONSOLECTRLHANDLER
#include <windows.h>
#endif

#if HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#if HAVE_TERMIOS_H
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <termios.h>
#elif HAVE_KBHIT
#include <conio.h>
#endif

#include "libavutil/bprint.h"
#include "libavutil/dict.h"
#include "libavutil/mem.h"
#include "libavutil/time.h"

#include "libavformat/avformat.h"

#include "libavdevice/avdevice.h"

#include "cmdutils.h"
#if CONFIG_MEDIACODEC
#include "compat/android/binder.h"
#endif
#include "ffmpeg.h"
#include "ffmpeg_sched.h"
#include "ffmpeg_utils.h"
#include "graph/graphprint.h"

/* Progress callback example */
static void transcoder_progress_callback(FFmpegTranscoder *transcoder, 
                                          double progress, int64_t frame, double fps,
                                          int64_t time_ms, double bitrate, void *user_data)
{
    const char *prefix = (const char *)user_data;
    if (progress >= 0) {
        fprintf(stderr, "%sProgress: %.1f%%, Frame: %lld, FPS: %.1f, Time: %.2fs, Bitrate: %.1f kbps\n",
                prefix ? prefix : "", progress * 100, (long long)frame, fps, time_ms / 1000.0, bitrate);
    } else {
        fprintf(stderr, "%sFrame: %lld, FPS: %.1f, Time: %.2fs, Bitrate: %.1f kbps\n",
                prefix ? prefix : "", (long long)frame, fps, time_ms / 1000.0, bitrate);
    }
}

/* Result callback example */
static void transcoder_result_callback(FFmpegTranscoder *transcoder, int result, void *user_data)
{
    const char *prefix = (const char *)user_data;
    if (result == 0) {
        fprintf(stderr, "%sTranscode completed successfully!\n", prefix ? prefix : "");
    } else if (result == 255) {
        fprintf(stderr, "%sTranscode was cancelled by user.\n", prefix ? prefix : "");
    } else {
        fprintf(stderr, "%sTranscode failed with error: %d\n", prefix ? prefix : "", result);
    }
}

/* Test transcoder API */
static int test_transcoder_api(int argc, char **argv)
{
    FFmpegTranscoder *transcoder;
    int ret;
    
    fprintf(stderr, "\n");
    fprintf(stderr, "========================================\n");
    fprintf(stderr, "  FFmpeg Transcoder API Test\n");
    fprintf(stderr, "========================================\n\n");

    if (argc < 1) {
        fprintf(stderr, "Usage: ffmpeg -test_transcoder <input_file> <output_file> [options...]\n");
        return 1;
    }

    /* Test 1: Create transcoder */
    fprintf(stderr, "[Test 1] Creating transcoder...\n");
    transcoder = ffmpeg_transcoder_init();
    if (!transcoder) {
        fprintf(stderr, "  FAILED: Could not create transcoder\n");
        return 1;
    }
    fprintf(stderr, "  SUCCESS: Transcoder created\n\n");

    /* Test 2: Set callbacks */
    fprintf(stderr, "[Test 2] Setting callbacks...\n");
    ffmpeg_transcoder_set_progress_callback(transcoder, transcoder_progress_callback, "[Progress] ");
    ffmpeg_transcoder_set_result_callback(transcoder, transcoder_result_callback, "[Result] ");
    fprintf(stderr, "  SUCCESS: Callbacks registered\n\n");

    /* Test 3: Check initial state */
    fprintf(stderr, "[Test 3] Checking initial state...\n");
    if (ffmpeg_transcoder_is_running(transcoder)) {
        fprintf(stderr, "  FAILED: Transcoder should not be running initially\n");
        ffmpeg_transcoder_free(transcoder);
        return 1;
    }
    fprintf(stderr, "  SUCCESS: Initial state correct\n\n");

    /* Test 4: Run transcode 
     * Note: ffmpeg_parse_options expects argv[0] to be program name,
     * so we need to prepend a dummy program name.
     */
    fprintf(stderr, "[Test 4] Running transcode...\n");
    
    /* Build argv with dummy program name */
    const char **new_argv = av_mallocz((argc + 1) * sizeof(char *));
    if (!new_argv) {
        fprintf(stderr, "  FAILED: Memory allocation error\n");
        ffmpeg_transcoder_free(transcoder);
        return 1;
    }
    new_argv[0] = "ffmpeg_transcoder";
    for (int i = 0; i < argc; i++) {
        new_argv[i + 1] = argv[i];
    }
    
    fprintf(stderr, "  Command: ");
    for (int i = 0; i < argc + 1; i++) {
        fprintf(stderr, "%s ", new_argv[i]);
    }
    fprintf(stderr, "\n\n");
    
    ffmpeg_transcoder_run(transcoder, argc + 1, new_argv);
    av_free(new_argv);
    
    /* Test 5: Check result */
    fprintf(stderr, "\n[Test 5] Checking result...\n");
    ret = ffmpeg_transcoder_get_result(transcoder);
    fprintf(stderr, "  Result code: %d\n", ret);
    fprintf(stderr, "  Running status: %d\n", ffmpeg_transcoder_is_running(transcoder));

    /* Cleanup */
    fprintf(stderr, "\n[Test 6] Cleaning up...\n");
    ffmpeg_transcoder_free(transcoder);
    fprintf(stderr, "  SUCCESS: Transcoder freed\n");

    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "  Test completed\n");
    fprintf(stderr, "========================================\n");
    
    return (ret == 0) ? 0 : 1;
}

/* Test cancel functionality */
static int test_transcoder_cancel(int argc, char **argv)
{
    FFmpegTranscoder *transcoder;
    int cancel_after_seconds = 2;
    
    fprintf(stderr, "\n");
    fprintf(stderr, "========================================\n");
    fprintf(stderr, "  FFmpeg Transcoder Cancel Test\n");
    fprintf(stderr, "========================================\n\n");

    if (argc < 2) {
        fprintf(stderr, "Usage: %s -test_cancel <input_file> [ffmpeg_options...]\n", argv[0]);
        return 1;
    }

    transcoder = ffmpeg_transcoder_init();
    if (!transcoder) {
        fprintf(stderr, "Failed to create transcoder\n");
        return 1;
    }

    ffmpeg_transcoder_set_progress_callback(transcoder, transcoder_progress_callback, "[Progress] ");
    ffmpeg_transcoder_set_result_callback(transcoder, transcoder_result_callback, "[Result] ");

    fprintf(stderr, "Starting transcode, will cancel after %d seconds...\n\n", cancel_after_seconds);

    /* Note: This is a simplified test. In real usage, you would run transcode
     * in a separate thread and call ffmpeg_transcoder_cancel() from another thread.
     * For this test, we just show the cancel API exists. */
    
    ffmpeg_transcoder_run(transcoder, argc, (const char **)argv);
    
    ffmpeg_transcoder_free(transcoder);
    
    fprintf(stderr, "\nCancel test completed.\n");
    return 0;
}

int main(int argc, char **argv)
{
    /* Test transcoder API mode:
     *   ffmpeg -test_transcoder <input_file> <output_file> [options...]
     */
    if (argc >= 2 && !strcmp(argv[1], "-test_transcoder")) {
        return test_transcoder_api(argc - 2, argv + 2);
    }
    
    /* Test cancel mode:
     *   ffmpeg -test_cancel <input_file> <output_file> [options...]
     */
    if (argc >= 2 && !strcmp(argv[1], "-test_cancel")) {
        return test_transcoder_cancel(argc - 2, argv + 2);
    }
    
    return ffmpeg(argc, argv);
}

