// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

/*
 * ccap_capture C ABI verification program
 *
 * Purpose: prove that the shared library (.dll / .so) can be called by
 * **external programs**, not just linked against by ccap itself.
 *
 * This program **deliberately does not link** the ccap_capture static
 * library —— it depends only on the C header and the shared library.
 * If it runs, that means:
 *   1. The exported symbols are correct (dllexport / visibility are right)
 *   2. The C ABI is self-contained (no specific C++ runtime version needed)
 *   3. The event callback works
 *
 * Usage:
 *   test_capi <path-to-ccap_capture.dll|so> <target> [args...]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ccap_capture.h"

static int g_events = 0;
static int g_notes = 0;

static void onEvent(void* user, const char* json_line, size_t length) {
    (void)user;
    (void)length;
    g_events++;
    /* Only print the first 120 characters to avoid flooding the screen */
    printf("EVENT: %.120s\n", json_line);
}

static void onNote(void* user, const char* note) {
    (void)user;
    g_notes++;
    printf("NOTE: %s\n", note);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <lib> <target> [args...]\n", argv[0]);
        return 2;
    }

    printf("ccap_capture version: %s\n", ccap_version());
    printf("platform: %s\n", ccap_platform_name(ccap_current_platform()));

    ccap_session* session = ccap_session_create();
    if (!session) {
        fprintf(stderr, "FAIL: cannot create session\n");
        return 1;
    }

    ccap_run_options opts;
    ccap_run_options_init(&opts);
    opts.scan_script = 0;      /* runtime events only */

    const char* target = argv[2];
    const char* args[64];
    size_t argCount = 0;
    for (int i = 3; i < argc && argCount < 64; ++i) {
        args[argCount++] = argv[i];
    }

    int rc = ccap_session_run(session, target, args, argCount, &opts,
                              onEvent, onNote, NULL);

    printf("rc=%d events=%d notes=%d\n", rc, g_events, g_notes);

    ccap_session_destroy(session);

    if (g_events <= 0) {
        fprintf(stderr, "FAIL: no events captured\n");
        return 1;
    }
    printf("PASS: C ABI works (%d event(s))\n", g_events);
    return 0;
}
