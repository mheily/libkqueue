/*
 * Copyright (c) 2022 Arran Cudbard-Bell <a.cudbardb@freeradius.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#ifndef NDEBUG

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "debug.h"
#include "private.h"

bool libkqueue_debug = false;

/** Prefix for libkqueue debug messages
 */
char const *libkqueue_debug_ident = "KQ";

/** Copy of the externally provided identity string
 */
static char *libkqueue_debug_ident_copy;

static void dbg_stderr(char const *fmt, ...);

/** Custom logging function.
 *
 * Defaults to dbg_stderr.  Backends that prefer a different default
 * (e.g. Win32's OutputDebugStringA route, which sidesteps the
 * conhost synchronous-write bottleneck that made KQUEUE_DEBUG=1
 * take 30 minutes vs 10 seconds) implement
 * kqops.libkqueue_dbg_default; libkqueue_debug_func_init consults
 * it on first kqueue().
 */
dbg_func_t libkqueue_debug_func = dbg_stderr;

static void
dbg_stderr(char const *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/** Set a new logging debug function
 *
 * NULL resets to the per-platform default (kqops.libkqueue_dbg_default
 * if the backend provides one, else dbg_stderr).
 */
void
libkqueue_debug_func_set(dbg_func_t func)
{
    if (!func) {
        if (kqops.libkqueue_dbg_default)
            libkqueue_debug_func = kqops.libkqueue_dbg_default();
        else
            libkqueue_debug_func = dbg_stderr;
    } else {
        libkqueue_debug_func = func;
    }
}

/** One-shot init from libkqueue_init: resolve the platform default.
 *
 * Only intended to be called once, while we hold kq_mtx during the
 * first kqueue() call - so no atomic protection here.  No-op if the
 * consumer already swapped in a custom func via
 * libkqueue_debug_func_set().
 */
void
libkqueue_debug_func_init(void)
{
    if (libkqueue_debug_func == dbg_stderr && kqops.libkqueue_dbg_default)
        libkqueue_debug_func = kqops.libkqueue_dbg_default();
}

/** Set a new debug identity
 */
void
libkqueue_debug_ident_set(char const *str)
{
    free(libkqueue_debug_ident_copy);
    libkqueue_debug_ident_copy = strdup(str);
    libkqueue_debug_ident = libkqueue_debug_ident_copy;
}

/** Clear any previously allocated identities
 */
TSAN_IGNORE
void
libkqueue_debug_ident_clear(void)
{
    libkqueue_debug_ident = "";
    free(libkqueue_debug_ident_copy);
    libkqueue_debug_ident_copy = NULL;
}
#endif
