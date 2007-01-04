/* $Id$ */

/***
  This file is part of PulseAudio.

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <pulse/xmalloc.h>

#include <pulsecore/core-error.h>
#include <pulsecore/log.h>

#include "ioline.h"

#define BUFFER_LIMIT (64*1024)
#define READ_SIZE (1024)

struct pa_ioline {
    pa_iochannel *io;
    pa_defer_event *defer_event;
    pa_mainloop_api *mainloop;
    int ref;
    int dead;

    char *wbuf;
    size_t wbuf_length, wbuf_index, wbuf_valid_length;

    char *rbuf;
    size_t rbuf_length, rbuf_index, rbuf_valid_length;

    void (*callback)(pa_ioline*io, const char *s, void *userdata);
    void *userdata;

    int defer_close;
};

static void io_callback(pa_iochannel*io, void *userdata);
static void defer_callback(pa_mainloop_api*m, pa_defer_event*e, void *userdata);

pa_ioline* pa_ioline_new(pa_iochannel *io) {
    pa_ioline *l;
    assert(io);

    l = pa_xnew(pa_ioline, 1);
    l->io = io;
    l->dead = 0;

    l->wbuf = NULL;
    l->wbuf_length = l->wbuf_index = l->wbuf_valid_length = 0;

    l->rbuf = NULL;
    l->rbuf_length = l->rbuf_index = l->rbuf_valid_length = 0;

    l->callback = NULL;
    l->userdata = NULL;
    l->ref = 1;

    l->mainloop = pa_iochannel_get_mainloop_api(io);

    l->defer_event = l->mainloop->defer_new(l->mainloop, defer_callback, l);
    l->mainloop->defer_enable(l->defer_event, 0);

    l->defer_close = 0;

    pa_iochannel_set_callback(io, io_callback, l);

    return l;
}

static void ioline_free(pa_ioline *l) {
    assert(l);

    if (l->io)
        pa_iochannel_free(l->io);

    if (l->defer_event)
        l->mainloop->defer_free(l->defer_event);

    pa_xfree(l->wbuf);
    pa_xfree(l->rbuf);
    pa_xfree(l);
}

void pa_ioline_unref(pa_ioline *l) {
    assert(l);
    assert(l->ref >= 1);

    if ((--l->ref) <= 0)
        ioline_free(l);
}

pa_ioline* pa_ioline_ref(pa_ioline *l) {
    assert(l);
    assert(l->ref >= 1);

    l->ref++;
    return l;
}

void pa_ioline_close(pa_ioline *l) {
    assert(l);
    assert(l->ref >= 1);

    l->dead = 1;

    if (l->io) {
        pa_iochannel_free(l->io);
        l->io = NULL;
    }

    if (l->defer_event) {
        l->mainloop->defer_free(l->defer_event);
        l->defer_event = NULL;
    }

    if (l->callback)
        l->callback = NULL;
}

void pa_ioline_puts(pa_ioline *l, const char *c) {
    size_t len;

    assert(l);
    assert(l->ref >= 1);
    assert(c);

    if (l->dead)
        return;

    len = strlen(c);
    if (len > BUFFER_LIMIT - l->wbuf_valid_length)
        len = BUFFER_LIMIT - l->wbuf_valid_length;

    if (len) {
        assert(l->wbuf_length >= l->wbuf_valid_length);

        /* In case the allocated buffer is too small, enlarge it. */
        if (l->wbuf_valid_length + len > l->wbuf_length) {
            size_t n = l->wbuf_valid_length+len;
            char *new = pa_xmalloc(n);
            if (l->wbuf) {
                memcpy(new, l->wbuf+l->wbuf_index, l->wbuf_valid_length);
                pa_xfree(l->wbuf);
            }
            l->wbuf = new;
            l->wbuf_length = n;
            l->wbuf_index = 0;
        } else if (l->wbuf_index + l->wbuf_valid_length + len > l->wbuf_length) {

            /* In case the allocated buffer fits, but the current index is too far from the start, move it to the front. */
            memmove(l->wbuf, l->wbuf+l->wbuf_index, l->wbuf_valid_length);
            l->wbuf_index = 0;
        }

        assert(l->wbuf_index + l->wbuf_valid_length + len <= l->wbuf_length);

        /* Append the new string */
        memcpy(l->wbuf + l->wbuf_index + l->wbuf_valid_length, c, len);
        l->wbuf_valid_length += len;

        l->mainloop->defer_enable(l->defer_event, 1);
    }
}

void pa_ioline_set_callback(pa_ioline*l, void (*callback)(pa_ioline*io, const char *s, void *userdata), void *userdata) {
    assert(l);
    assert(l->ref >= 1);

    l->callback = callback;
    l->userdata = userdata;
}

static void failure(pa_ioline *l, int process_leftover) {
    assert(l);
    assert(l->ref >= 1);
    assert(!l->dead);

    if (process_leftover && l->rbuf_valid_length > 0) {
        /* Pass the last missing bit to the client */

        if (l->callback) {
            char *p = pa_xstrndup(l->rbuf+l->rbuf_index, l->rbuf_valid_length);
            l->callback(l, p, l->userdata);
            pa_xfree(p);
        }
    }

    if (l->callback) {
        l->callback(l, NULL, l->userdata);
        l->callback = NULL;
    }

    pa_ioline_close(l);
}

static void scan_for_lines(pa_ioline *l, size_t skip) {
    assert(l && l->ref >= 1 && skip < l->rbuf_valid_length);

    while (!l->dead && l->rbuf_valid_length > skip) {
        char *e, *p;
        size_t m;

        if (!(e = memchr(l->rbuf + l->rbuf_index + skip, '\n', l->rbuf_valid_length - skip)))
            break;

        *e = 0;

        p = l->rbuf + l->rbuf_index;
        m = strlen(p);

        l->rbuf_index += m+1;
        l->rbuf_valid_length -= m+1;

        /* A shortcut for the next time */
        if (l->rbuf_valid_length == 0)
            l->rbuf_index = 0;

        if (l->callback)
            l->callback(l, p, l->userdata);

        skip = 0;
    }

    /* If the buffer became too large and still no newline was found, drop it. */
    if (l->rbuf_valid_length >= BUFFER_LIMIT)
        l->rbuf_index = l->rbuf_valid_length = 0;
}

static int do_write(pa_ioline *l);

static int do_read(pa_ioline *l) {
    assert(l && l->ref >= 1);

    while (!l->dead && pa_iochannel_is_readable(l->io)) {
        ssize_t r;
        size_t len;

        len = l->rbuf_length - l->rbuf_index - l->rbuf_valid_length;

        /* Check if we have to enlarge the read buffer */
        if (len < READ_SIZE) {
            size_t n = l->rbuf_valid_length+READ_SIZE;

            if (n >= BUFFER_LIMIT)
                n = BUFFER_LIMIT;

            if (l->rbuf_length >= n) {
                /* The current buffer is large enough, let's just move the data to the front */
                if (l->rbuf_valid_length)
                    memmove(l->rbuf, l->rbuf+l->rbuf_index, l->rbuf_valid_length);
            } else {
                /* Enlarge the buffer */
                char *new = pa_xmalloc(n);
                if (l->rbuf_valid_length)
                    memcpy(new, l->rbuf+l->rbuf_index, l->rbuf_valid_length);
                pa_xfree(l->rbuf);
                l->rbuf = new;
                l->rbuf_length = n;
            }

            l->rbuf_index = 0;
        }

        len = l->rbuf_length - l->rbuf_index - l->rbuf_valid_length;

        assert(len >= READ_SIZE);

        /* Read some data */
        if ((r = pa_iochannel_read(l->io, l->rbuf+l->rbuf_index+l->rbuf_valid_length, len)) <= 0) {
            if (r < 0) {
                pa_log("read(): %s", pa_cstrerror(errno));
                failure(l, 0);
            } else
                failure(l, 1);

            return -1;
        }

        l->rbuf_valid_length += r;

        /* Look if a line has been terminated in the newly read data */
        scan_for_lines(l, l->rbuf_valid_length - r);
    }

    return 0;
}

/* Try to flush the buffer */
static int do_write(pa_ioline *l) {
    ssize_t r;
    assert(l && l->ref >= 1);

    while (!l->dead && pa_iochannel_is_writable(l->io) && l->wbuf_valid_length) {

        if ((r = pa_iochannel_write(l->io, l->wbuf+l->wbuf_index, l->wbuf_valid_length)) < 0) {
            pa_log("write(): %s", r < 0 ? pa_cstrerror(errno) : "EOF");
            failure(l, 0);
            return -1;
        }

        l->wbuf_index += r;
        l->wbuf_valid_length -= r;

        /* A shortcut for the next time */
        if (l->wbuf_valid_length == 0)
            l->wbuf_index = 0;
    }

    return 0;
}

/* Try to flush read/write data */
static void do_work(pa_ioline *l) {
    assert(l);
    assert(l->ref >= 1);

    pa_ioline_ref(l);

    l->mainloop->defer_enable(l->defer_event, 0);

    if (!l->dead)
        do_read(l);

    if (!l->dead)
        do_write(l);

    if (l->defer_close && !l->wbuf_valid_length)
        failure(l, 1);

    pa_ioline_unref(l);
}

static void io_callback(pa_iochannel*io, void *userdata) {
    pa_ioline *l = userdata;
    assert(io && l && l->ref >= 1);

    do_work(l);
}

static void defer_callback(pa_mainloop_api*m, pa_defer_event*e, void *userdata) {
    pa_ioline *l = userdata;
    assert(l && l->ref >= 1 && l->mainloop == m && l->defer_event == e);

    do_work(l);
}

void pa_ioline_defer_close(pa_ioline *l) {
    assert(l);
    assert(l->ref >= 1);

    l->defer_close = 1;

    if (!l->wbuf_valid_length)
        l->mainloop->defer_enable(l->defer_event, 1);
}

void pa_ioline_printf(pa_ioline *l, const char *format, ...) {
    char *t;
    va_list ap;

    assert(l);
    assert(l->ref >= 1);

    va_start(ap, format);
    t = pa_vsprintf_malloc(format, ap);
    va_end(ap);

    pa_ioline_puts(l, t);
    pa_xfree(t);
}
