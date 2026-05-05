/*
 * Copyright (c) 2009 Mark Heily <mark@heily.com>
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

#include "common.h"

extern int kqfd;

/*
 * Resolve where on-disk test files (mkstemp templates, vnode test
 * files etc.) live.  Order: --tmpdir argv override, $TMPDIR, then a
 * platform-sane default.  Returned pointer is process-global and
 * must not be freed.
 */
static char test_tmpdir_buf[1024];

void
test_tmpdir_set(const char *path)
{
    snprintf(test_tmpdir_buf, sizeof(test_tmpdir_buf), "%s", path);
}

const char *
test_tmpdir(void)
{
    if (test_tmpdir_buf[0] != '\0')
        return test_tmpdir_buf;
    {
        const char *e = getenv("TMPDIR");
        if (e != NULL && e[0] != '\0')
            return e;
    }
#ifdef __ANDROID__
    return "/data/local/tmp";
#elif defined(_WIN32)
    /*
     * Win32 has no /tmp.  Resolve via GetTempPathA's standard
     * TMP -> TEMP -> USERPROFILE -> WinDir lookup, strip the
     * trailing backslash so the "%s/foo" snprintf style stays
     * uniform with POSIX, and cache in test_tmpdir_buf.
     */
    {
        DWORD n = GetTempPathA((DWORD) sizeof(test_tmpdir_buf),
                               test_tmpdir_buf);
        if (n == 0 || n > sizeof(test_tmpdir_buf))
            return "C:\\Windows\\Temp";
        if (n > 0 && (test_tmpdir_buf[n - 1] == '\\' ||
                      test_tmpdir_buf[n - 1] == '/'))
            test_tmpdir_buf[n - 1] = '\0';
        return test_tmpdir_buf;
    }
#else
    return "/tmp";
#endif
}

/* Checks if any events are pending, which is an error. */
void
_test_no_kevents(int kqfd, const char *file, int line)
{
    int nfds;
    struct timespec timeo;
    struct kevent kev;

    memset(&timeo, 0, sizeof(timeo));
    nfds = kevent(kqfd, NULL, 0, &kev, 1, &timeo);
    if (nfds < 0)
        err(1, "kevent(2)");
    if (nfds > 0) {
        printf("\n[%s:%d]: Unexpected event:", file, line);
        err(1, "%s", kevent_to_str(&kev));
    }
}

/* Retrieve a single kevent */
void
kevent_get(struct kevent kev[], int numevents, int kqfd, int expect)
{
    struct kevent buf;
    int nfds;

    if (kev == NULL) {
       kev = &buf;
       numevents = 1;
    }

    kevent_rv_cmp(expect, kevent(kqfd, NULL, 0, kev, numevents, NULL));
}

/**
 * Retrieve a single kevent, specifying a maximum time to wait for it.
 * Result:
 * 0 = Timeout
 * 1 = Success
 */
int
kevent_get_timeout(struct kevent kev[], int numevents, int fd, struct timespec *ts)
{
    int nfds;

    nfds = kevent(fd, NULL, 0, kev, numevents, ts);
    if (nfds < 0) {
        err(1, "kevent(2)");
    } else if (nfds == 0) {
        return 0;
    }

    return 1;
}

/* In Linux, a kevent() call with less than 1ms resolution
   will perform a pselect() call to obtain the higer resolution.
   This test exercises that codepath.
 */
void
kevent_get_hires(struct kevent kev[], int numevents, int kqfd, struct timespec *ts)
{
    int nfds;

    nfds = kevent(kqfd, NULL, 0, kev, numevents, ts);
    if (nfds < 1)
        die("kevent(2)");
}

/*
 * This was originally an O(1) array lookup using designated initialisers of
 * the form [~EVFILT_READ] = "EVFILT_READ".  The trick relies on EVFILT_SYSCOUNT
 * being >= the absolute value of every defined EVFILT_ constant.  NetBSD
 * defines EVFILT_FS = -9 but EVFILT_SYSCOUNT = 8, so ~EVFILT_FS = 8 exceeds
 * the array bound and the compiler rejects it.  Switch is good enough for a
 * diagnostic helper.
 */
static const char *
filter_name(short filt)
{
    switch (filt) {
#ifdef EVFILT_READ
    case EVFILT_READ:
        return "EVFILT_READ";
#endif
#ifdef EVFILT_WRITE
    case EVFILT_WRITE:
        return "EVFILT_WRITE";
#endif
#ifdef EVFILT_AIO
    case EVFILT_AIO:
        return "EVFILT_AIO";
#endif
#ifdef EVFILT_VNODE
    case EVFILT_VNODE:
        return "EVFILT_VNODE";
#endif
#ifdef EVFILT_PROC
    case EVFILT_PROC:
        return "EVFILT_PROC";
#endif
#ifdef EVFILT_SIGNAL
    case EVFILT_SIGNAL:
        return "EVFILT_SIGNAL";
#endif
#ifdef EVFILT_TIMER
    case EVFILT_TIMER:
        return "EVFILT_TIMER";
#endif
#ifdef EVFILT_NETDEV
    case EVFILT_NETDEV:
        return "EVFILT_NETDEV";
#endif
#ifdef EVFILT_FS
    case EVFILT_FS:
        return "EVFILT_FS";
#endif
#ifdef EVFILT_LIO
    case EVFILT_LIO:
        return "EVFILT_LIO";
#endif
#ifdef EVFILT_USER
    case EVFILT_USER:
        return "EVFILT_USER";
#endif
#ifdef EVFILT_LIBKQUEUE
    case EVFILT_LIBKQUEUE:
        return "EVFILT_LIBKQUEUE";
#endif
    default:
        return "EVFILT_INVALID";
    }
}

static const char *
kevent_filter_dump(const struct kevent *kev)
{
    static __thread char buf[64];

    snprintf(buf, sizeof(buf), "%d (%s)",
            kev->filter, filter_name(kev->filter));
    return ((const char *) buf);
}

char *
kevent_fflags_dump(struct kevent *kev)
{
    static __thread char buf[512];
    size_t len;

#define KEVFFL_DUMP(attrib) \
    if (kev->fflags & attrib) \
    strncat((char *) buf, #attrib" ", 64);

    snprintf(buf, sizeof(buf), "fflags=0x%04x (", kev->fflags);
    switch (kev->filter) {
#ifdef EVFILT_VNODE
    case EVFILT_VNODE:
        KEVFFL_DUMP(NOTE_DELETE);
#ifdef NOTE_WRITE
        KEVFFL_DUMP(NOTE_WRITE);
#endif
        KEVFFL_DUMP(NOTE_EXTEND);
        KEVFFL_DUMP(NOTE_ATTRIB);
        KEVFFL_DUMP(NOTE_LINK);
#ifdef NOTE_RENAME
        KEVFFL_DUMP(NOTE_RENAME);
#endif
        break;
#endif
#ifdef EVFILT_USER
    case EVFILT_USER:
        KEVFFL_DUMP(NOTE_FFNOP);
        KEVFFL_DUMP(NOTE_FFAND);
        KEVFFL_DUMP(NOTE_FFOR);
        KEVFFL_DUMP(NOTE_FFCOPY);
        KEVFFL_DUMP(NOTE_TRIGGER);
        break;
#endif
#if defined(EVFILT_READ) || defined(EVFILT_WRITE)
# ifdef EVFILT_READ
    case EVFILT_READ:
# endif
# ifdef EVFILT_WRITE
    case EVFILT_WRITE:
# endif
# ifdef NOTE_LOWAT
        KEVFFL_DUMP(NOTE_LOWAT);
# endif
        break;
#endif
#ifdef EVFILT_PROC
    case EVFILT_PROC:
        KEVFFL_DUMP(NOTE_CHILD);
        KEVFFL_DUMP(NOTE_EXIT);
# ifdef NOTE_EXITSTATUS
        KEVFFL_DUMP(NOTE_EXITSTATUS);
# endif
        KEVFFL_DUMP(NOTE_FORK);
        KEVFFL_DUMP(NOTE_EXEC);
# ifdef NOTE_SIGNAL
        KEVFFL_DUMP(NOTE_SIGNAL);
# endif
        break;
#endif

#ifdef EVFILT_LIBKQUEUE
    case EVFILT_LIBKQUEUE:
        KEVFFL_DUMP(NOTE_VERSION);
        KEVFFL_DUMP(NOTE_VERSION_STR);
        break;
#endif

    default:
        break;
    }
    len = strlen(buf);
    if (buf[len - 1] == ' ') buf[len - 1] = '\0';    /* Trim trailing space */
    strcat(buf, ")");

#undef KEVFFL_DUMP

    return (buf);
}

char *
kevent_flags_dump(struct kevent *kev)
{
    static __thread char buf[512];

#define KEVFL_DUMP(attrib) \
    if (kev->flags & attrib) \
    strncat(buf, #attrib" ", 64);

    snprintf(buf, sizeof(buf), "flags = %d (", kev->flags);
    KEVFL_DUMP(EV_ADD);
    KEVFL_DUMP(EV_ENABLE);
    KEVFL_DUMP(EV_DISABLE);
    KEVFL_DUMP(EV_DELETE);
    KEVFL_DUMP(EV_ONESHOT);
    KEVFL_DUMP(EV_CLEAR);
    KEVFL_DUMP(EV_EOF);
    KEVFL_DUMP(EV_ERROR);
#ifdef EV_DISPATCH
    KEVFL_DUMP(EV_DISPATCH);
#endif
#ifdef EV_RECEIPT
    KEVFL_DUMP(EV_RECEIPT);
#endif
    buf[strlen(buf) - 1] = ')';

    return (buf);
}

/* TODO - backport changes from src/common/kevent.c kevent_dump() */
const char *
kevent_to_str(struct kevent *kev)
{
    static __thread char buf[512];

    snprintf(buf, sizeof(buf),
            "[ident=%d, filter=%s, %s, %s, data=%d, udata=%p]",
            (u_int) kev->ident,
            kevent_filter_dump(kev),
            kevent_flags_dump(kev),
            kevent_fflags_dump(kev),
            (int) kev->data,
            kev->udata);

    return buf;
}

void
kevent_update(int kqfd, struct kevent *kev)
{
    if (kevent(kqfd, kev, 1, NULL, 0, NULL) < 0) {
        printf("Unable to add the following kevent:\n%s\n",
                kevent_to_str(kev));
        die("kevent");
    }
}

void
kevent_update_expect_fail(int kqfd, struct kevent *kev)
{
    if (kevent(kqfd, kev, 1, NULL, 0, NULL) >= 0) {
        printf("Performing update should fail");
        die("kevent");
    }
}

void
kevent_add(int kqfd, struct kevent *kev,
        uintptr_t ident,
        short     filter,
        u_short   flags,
        u_int     fflags,
        intptr_t  data,
        void      *udata)
{
    EV_SET(kev, ident, filter, flags, fflags, data, NULL);
    if (kevent(kqfd, kev, 1, NULL, 0, NULL) < 0) {
        printf("Unable to add the following kevent:\n%s\n",
                kevent_to_str(kev));
        die("kevent");
    }
}

/** Check kqueue echo's the event back to use correctly
 *
 * @param[in] kqfd    we're adding events to.
 * @param[out] kev    the event we got passed back in
 *                    the receipt.
 * @param[in] ident   Identifier for the event.
 * @param[in] filter  to add event to.
 * @param[in] flags   to set.
 * @param[in] fflags  to set.
 * @param[in] data    to set.
 * @param[in] udata   to set.
 * @param[in] file    this function was called from.
 * @param[in] line    this function was called from.
 */
void
_kevent_add_with_receipt(int kqfd, struct kevent *kev,
        uintptr_t ident,
        short     filter,
        u_short   flags,
        u_int     fflags,
        intptr_t  data,
        void      *udata,
        char const *file,
        int       line)
{
    struct kevent receipt;

    EV_SET(kev, ident, filter, flags | EV_RECEIPT, fflags, data, NULL);
    if (kevent(kqfd, kev, 1, &receipt, 1, NULL) < 0) {
        printf("Unable to add the following kevent:\n%s\n",
                kevent_to_str(kev));
        die("kevent");
    }


    /*
     * BSD platforms vary in what flags the receipt event echoes back:
     *   macOS / libkqueue: returns EV_ADD | EV_ERROR | EV_RECEIPT
     *   FreeBSD:           returns EV_ADD | EV_ERROR (no EV_RECEIPT)
     *   OpenBSD / NetBSD:  returns EV_ERROR only (no EV_ADD, no EV_RECEIPT)
     * Adjust expected flags to match the platform before comparing, then
     * restore them so callers can use kev for subsequent comparisons.
     */
#if defined(__FreeBSD__)
    kev->flags &= ~EV_RECEIPT;
#elif defined(__OpenBSD__) || defined(__NetBSD__)
    kev->flags &= ~(EV_ADD | EV_RECEIPT);
#endif

    kev->flags |= EV_ERROR;
    _kevent_cmp(kev, &receipt, file, line);
    kev->flags ^= EV_ERROR;

#if defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    kev->flags |= EV_RECEIPT;
#endif
#if defined(__OpenBSD__) || defined(__NetBSD__)
    kev->flags |= EV_ADD;
#endif
}

void
_kevent_cmp(struct kevent *expected, struct kevent *got, const char *file, int line)
{
/* XXX-
   Workaround for inconsistent implementation of kevent(2)
 */
#if defined (__FreeBSD_kernel__) || defined (__FreeBSD__)
    if (expected->flags & EV_ADD)
        got->flags |= EV_ADD;
#endif
    if (memcmp(expected, got, sizeof(*expected)) != 0) {
        printf("[%s:%d]: kevent_cmp() failed:\n", file, line);
        printf("expected %s\n", kevent_to_str(expected));
        printf("but got  %s\n", kevent_to_str(got));
        abort();
    }
}

void
_kevent_rv_cmp(int expected, int got, const char *file, int line)
{
    if (expected != got) {
        printf("[%s:%d]: kevent_rv_cmp() failed:\n", file, line);
        printf("expected %u\n", expected);
        printf("but got  %u\n", got);
        abort();
    }
}
