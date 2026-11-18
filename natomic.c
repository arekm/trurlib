/*
  Copyright (C) 2026 Pawel A. Gajda <mis@pld-linux.org>

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

#ifdef HAVE_MKOSTEMP
# define _GNU_SOURCE 1          /* mkostemp() is declared only with it */
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nmalloc.h"
#include "natomic.h"

#define SYMLINKS_MAX 40         /* Linux's MAXSYMLINKS */

struct trurl_atomic_private {
    int      fd;
    unsigned flags;
    mode_t   mode;              /* of a new file, from the open's umask */
    char     *target;           /* after tmpath, in the same block */
    char     tmpath[];
};

/* the file a write to path ends up in, as open(..., O_CREAT) would find
   it: the symlinks of the last component followed, a dangling one to its
   (new) target; the directories are left as they are, rename() works
   through them */
static int resolve_target(const char *path, char *out, size_t size)
{
    char        link[PATH_MAX], tmp[PATH_MAX];
    struct stat st;
    const char  *slash;
    ssize_t     len;
    int         n, depth;

    if ((size_t)snprintf(out, size, "%s", path) >= size)
        goto l_toolong;

    for (depth = 0; depth < SYMLINKS_MAX; depth++) {
        if (lstat(out, &st) != 0)
            return errno == ENOENT ? 0 : -1; /* a new file */

        if (S_ISREG(st.st_mode))
            return 0;

        /* a rename() would replace a directory's or a device's node, not
           write into it as an open() does */
        if (!S_ISLNK(st.st_mode)) {
            errno = S_ISDIR(st.st_mode) ? EISDIR : EINVAL;
            return -1;
        }

        if ((len = readlink(out, link, sizeof(link))) == -1)
            return -1;

        if ((size_t)len >= sizeof(link)) /* cut */
            goto l_toolong;
        link[len] = '\0';

        /* a relative target is relative to the link's directory */
        slash = strrchr(out, '/');
        if (link[0] == '/' || slash == NULL)
            n = snprintf(tmp, sizeof(tmp), "%s", link);
        else
            n = snprintf(tmp, sizeof(tmp), "%.*s%s",
                         (int)(slash - out + 1), out, link);

        if (n < 0 || (size_t)n >= sizeof(tmp) || (size_t)n >= size)
            goto l_toolong;

        memcpy(out, tmp, (size_t)n + 1);
    }

    errno = ELOOP;
    return -1;

 l_toolong:
    errno = ENAMETOOLONG;
    return -1;
}

/* <dir>/.<name>.tmp.XXXXXX */
static int tmpname(char *tmpath, size_t size, const char *path)
{
    const char *base = strrchr(path, '/');
    int n;

    if (base != NULL)
        n = snprintf(tmpath, size, "%.*s.%s.tmp.XXXXXX",
                     (int)(base - path + 1), path, base + 1);
    else
        n = snprintf(tmpath, size, ".%s.tmp.XXXXXX", path);

    if (n < 0 || (size_t)n >= size) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}

/* close-on-exec: a child forked while the file is written must not keep
   it open, nor write to it */
static int mktmp(char *tmpath)
{
    int fd;

#ifdef HAVE_MKOSTEMP
    fd = mkostemp(tmpath, O_CLOEXEC);
#else
    /* a fork() between the two still inherits it */
    if ((fd = mkstemp(tmpath)) != -1 && fcntl(fd, F_SETFD, FD_CLOEXEC) == -1) {
        int err = errno;

        close(fd);
        unlink(tmpath);
        errno = err;
        fd = -1;
    }
#endif

    return fd;
}

tn_atomic *n_atomic_open(const char *path, unsigned flags)
{
    char      target[PATH_MAX], tmpath[PATH_MAX];
    tn_atomic *af;
    size_t    len, tlen;
    mode_t    mask;
    int       fd;

    if (flags & ~(unsigned)TN_ATOMIC_SYNC) {
        errno = EINVAL;
        return NULL;
    }

    if (*path == '\0') {         /* as open("") */
        errno = ENOENT;
        return NULL;
    }

    if (resolve_target(path, target, sizeof(target)) != 0)
        return NULL;

    if (tmpname(tmpath, sizeof(tmpath), target) != 0)
        return NULL;

    if ((fd = mktmp(tmpath)) == -1)
        return NULL;

    len = strlen(tmpath) + 1;
    tlen = strlen(target) + 1;

    af = n_malloc(sizeof(*af) + len + tlen);
    memcpy(af->tmpath, tmpath, len);

    af->target = af->tmpath + len;
    memcpy(af->target, target, tlen);

    af->fd = fd;
    af->flags = flags;

    /* umask() is process-wide; the writers of a file are not expected to
       create unrelated files at the same time */
    mask = umask(0);
    umask(mask);
    af->mode = 0666 & ~mask;

    return af;
}

int n_atomic_fd(const tn_atomic *af)
{
    return af->fd;
}

const char *n_atomic_tmpath(const tn_atomic *af)
{
    return af->tmpath;
}

/* the mode and owner of the file replaced, or the mode of a new one.
   fchown() to another user needs privileges, so it is best effort: the
   file stays the writer's */
static int setperms(tn_atomic *af)
{
    struct stat st;

    if (stat(af->target, &st) == 0 && S_ISREG(st.st_mode)) {
        if (fchown(af->fd, st.st_uid, st.st_gid) != 0 && errno != EPERM)
            return -1;

        /* chown() clears them; neither setuid nor setgid is kept anyway */
        return fchmod(af->fd, st.st_mode & 0777);
    }

    return fchmod(af->fd, af->mode);
}

static int fsync_retry(int fd)
{
    int rc;

    while ((rc = fsync(fd)) != 0 && errno == EINTR)
        ;
    return rc;
}

/* without it a crash can keep the old name pointing at the old file; a
   filesystem that cannot sync a directory (EINVAL) does not need to */
static int sync_dir(const char *path)
{
    char        dir[PATH_MAX];
    const char  *slash = strrchr(path, '/');
    int         fd, rc = 0;

    if (slash == NULL)
        snprintf(dir, sizeof(dir), ".");
    else if (slash == path)
        snprintf(dir, sizeof(dir), "/");
    else
        snprintf(dir, sizeof(dir), "%.*s", (int)(slash - path), path);

    if ((fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC)) == -1)
        return -1;

    if (fsync_retry(fd) != 0 && errno != EINVAL && errno != ENOTSUP)
        rc = -1;

    close(fd);
    return rc;
}

int n_atomic_commit(tn_atomic *af)
{
    int err = 0;

    if (setperms(af) != 0)
        err = errno;

    /* a crash after the rename() must not find a cut file under the name */
    else if ((af->flags & TN_ATOMIC_SYNC) && fsync_retry(af->fd) != 0)
        err = errno;

    if (close(af->fd) != 0 && err == 0) /* the data is on disk now or never */
        err = errno;

    if (err == 0 && rename(af->tmpath, af->target) != 0)
        err = errno;

    if (err != 0) {
        unlink(af->tmpath);     /* the commit's error is the one to report */

    } else if (af->flags & TN_ATOMIC_SYNC) {
        if (sync_dir(af->target) != 0) /* replaced, the rename may not last */
            err = errno;
    }

    free(af);

    if (err != 0) {
        errno = err;
        return -1;
    }

    return 0;
}

int n_atomic_abort(tn_atomic *af)
{
    int err = 0;

    if (close(af->fd) != 0)
        err = errno;

    /* an already missing file is not an error */
    if (unlink(af->tmpath) != 0 && errno != ENOENT && err == 0)
        err = errno;

    free(af);

    if (err != 0) {
        errno = err;
        return -1;
    }

    return 0;
}
