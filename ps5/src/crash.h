/*
 * Ship of Harkinian for the PlayStation 5 - the crash handler (ps5/src/crash.c).
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Catch SIGSEGV, SIGBUS, SIGILL, SIGFPE and SIGABRT: each writes
 * <data>/logs/crash-<UTC time>.txt (registers, a frame-pointer backtrace named from
 * eboot.sym when there is one, the build id, the log's last lines), then lets the
 * console's own crash handling go on. Called once, from the start-up constructor. */
void ps5_crash_install(const char *data_dir);

/* The game's own lines for a report (scene, room, Link): fn writes at most size
 * bytes of text into buf. Run last, after the report is on disk, since the game's
 * state may be what broke. */
typedef void (*ps5_crash_context_fn)(char *buf, size_t size);
void ps5_crash_set_context(ps5_crash_context_fn fn);

/* The executable's load address, as the crash handler finds it (0x400000 on the
 * consoles seen so far); an address minus this is what llvm-addr2line takes. */
unsigned long ps5_image_base(void);

#ifdef __cplusplus
}
#endif
