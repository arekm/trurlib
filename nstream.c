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
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "trurl_internal.h"
#include "nassert.h"
#include "nmalloc.h"
#include "ndie.h"
#include "niobuf.h"
#include "nstr.h"
#define TN_STREAM_USE_GZIO_NG 1
#include "nstream.h"

#if defined (HAVE_LIBZ) && defined(HAVE_LIBZ_NG)
#define USE_DEFAULT_ZLIB 1
static const char *default_libz = "zlib";
#else
static const char *default_libz = "zlib-ng";
#endif

#ifdef HAVE_LIBZ_NG
void n_stream_zlib_ng_init(tn_stream *st);
#endif

#ifdef HAVE_LIBZ
void n_stream_zlib_init(tn_stream *st);
#endif

/* the bits of a type argument that hold the type itself */
#define TN_STREAM_TYPE_MASK 0xff
#define TN_STREAM_TYPE(t)   ((t) & TN_STREAM_TYPE_MASK)

#define TMP_SUFFIX      ".tmpXXXXXX"
#define TMP_SUFFIX_LEN  (sizeof(TMP_SUFFIX) - 1)

static
tn_stream *atomic_open(const char *path, const char *mode, int type, int flags);

const char *trurl_default_libz()
{
    return default_libz;
}

static int do_s_read(void *stream, void *buf, size_t size)
{
    size_t n = fread(buf, 1, size, stream);

    if (n == 0 && size > 0 && ferror(stream))
        return -1;
    return n;
}

static int do_s_write(void *stream, const void *buf, size_t size)
{
    return fwrite(buf, 1, size, stream);
}

static char *do_s_gets(void *stream, char *buf, size_t size)
{
    return fgets(buf, size, stream);
}

/* path_size leaves room for the temporary name an atomic stream keeps */
static tn_stream *n_stream_new(int type, size_t path_size)
{
    tn_stream *st;

    st = n_malloc(sizeof(*st) + path_size);
    memset(st, 0, sizeof(*st));
    st->fd = -1;
    st->type = (uint8_t)type;

    switch (type) {
        case TN_STREAM_STDIO:
            st->st_open  = (void *(*)(const char *, const char *))fopen;
            st->st_dopen = (void *(*)(int, const char *))fdopen;
            st->st_read  = (int (*)(void*, void*, size_t))do_s_read;
            st->st_write = (int (*)(void*, const void*, size_t))do_s_write;
            st->st_gets  = do_s_gets;
            st->st_getc  = (int (*)(void*))getc;
            st->st_ungetc = (int (*)(int, void*))ungetc;
            st->st_seek  = (int (*)(void*, long, int))fseek;
            st->st_tell  = (long (*)(void*))ftell;
            st->st_flush = (int (*)(void*))fflush;
            st->st_close = (int (*)(void*))fclose;
            break;

        case TN_STREAM_GZIO:
#ifdef USE_DEFAULT_ZLIB
            n_stream_zlib_init(st);
#elifdef HAVE_LIBZ
            n_stream_zlib_init(st);
#elifdef HAVE_LIBZ_NG
            n_stream_zlib_ng_init(st);
#else
            n_die("%d: unsupported stream type\n", type);
#endif
            break;

        case TN_STREAM_GZIO_NG:
#ifdef HAVE_LIBZ_NG
            n_stream_zlib_ng_init(st);
#else
            n_die("%d: unsupported stream type\n", type);
#endif
            break;

         case TN_STREAM_ZSTDIO:
            st->st_open  = (void *(*)(const char *, const char *))n_iobuf_open;
            st->st_dopen = (void *(*)(int, const char *))n_iobuf_dopen;
            st->st_read  = (int (*)(void*, void*, size_t))n_iobuf_read;
            st->st_write = (int (*)(void*, const void*, size_t))n_iobuf_write;
            st->st_gets  = (char *(*)(void*, char*, size_t))n_iobuf_gets;
            st->st_getc  = (int (*)(void*))n_iobuf_getc;
            st->st_ungetc = NULL;
            st->st_seek  = (int (*)(void*, long, int))n_iobuf_seek;
            st->st_tell  = (long (*)(void*))n_iobuf_tell;
            st->st_flush = (int (*)(void*))n_iobuf_flush;
            st->st_close = (int (*)(void*))n_iobuf_close;
            break;

        default:
            n_die("%d: unknown stream type\n", type);
            free(st);               /* a n_die() hook may let us return */
            errno = EINVAL;
            return NULL;
    }

    return st;
}

int n_stream_guess_type(const char *path)
{
    const char *p;
    int type;

    type = TN_STREAM_STDIO;

    if ((p = strrchr(path, '.'))) {
        if (strcmp(p, ".gz") == 0) {
            type = TN_STREAM_GZIO;
        } else if (strcmp(p, ".zst") == 0) {
            type = TN_STREAM_ZSTDIO;
        } else if (strcmp(p, ".ngz") == 0) {
#if HAVE_LIBZ_NG
            type = TN_STREAM_GZIO_NG;
#else
            type = TN_STREAM_GZIO; /* fallback to zlib */
#endif
        }
    }

    return type;
}

/* a .gz, .zst or .ngz name decides, unless the caller named a compression
   type and the path says nothing about it */
static int stream_type(const char *path, int type)
{
    int guessed = n_stream_guess_type(path);

    type = TN_STREAM_TYPE(type);

    if (type == TN_STREAM_UNKNOWN || guessed != TN_STREAM_STDIO)
        type = guessed;

    return type;
}

tn_stream *n_stream_open(const char *path, const char *mode, int type)
{
    int flags;
    tn_stream *st;
    void *stream;

    if (type == -1)             /* TN_STREAM_UNKNOWN was -1 before the flags */
        type = TN_STREAM_UNKNOWN;

    flags = type & ~TN_STREAM_TYPE_MASK;
    type = stream_type(path, type);

    if (flags & ~(TN_STREAM_ATOMIC | TN_STREAM_SYNC)) {
        errno = EINVAL;         /* an unknown flag */
        return NULL;
    }

    if ((flags & TN_STREAM_SYNC) && (flags & TN_STREAM_ATOMIC) == 0) {
        errno = EINVAL;         /* there is no rename to make durable */
        return NULL;
    }

    if (flags & TN_STREAM_ATOMIC)
        return atomic_open(path, mode, type, flags);

    if ((st = n_stream_new(type, 0)) == NULL)
        return NULL;

    if ((stream = st->st_open(path, mode))) {
        st->stream = stream;
    } else {
        n_stream_close(st);
        st = NULL;
    }

    return st;
}

tn_stream *n_stream_dopen(int fd, const char *mode, int type)
{
    tn_stream *st;
    void *stream;

    if (type == -1)             /* TN_STREAM_UNKNOWN was -1 before the flags */
        type = TN_STREAM_UNKNOWN;

    if (type & ~TN_STREAM_TYPE_MASK) {
        errno = EINVAL;         /* no file name to rename the fd to */
        return NULL;
    }

    if ((st = n_stream_new(type, 0)) == NULL)
        return NULL;

    if ((stream = st->st_dopen(fd, mode))) {
        st->stream = stream;
        st->fd = fd;

    } else {
        n_stream_close(st);
        st = NULL;
    }

    return st;
}

/* closes the stream's backend; 0, or -1 with errno set */
static int stream_close(tn_stream *st)
{
    int err = 0;

    if (st->stream && st->st_close(st->stream) != 0)
        err = errno ? errno : EIO;
    st->stream = NULL;

    if (err) {
        errno = err;
        return -1;
    }
    return 0;
}

/* 0, or -1 with errno set; an already missing file is not an error */
static int unlink_tmp(const char *path)
{
    if (unlink(path) == 0 || errno == ENOENT)
        return 0;

    return -1;
}

static int fsync_retry(int fd)
{
    int rc;

    while ((rc = fsync(fd)) != 0 && errno == EINTR)
        ;
    return rc;
}

/* fsync the directory holding target; 0, or the errno of the failure */
static int sync_dir(const char *target)
{
    char *dir = n_strdup(target);
    const char *dirname = n_dirname(dir);
    int fd, err = 0;

    if (dirname == NULL)        /* no '/': the current directory */
        dirname = ".";

    else if (*dirname == '\0')
        dirname = "/";

    if ((fd = open(dirname, O_RDONLY | O_DIRECTORY)) < 0) {
        err = errno;

    } else {
        if (fsync_retry(fd) != 0 && errno != EINVAL && errno != ENOTSUP)
            err = errno;
        close(fd);
    }

    free(dir);

    return err;
}

/* fchmod, close and rename; on any failure the temporary file goes away and
   the target keeps its old contents */
static int atomic_close(tn_stream *st)
{
    int fd = -1, err = 0;

    if (st->werr)               /* the write error is the one to report */
        err = st->werr;
    else if (fchmod(st->fd, st->mode) != 0)  /* 0600 while it was written */
        err = errno;

    /* the descriptor has to outlive st_close(), which writes the trailer of
       a compressed stream and closes the original */
    if (err == 0 && (st->flags & TN_STREAM_SYNC)) {
        if ((fd = dup(st->fd)) < 0)
            err = errno;
    }

    if (stream_close(st) != 0 && err == 0)
        err = errno;

    if (fd >= 0) {
        if (err == 0 && fsync_retry(fd) != 0)
            err = errno;
        close(fd);
    }

    if (err == 0) {
        size_t len = strlen(st->path);
        n_assert(len > TMP_SUFFIX_LEN);
        char *target = n_strdupl(st->path, len - TMP_SUFFIX_LEN);

        if (rename(st->path, target) != 0)
            err = errno;

        /* in place now; a crash could still lose the rename */
        else if (st->flags & TN_STREAM_SYNC)
            err = sync_dir(target);
        free(target);
    }

    if (err != 0) {
        unlink_tmp(st->path);   /* the close error is the one to report */
        errno = err;
        return -1;
    }

    return 0;
}

int n_stream_close(tn_stream *st)
{
    int rc, err = 0;

    if (st->flags & TN_STREAM_ATOMIC)
        rc = atomic_close(st);
    else
        rc = stream_close(st);

    if (rc != 0)
        err = errno;

    free(st);

    if (rc != 0)
        errno = err;
    return rc;
}

int n_stream_abort(tn_stream *st)
{
    int err = 0;

    if ((st->flags & TN_STREAM_ATOMIC) == 0) {
        n_die("non-atomic stream cannot be aborted\n");
        return n_stream_close(st);
    }

    if (stream_close(st) != 0)
        err = errno;

    if (unlink_tmp(st->path) != 0 && err == 0)
        err = errno;

    free(st);

    if (err) {
        errno = err;
        return -1;
    }
    return 0;
}

/* what the replaced file had before, or what a new one gets from the umask */
static mode_t atomic_mode(const char *path)
{
    struct stat sb;
    mode_t mask;

    if (stat(path, &sb) == 0 && S_ISREG(sb.st_mode))
        return sb.st_mode & 0777;   /* neither the owner nor setuid is kept */

    /* umask() is process-wide; the writers of a file are not expected to
       create unrelated files at the same time */
    mask = umask(0);
    umask(mask);
    return 0666 & ~mask;
}

static
tn_stream *atomic_open(const char *path, const char *mode, int type, int flags)
{
    size_t len = strlen(path);
    tn_stream *st;
    int fd, err;

    /* a read or an append stream would rename an empty file over the target */
    if (*mode != 'w') {
        errno = EINVAL;
        return NULL;
    }

    if ((st = n_stream_new(type, len + TMP_SUFFIX_LEN + 1)) == NULL)
        return NULL;

    memcpy(st->path, path, len);
    memcpy(st->path + len, TMP_SUFFIX, TMP_SUFFIX_LEN + 1);

    if ((fd = mkstemp(st->path)) < 0) {
        err = errno;
        free(st);
        errno = err;
        return NULL;
    }

    if ((st->stream = st->st_dopen(fd, mode)) == NULL) {
        err = errno;
        close(fd);
        unlink(st->path);
        free(st);
        errno = err;
        return NULL;
    }

    st->fd = fd;
    st->mode = atomic_mode(path);
    st->flags |= (uint16_t)flags;

    return st;
}

int n_stream_vprintf(tn_stream *st, const char *fmt, va_list args)
{
    char     buf[1024 * 32], *p = buf;
    va_list  ap;
    int      n, nw, err = 0;

    va_copy(ap, args);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n < 0)
        return -1;

    if ((size_t) n >= sizeof(buf)) {
        if ((p = n_malloc((size_t) n + 1)) == NULL)
            return -1;
        vsnprintf(p, (size_t) n + 1, fmt, args);
    }

    nw = n_stream_write(st, p, n);
    if (nw != n)
        err = errno;

    if (p != buf)
        free(p);

    if (nw != n) {
        errno = err;
        return -1;
    }

    return nw;
}

int n_stream_printf(tn_stream *st, const char *fmt, ...)
{
    va_list ap;
    int n;


    va_start(ap, fmt);
    n = n_stream_vprintf(st, fmt, ap);
    va_end(ap);

    return n;
}

inline
int n_stream_gets(tn_stream *st, char *buf, size_t size)
{
    if (st->st_gets(st->stream, buf, size))
		return strlen(buf);

	return 0;
}

#ifndef	MAX_CANON
# define	MAX_CANON	256
#endif
#undef getc
int n_stream_getline(tn_stream *st, char **bufptr, size_t *size)
{
    char *buf = *bufptr;
    size_t n;

    if (buf == NULL) {
        buf = n_malloc(MAX_CANON);
        *size = MAX_CANON;
    }

    *buf = '\0';

    n = n_stream_gets(st, buf, *size);
    if (n < *size - 1 || (n > 0 && buf[n - 1] == '\n')) {
        *bufptr = buf;
        return n;
    }

    while (1) {
        register int c;
        if (n == *size - 1) {
            *size *= 2;
            buf = n_realloc(buf, *size);
        }
        c = st->st_getc(st->stream);
        if (c == EOF)
            break;

        buf[n++] = c;
        if (c == '\n')
            break;
    }

    buf[n] = '\0';
    *bufptr = buf;
    return n;
}
