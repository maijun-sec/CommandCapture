// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

/*
 * ccap_capture - C ABI
 *
 * The **stable public interface** to the CommandCapture capture capability.
 *
 * Why a C ABI instead of exposing C++ directly:
 *   The C++ ABI is not stable across compilers / across versions -- the same
 *   header file compiled
 *   with MSVC and compiled with GCC has different symbol names and different
 *   object layouts.
 *   As soon as a shared library has to serve bindings for several languages,
 *   it has to go through a C ABI.
 *
 * Consumers:
 *   - Python: ctypes.CDLL("ccap_capture.so")
 *   - Go:     cgo
 *   - Rust:   bindgen
 *   - any language that can call FFI
 *
 * Lifecycle:
 *   ccap_session_create()  ->  ccap_session_run()  ->  ccap_session_destroy()
 *
 * Thread safety:
 *   A single session is not thread safe (there is a global mutex protecting the
 *   event collector internally,
 *   but run() should not be called concurrently). Separate sessions do not
 *   affect each other.
 */

#ifndef CCAP_CAPTURE_H
#define CCAP_CAPTURE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  if defined(CCAP_CAPTURE_BUILD_DLL)
#    define CCAP_API __declspec(dllexport)
#  elif defined(CCAP_CAPTURE_USE_DLL)
#    define CCAP_API __declspec(dllimport)
#  else
#    define CCAP_API
#  endif
#else
#  if defined(CCAP_CAPTURE_BUILD_DLL)
#    define CCAP_API __attribute__((visibility("default")))
#  else
#    define CCAP_API
#  endif
#endif

/* ── Version ───────────────────────────────────────────────────────────────
 * Semantic versioning. Increment major on an ABI change (incompatible),
 * increment minor when only adding features.
 */
#define CCAP_CAPTURE_VERSION_MAJOR 1
#define CCAP_CAPTURE_VERSION_MINOR 0
#define CCAP_CAPTURE_VERSION_PATCH 0

/* ── Platform ────────────────────────────────────────────────────────────── */
typedef enum {
    CCAP_PLATFORM_UNKNOWN = 0,
    CCAP_PLATFORM_WINDOWS = 1,
    CCAP_PLATFORM_LINUX   = 2
} ccap_platform;

/* ── Event source ────────────────────────────────────────────────────────── */
typedef enum {
    CCAP_SOURCE_RUNTIME = 0,   /* a process was really created */
    CCAP_SOURCE_STATIC  = 1    /* a builtin command found by script static analysis */
} ccap_event_source;

/* ── Run options ──────────────────────────────────────────────────────────
 *
 * The field names use snake_case -- this is the convention of the C ABI
 * (LLVM's clang-c API does the same,
 * `clang_visitChildren(..., CXClientData client_data)`).
 *
 * The C ABI is an **external contract**: changing it breaks compatibility. So
 * it does not follow
 * the camelBack convention of the C++ side -- the two are two different
 * languages with two different conventions.
 */
typedef struct {
    const char* cwd;                 /* working directory, NULL = current directory */
    int         scan_script;         /* whether to do script static analysis */
    int         script_recursive;
    int         script_depth;        /* static analysis recursion depth */
    int         timeout_ms;          /* 0 = no limit */
    int         quiet_ms;            /* quiet period after the root process exits */
    int         verbose;
    int         clear_being_debugged; /* Windows only */
    int         redirect_output_to_stderr;
} ccap_run_options;

/* Fill options with the default values (every field gets a safe default) */
CCAP_API void ccap_run_options_init(ccap_run_options* options);

/* ── Callbacks ─────────────────────────────────────────────────────────────
 * on_event: for every event received, one line of JSON is passed in (**without**
 *           the trailing newline).
 *           ccap reuses that memory after the callback returns -- copy it
 *           yourself if you need to keep it.
 * on_note:  diagnostic information (notes). Also one line of text at a time.
 * user:     user context, passed back to the callbacks unchanged.
 */
typedef void (*ccap_event_callback)(void* user, const char* json_line, size_t length);
typedef void (*ccap_note_callback)(void* user, const char* note);

/* ── Session ─────────────────────────────────────────────────────────────── */
typedef struct ccap_session ccap_session;

/* Create a session. Returns NULL on failure. */
CCAP_API ccap_session* ccap_session_create(void);

/* Destroy a session. NULL may be passed (no operation). */
CCAP_API void ccap_session_destroy(ccap_session* session);

/*
 * Run one capture.
 *
 * target / args must be strings whose **lifetime covers this call**
 * (ccap does not touch them after the function returns).
 *
 * Return value:
 *   0  success (the target finished; this does not mean there are events)
 *   1  the target finished, but the capture had errors (see the note callback)
 *   2  invalid arguments
 *   3  the target could not be started at all
 */
CCAP_API int ccap_session_run(ccap_session* session,
                              const char* target,
                              const char* const* args,
                              size_t argCount,
                              const ccap_run_options* options,
                              ccap_event_callback onEvent,
                              ccap_note_callback onNote,
                              void* user);

/* ── Convenience functions ──────────────────────────────────────────────── */

/* The current platform */
CCAP_API ccap_platform ccap_current_platform(void);

/* The platform name ("windows" / "linux" / "unknown") */
CCAP_API const char* ccap_platform_name(ccap_platform platform);

/* The version string ("1.0.0") */
CCAP_API const char* ccap_version(void);

/*
 * Script static analysis only.
 *
 * Returns the number of events. The events are also delivered through the
 * on_event callback.
 */
CCAP_API int ccap_session_scan(ccap_session* session,
                                const char* scriptPath,
                                const ccap_run_options* options,
                                ccap_event_callback onEvent,
                                ccap_note_callback onNote,
                                void* user);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* CCAP_CAPTURE_H */
