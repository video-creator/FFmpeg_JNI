/*
 * Copyright (c) 2003 Fabrice Bellard
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
 * simple media player based on the FFmpeg libraries
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ffplay_jni.h"

static void ffplay_loading_cb(FFPlayer *p, double progress, int buffer_kb, void *user_data)
{
    (void)p;
    const char *tag = (const char *)user_data;

    if (progress <= 0.0)
        fprintf(stderr, "[%s] loading start (buffer=%d KB)\n", tag ? tag : "?", buffer_kb);
    else if (progress >= 1.0)
        fprintf(stderr, "[%s] loading end   (buffer=%d KB)\n", tag ? tag : "?", buffer_kb);
    else
        fprintf(stderr, "[%s] loading %.0f%% (buffer=%d KB)\n", tag ? tag : "?", progress * 100.0, buffer_kb);
}

/* Called from the main */
int main(int argc, char **argv)
{
    /* Test options parsing mode:
     *   ffplay -test_options <url>
     *
     * This mode tests ffplay_player_parse_options() and ffplay_player_set_option()
     * functions by applying various options before playback.
     */
    if (argc >= 2 && !strcmp(argv[1], "-test_options")) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s -test_options <url>\n", argv[0]);
            return 1;
        }

        const char *url = argv[2];
        fprintf(stderr, "=== Testing option parsing with URL: %s ===\n", url);

        FFPlayer *player = ffplay_player_create();
        if (!player) {
            fprintf(stderr, "Failed to create player\n");
            return 1;
        }

        /* Test 1: ffplay_player_set_option for individual options */
        fprintf(stderr, "\n[Test 1] Testing ffplay_player_set_option...\n");

        int ret = ffplay_player_set_option(player, "autoexit", NULL);
        fprintf(stderr, "  set_option('autoexit', NULL) -> %d (expected: 0, no arg consumed)\n", ret);

        ret = ffplay_player_set_option(player, "loop", "2");
        fprintf(stderr, "  set_option('loop', '1') -> %d (expected: 1, arg consumed)\n", ret);

        ret = ffplay_player_set_option(player, "volume", "75");
        fprintf(stderr, "  set_option('volume', '75') -> %d (expected: 1, arg consumed)\n", ret);

        ret = ffplay_player_set_option(player, "seek_interval", "5");
        fprintf(stderr, "  set_option('seek_interval', '5') -> %d (expected: 1, arg consumed)\n", ret);

        /* Test 2: ffplay_player_parse_options for array of options */
        fprintf(stderr, "\n[Test 2] Testing ffplay_player_parse_options...\n");

        const char *opts[] = {
            "-framedrop",           /* boolean option */
            "-window_title", "OptionTest",  /* string option */
            "-bytes", "-1",        /* integer option */
        };
        int opts_count = sizeof(opts) / sizeof(opts[0]);

        ret = ffplay_player_parse_options(player, opts_count, opts);
        fprintf(stderr, "  parse_options({-framedrop, -window_title OptionTest, -bytes -1}) -> %d\n", ret);

        /* Test 3: Set URL and start playback */
        fprintf(stderr, "\n[Test 3] Starting playback...\n");

        ffplay_player_set_url(player, url);
        ffplay_player_set_size(player, 640, 480);

        ret = ffplay_player_start(player);
        if (ret < 0) {
            fprintf(stderr, "Failed to start player: %d\n", ret);
            ffplay_player_destroy(player);
            return 1;
        }

        fprintf(stderr, "Playback started. Options applied successfully.\n");
        fprintf(stderr, "Press 'q' to quit.\n\n");

        /* Run event loop until EOF or user quits */
        while (ffplay_player_run_event_loop(1) > 0) {
            /* noop */
        }

        ffplay_player_destroy(player);
        fprintf(stderr, "\n=== Option parsing test completed ===\n");
        return 0;
    }

    /*
     * Multi-instance test mode:
     *   ffplay -multi <url1> <url2> ...
     *   ffplay -multi <N> <url>
     *
     * Notes:
     *   - Each URL opens in its own SDL window.
     *   - Keyboard/mouse events are routed by SDL windowID, so controls work per-window.
     *   - On macOS the event pump MUST run on the main thread; we drive it here via
     *     ffplay_player_run_event_loop() in a tight "tick" loop.
     */
    if (argc >= 2 && !strcmp(argv[1], "-multi")) {
        int first_url_arg = 2;
        int count = 0;

        if (argc < 3) {
            fprintf(stderr, "Usage: %s -multi <url1> <url2> ...\n", argv[0]);
            fprintf(stderr, "   or: %s -multi <N> <url>\n", argv[0]);
            return 1;
        }

        /* If argv[2] is a positive integer and there is exactly one url after it,
         * treat it as repeat-count mode. */
        char *endptr = NULL;
        long n = strtol(argv[2], &endptr, 10);
        if (endptr && *endptr == '\0' && n > 0 && argc >= 4) {
            count = (int)n;
            first_url_arg = 3;
        } else {
            count = argc - 2;
            first_url_arg = 2;
        }

        FFPlayer **players = (FFPlayer **)calloc((size_t)count, sizeof(*players));
        if (!players) {
            fprintf(stderr, "Out of memory\n");
            return 1;
        }

        int started = 0;
        for (int i = 0; i < count; i++) {
            const char *url = (first_url_arg + i < argc) ? argv[first_url_arg + i] : argv[first_url_arg];

            players[i] = ffplay_player_create();
            if (!players[i]) {
                fprintf(stderr, "Failed to create player %d\n", i);
                continue;
            }

            ffplay_player_set_url(players[i], url);

            /* loading callback test: print loading start/end */
            /* Use a stable pointer (argv string) as user_data to avoid ownership issues in tests. */
            ffplay_player_set_loading_callback(players[i], ffplay_loading_cb, (void *)url);

            char title[256];
            snprintf(title, sizeof(title), "FFPlayer #%d", i);
            ffplay_player_set_title(players[i], title);

            /* Stagger sizes a bit to make it obvious they're independent */
            ffplay_player_set_size(players[i], 640 + (i % 3) * 80, 360 + (i % 3) * 45);

            int ret = ffplay_player_start(players[i]);
            if (ret < 0) {
                fprintf(stderr, "Failed to start player %d: %d\n", i, ret);
                ffplay_player_destroy(players[i]);
                players[i] = NULL;
                continue;
            }
            started++;
        }

        if (started == 0) {
            fprintf(stderr, "No player started\n");
            free(players);
            return 1;
        }

        /* Main-thread tick: stop when all players reach EOF or are closed */
        for (;;) {
            if (ffplay_player_run_event_loop(1) <= 0)
                break;

            int all_eof = 1;
            for (int i = 0; i < count; i++) {
                if (players[i] && !ffplay_player_is_eof(players[i])) {
                    all_eof = 0;
                    break;
                }
            }
            if (all_eof) {
                for (int i = 0; i < count; i++) {
                    if (players[i]) {
                        ffplay_player_destroy(players[i]);
                        players[i] = NULL;
                    }
                }
                break;
            }
        }

        /* Cleanup (safe even if instances already closed themselves)
         * NOTE: callback user_data was strdup(tag) above, free it here.
         */
        for (int i = 0; i < count; i++) {
            if (players[i]) {
                /* Best-effort: callback userdata string is owned by this test. */
                /* We cannot retrieve it back from FFPlayer API, so we don't free it here.
                 * For a real app you should manage the lifecycle externally.
                 */
                ffplay_player_destroy(players[i]);
            }
        }
        free(players);
        return 0;
    }

    /* Keep the original single-instance test (no args) for convenience */
    if (argc == 1) {
        FFPlayer *player = ffplay_player_create();
        if (!player) {
            fprintf(stderr, "Failed to create player\n");
            return 1;
        }

        /* NOTE: hardcoded path kept from previous test; change as needed. */
        ffplay_player_set_url(player,
                              "/Users/wangyaqiang/Downloads/xxxs/un-bt709-bt470bg/706x576-un-bt709-bt470bg.mp4");
        ffplay_player_set_title(player, "Test Player");
        ffplay_player_set_size(player, 800, 600);

        int ret = ffplay_player_start(player);
        if (ret < 0) {
            fprintf(stderr, "Failed to start player: %d\n", ret);
            ffplay_player_destroy(player);
            return 1;
        }

        while (ffplay_player_run_event_loop(1) > 0) {
            /* noop */
        }

        ffplay_player_destroy(player);
        return 0;
    }

    /* Use the legacy blocking CLI */
    return ffplay(argc, (const char **)argv);
}
