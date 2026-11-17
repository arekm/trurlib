/*
  Copyright (C) 2002 - 2007 Pawel A. Gajda <mis@pld-linux.org>

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU Library General Public License, version 2 as
  published by the Free Software Foundation (see file COPYING for details).

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#if HAVE_LIBZ
#include <zlib.h>

#include "trurl_internal.h"
#include "nassert.h"
#include "nmalloc.h"
#include "nstream.h"
#include "ndie.h"

#define ZLIB_TRACE 0

/* what st->stream of a gzip stream points to */
struct gz_stream {
    gzFile gz;
    int    err;                 /* the first error of a read, Z_OK if none */
};

static inline gzFile gz_of(void *stream)
{
    return ((struct gz_stream *) stream)->gz;
}

/* gzclose() reports neither a damaged member (Z_DATA_ERROR) nor a read
   that failed (Z_ERRNO), and a cut (Z_BUF_ERROR) only until zlib clears it
   at the next call or zlib-ng at a seek back, so the first error is kept */
static int note_error(void *stream)
{
    struct gz_stream *gs = stream;
    int err;

    gzerror(gs->gz, &err);
    if (gs->err == Z_OK)
        gs->err = err;
    return gs->err != Z_OK;
}

static int do_gz_read(void *stream, void *buf, size_t size)
{
    int n, failed;

    if (size == 0)              /* gzread() would return an earlier error */
        return 0;
    n = gzread(gz_of(stream), buf, size);
    failed = note_error(stream);
    return n == 0 && failed ? -1 : n;
}

static int do_gz_write(void *stream, const void *buf, size_t size)
{
    return gzwrite(gz_of(stream), buf, size);
}

static char *do_gz_gets(void *stream, char *buf, size_t size)
{
    char *s = gzgets(gz_of(stream), buf, size);

    note_error(stream);
    return s;
}


static int do_gz_flush(void *stream)
{
    return gzflush(gz_of(stream), Z_FULL_FLUSH);
}

static long do_gz_tell(void *stream)
{
    return gztell(gz_of(stream));
}

static int do_gz_getc(void *stream)
{
    register int c = gzgetc(gz_of(stream));

    note_error(stream);
    if (c == -1)
        return EOF;

    return c;
}

#if HAVE_GZUNGETC               /* zlib >= 1.2.0.2 */
static int do_gz_ungetc(int c, void *stream)
{
    register int cc = gzungetc(c, gz_of(stream));
    if (cc == -1)
        return EOF;
    n_assert(cc == c);
    return cc;
}
#endif

static int gz_fseek_wrap(void *stream, long offset, int whence)
{
    z_off_t rc, off = offset;

#if ZLIB_TRACE_SEEK
    int seek = 0;
    switch (whence) {
    case SEEK_CUR:
        seek = gztell(gz_of(stream)) + offset;
        break;

    case SEEK_SET:
        seek = offset;
        break;

    case SEEK_END:
        n_die("SEEK_END is not supported\n");
        break;

    default:
        n_die("iobuf: unknown whence (%d)\n", whence);
        break;
    }

    if (seek < gztell(gz_of(stream))) {
        DBGF_F("%p backward from %lld => %ld (whence %d)\n", stream, gztell(gz_of(stream)), offset, whence);
    }
#endif

    rc = gzseek(gz_of(stream), off, whence);

#if ZLIB_TRACE
    if (rc < 0) {
        int ec;
        printf("zlib_fseek: %s\n", gzerror(gz_of(stream), &ec));
    }
#endif

    if (rc > 0)
        rc = 0;

    return rc;
}

void *do_gz_open(const char *path, const char *mode)
{
    struct gz_stream *gs = n_calloc(1, sizeof(*gs));
    errno = 0;

    if ((gs->gz = gzopen(path, mode)) == NULL) {
        if (errno == 0) {
            if (Z_MEM_ERROR)
                errno = ENOMEM;
            else
                errno = EIO;
        }
        free(gs);
        return NULL;
    }

    return gs;
}

static void *do_gz_dopen(int fd, const char *mode)
{
    struct gz_stream *gs = n_calloc(1, sizeof(*gs));

    if ((gs->gz = gzdopen(fd, mode)) == NULL) {
        free(gs);
        return NULL;
    }

    return gs;
}

static int do_gz_close(void *stream)
{
    int rc = gzclose(gz_of(stream));

    if (rc == Z_OK)
        rc = ((struct gz_stream *) stream)->err;
    free(stream);
    return rc;
}

void n_stream_zlib_init(tn_stream *st) {
    st->st_open  = do_gz_open;
    st->st_dopen = do_gz_dopen;
    st->st_read  = do_gz_read;
    st->st_write = do_gz_write;
    st->st_gets  = do_gz_gets;
    st->st_getc  = do_gz_getc;
#if HAVE_GZUNGETC
    st->st_ungetc = do_gz_ungetc;
#else
    st->st_ungetc = NULL;
#endif
    st->st_seek  = gz_fseek_wrap;
    st->st_tell  = do_gz_tell;
    st->st_flush = do_gz_flush;
    st->st_close = do_gz_close;
}

#endif  /* HAVE_LIBZ */
