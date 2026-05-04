/*
 * Copyright (c) 2026 Arran Cudbard-Bell <a.cudbardb@freeradius.org>
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

#include "../common/private.h"

/*
 * Synchronous per-line WriteFile to conhost dominates the runtime of
 * KQUEUE_DEBUG=1 on Win32 (30 minutes vs 10 seconds to reach the
 * same point).  Route debug output through
 * OutputDebugStringA's kernel-side ring buffer instead - DebugView,
 * WinDbg, and the VS Output pane all pick it up - and let the user
 * override via KQUEUE_DEBUG_OUTPUT=stderr|odbg|both.
 */

static void
dbg_odbg(char const *fmt, ...)
{
    static __thread char buf[2048];
    va_list ap;

    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = '\0';
    OutputDebugStringA(buf);
}

static void
dbg_both(char const *fmt, ...)
{
    static __thread char buf[2048];
    va_list ap;

    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = '\0';
    fputs(buf, stderr);
    OutputDebugStringA(buf);
}

static void
dbg_stderr_local(char const *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

dbg_func_t
windows_dbg_default(void)
{
    char const *e = getenv("KQUEUE_DEBUG_OUTPUT");
    if (e) {
        if (!strcmp(e, "stderr")) return dbg_stderr_local;
        if (!strcmp(e, "both"))   return dbg_both;
    }
    return dbg_odbg;
}

#endif /* NDEBUG */
