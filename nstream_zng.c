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

#if HAVE_LIBZ_NG
#define WITH_GZFILEOP 1
#include <zlib-ng/zlib-ng.h>

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
    int    reading;             /* opened with neither 'w' nor 'a' */
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

    zng_gzerror(gs->gz, &err);
    if (gs->err == Z_OK)
        gs->err = err;
    return gs->err != Z_OK;
}

static int do_ngz_read(void *stream, void *buf, size_t size)
{
    int n, failed;

    if (size == 0)              /* gzread() would return an earlier error */
        return 0;
    n = zng_gzread(gz_of(stream), buf, size);
    failed = note_error(stream);
    return n == 0 && failed ? -1 : n;
}

static int do_ngz_write(void *stream, const void *buf, size_t size)
{
    return zng_gzwrite(gz_of(stream), buf, size);
}

static char *do_ngz_gets(void *stream, char *buf, size_t size)
{
    char *s = zng_gzgets(gz_of(stream), buf, size);

    note_error(stream);
    return s;
}

/* 0 for a stream that reads, as from a plain and a zstd stream: gzflush()
   refuses it, which n_stream_flush() takes for a write that failed */
static int do_ngz_flush(void *stream)
{
    struct gz_stream *gs = stream;

    if (gs->reading)
        return Z_OK;
    return zng_gzflush(gs->gz, Z_FULL_FLUSH);
}

static long do_ngz_tell(void *stream)
{
    return zng_gztell(gz_of(stream));
}

static int do_ngz_getc(void *stream)
{
    register int c = zng_gzgetc(gz_of(stream));

    note_error(stream);
    if (c == -1)
        return EOF;

    return c;
}

static int do_ngz_ungetc(int c, void *stream)
{
    register int cc = zng_gzungetc(c, gz_of(stream));
    if (cc == -1)
        return EOF;
    n_assert(cc == c);
    return cc;
}

static int ngz_fseek_wrap(void *stream, long offset, int whence)
{
    z_off_t rc, off = offset;

#if ZLIB_TRACE_SEEK
    int seek = 0;
    switch (whence) {
    case SEEK_CUR:
        seek = zng_gztell(gz_of(stream)) + offset;
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

    if (seek < zng_gztell(gz_of(stream))) {
        DBGF_F("%p backward from %lld => %ld (whence %d)\n", stream, zng_gztell(gz_of(stream)), offset, whence);
    }
#endif

    rc = zng_gzseek(gz_of(stream), off, whence);

#if ZLIB_TRACE
    if (rc < 0) {
        int ec;
        printf("zlib_fseek: %s\n", zng_gzerror(gz_of(stream), &ec));
    }
#endif

    if (rc > 0)
        rc = 0;

    return rc;
}

void *do_ngz_open(const char *path, const char *mode)
{
    struct gz_stream *gs = n_calloc(1, sizeof(*gs));
    errno = 0;

    if ((gs->gz = zng_gzopen(path, mode)) == NULL) {
        if (errno == 0) {
            if (Z_MEM_ERROR)
                errno = ENOMEM;
            else
                errno = EIO;
        }
        free(gs);
        return NULL;
    }

    gs->reading = strpbrk(mode, "wa") == NULL;
    return gs;
}

static void *do_ngz_dopen(int fd, const char *mode)
{
    struct gz_stream *gs = n_calloc(1, sizeof(*gs));

    if ((gs->gz = zng_gzdopen(fd, mode)) == NULL) {
        free(gs);
        return NULL;
    }

    gs->reading = strpbrk(mode, "wa") == NULL;
    return gs;
}

static int do_ngz_close(void *stream)
{
    int rc = zng_gzclose(gz_of(stream));

    if (rc == Z_OK)
        rc = ((struct gz_stream *) stream)->err;
    free(stream);
    return rc;
}

void n_stream_zlib_ng_init(tn_stream *st) {
    st->st_open  = do_ngz_open;
    st->st_dopen = do_ngz_dopen;
    st->st_read  = do_ngz_read;
    st->st_write = do_ngz_write;
    st->st_gets  = do_ngz_gets;
    st->st_getc  = do_ngz_getc;
    st->st_ungetc = do_ngz_ungetc;
    st->st_seek  = ngz_fseek_wrap;
    st->st_tell  = do_ngz_tell;
    st->st_flush = do_ngz_flush;
    st->st_close = do_ngz_close;
}

#endif  /* HAVE_LIBZ_NG */
