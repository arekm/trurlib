#define _GNU_SOURCE 1           /* RTLD_NEXT */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#include "n_check.h"
#include "natomic.h"

/* libtrurl resolves these through the executable's definitions, so the
   test can watch them and fail them on demand; the real ones are found
   with dlsym(RTLD_NEXT) */
static int fsync_fail;          /* 1 the file's, 2 the dir's EIO, 3 EINVAL */
static int file_syncs, dir_syncs, file_synced_renamed, dir_cloexec;
static int rename_fails, renamed;
static int close_fail_fd = -1;
static int fchown_calls, fchown_errno;
static uid_t fchown_uid;
static gid_t fchown_gid;

#define REAL(name) real_##name = dlsym(RTLD_NEXT, #name)

int fsync(int fd)
{
    static int (*real_fsync)(int);
    struct stat sb;

    if (real_fsync == NULL)
        REAL(fsync);

    if (fstat(fd, &sb) == 0 && S_ISDIR(sb.st_mode)) {
        dir_syncs++;
        dir_cloexec = (fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0;
        if (fsync_fail == 2 || fsync_fail == 3) {
            errno = fsync_fail == 2 ? EIO : EINVAL;
            return -1;
        }

    } else {
        file_syncs++;
        file_synced_renamed = renamed;
        if (fsync_fail == 1) {
            errno = EIO;
            return -1;
        }
    }

    return real_fsync(fd);
}

int rename(const char *oldpath, const char *newpath)
{
    static int (*real_rename)(const char *, const char *);

    if (real_rename == NULL)
        REAL(rename);

    if (rename_fails) {
        errno = EIO;
        return -1;
    }

    renamed++;
    return real_rename(oldpath, newpath);
}

/* the descriptor is closed anyway, as on Linux */
int close(int fd)
{
    static int (*real_close)(int);

    if (real_close == NULL)
        REAL(close);

    if (fd == close_fail_fd) {
        close_fail_fd = -1;
        real_close(fd);
        errno = EIO;
        return -1;
    }

    return real_close(fd);
}

int fchown(int fd, uid_t uid, gid_t gid)
{
    static int (*real_fchown)(int, uid_t, gid_t);

    if (real_fchown == NULL)
        REAL(fchown);

    fchown_calls++;
    fchown_uid = uid;
    fchown_gid = gid;

    if (fchown_errno) {
        errno = fchown_errno;
        return -1;
    }

    return real_fchown(fd, uid, gid);
}

/* <dir>/.<name>.tmp.XXXXXX */
static int tmp_files(const char *path)
{
    const char *base = strrchr(path, '/');
    char pattern[PATH_MAX + 16];
    glob_t g;
    int n;

    if (base != NULL)
        snprintf(pattern, sizeof(pattern), "%.*s.%s.tmp.??????",
                 (int)(base - path + 1), path, base + 1);
    else
        snprintf(pattern, sizeof(pattern), ".%s.tmp.??????", path);

    memset(&g, 0, sizeof(g));
    n = glob(pattern, 0, NULL, &g) == 0 ? (int)g.gl_pathc : 0;
    globfree(&g);
    return n;
}

static void write_fd(int fd, const char *content)
{
    size_t len = strlen(content);

    fail_if(write(fd, content, len) != (ssize_t)len, "short write");
}

/* path replaced by content through n_atomic_*(); the commit's result */
static int put(const char *path, const char *content, unsigned flags)
{
    tn_atomic *af = n_atomic_open(path, flags);

    fail_if(af == NULL, "cannot open %s atomically: %m", path);
    write_fd(n_atomic_fd(af), content);
    return n_atomic_commit(af);
}

static void expect_file(const char *path, const char *content)
{
    char buf[256];
    ssize_t n;
    int fd;

    fd = open(path, O_RDONLY);
    fail_if(fd == -1, "cannot open %s", path);
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    fail_if(n < 0, "cannot read %s", path);
    buf[n] = '\0';
    expect_str(buf, content);
}

static void expect_link(const char *path)
{
    struct stat sb;

    fail_if(lstat(path, &sb) != 0, "cannot lstat %s", path);
    fail_unless(S_ISLNK(sb.st_mode), "%s is no longer a symlink", path);
}

/* the directories of the two paths are the same one */
static int same_dir(const char *a, const char *b)
{
    char da[PATH_MAX], db[PATH_MAX];
    struct stat sa, sb;

    snprintf(da, sizeof(da), "%.*s", (int)(strrchr(a, '/') - a), a);
    snprintf(db, sizeof(db), "%.*s", (int)(strrchr(b, '/') - b), b);
    fail_if(stat(da, &sa) != 0 || stat(db, &sb) != 0,
            "cannot stat %s or %s", da, db);

    return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

/* the temporary file is the hidden one next to the target, 0600 until the
   commit, and the target is replaced only by the commit */
START_TEST(test_atomic_commit)
{
    const char *path = NTEST_TMPPATH("natomic.txt");
    const char *base = strrchr(path, '/') + 1;
    char prefix[PATH_MAX];
    const char *tmpath, *tmpbase;
    struct stat sb;
    tn_atomic *af;

    unlink(path);
    expect_int(put(path, "old", 0), 0);
    expect_file(path, "old");

    af = n_atomic_open(path, 0);
    fail_if(af == NULL, "cannot open %s atomically", path);
    tmpath = n_atomic_tmpath(af);
    tmpbase = strrchr(tmpath, '/') + 1;

    expect_int(same_dir(tmpath, path), 1);
    snprintf(prefix, sizeof(prefix), ".%s.tmp.", base);
    fail_if(strncmp(tmpbase, prefix, strlen(prefix)) != 0,
            "%s is not %sXXXXXX", tmpbase, prefix);
    expect_int((int)strlen(tmpbase), (int)strlen(prefix) + 6);

    fail_if(stat(tmpath, &sb) != 0, "cannot stat %s", tmpath);
    expect_int((int)(sb.st_mode & 0777), 0600);
    fail_unless(fcntl(n_atomic_fd(af), F_GETFD) & FD_CLOEXEC,
                "the temporary file is inherited by an exec");

    write_fd(n_atomic_fd(af), "new");
    expect_file(path, "old");
    expect_int(n_atomic_commit(af), 0);
    expect_file(path, "new");
    expect_int(tmp_files(path), 0);
    unlink(path);
}
END_TEST

/* a replaced file keeps its mode, without setuid; a new one gets the umask
   of the open, not of the commit */
START_TEST(test_atomic_mode)
{
    const char *path = NTEST_TMPPATH("natomic-mode.txt");
    struct stat sb;
    tn_atomic *af;
    mode_t mask;

    unlink(path);
    mask = umask(027);
    af = n_atomic_open(path, 0);
    umask(022);
    fail_if(af == NULL, "cannot open %s atomically", path);
    write_fd(n_atomic_fd(af), "new");
    expect_int(n_atomic_commit(af), 0);
    umask(mask);

    fail_if(stat(path, &sb) != 0, "cannot stat %s", path);
    expect_int((int)(sb.st_mode & 0777), 0640);

    fail_if(chmod(path, 04751) != 0, "cannot chmod %s", path);
    expect_int(put(path, "newer", 0), 0);
    fail_if(stat(path, &sb) != 0, "cannot stat %s", path);
    expect_int((int)(sb.st_mode & 07777), 0751);
    unlink(path);
}
END_TEST

/* the owner and group of a replaced file are passed to fchown(); a refused
   fchown() (EPERM, another user's file) still commits, any other error
   does not */
START_TEST(test_atomic_owner)
{
    const char *path = NTEST_TMPPATH("natomic-owner.txt");
    struct stat sb;

    unlink(path);
    fchown_calls = 0;
    expect_int(put(path, "old", 0), 0);
    expect_int(fchown_calls, 0);        /* a new file is the writer's */

    fail_if(stat(path, &sb) != 0, "cannot stat %s", path);
    expect_int(put(path, "newer", 0), 0);
    expect_int(fchown_calls, 1);
    expect_int((int)fchown_uid, (int)sb.st_uid);
    expect_int((int)fchown_gid, (int)sb.st_gid);

    fchown_errno = EPERM;
    expect_int(put(path, "newest", 0), 0);
    expect_file(path, "newest");

    fchown_errno = EIO;
    errno = 0;
    expect_int(put(path, "lost", 0), -1);
    expect_int(errno, EIO);
    fchown_errno = 0;

    expect_file(path, "newest");
    expect_int(tmp_files(path), 0);
    unlink(path);
}
END_TEST

/* the real thing: a file chgrp'ed to another group of the writer keeps it */
START_TEST(test_atomic_group)
{
    const char *path = NTEST_TMPPATH("natomic-group.txt");
    gid_t groups[NGROUPS_MAX], gid = (gid_t)-1;
    struct stat sb;
    int i, n;

    n = getgroups(NGROUPS_MAX, groups);
    for (i = 0; i < n; i++) {
        if (groups[i] != getegid()) {
            gid = groups[i];
            break;
        }
    }

    if (gid == (gid_t)-1) {
        NTEST_LOG("skipped: no supplementary group\n");
        return;
    }

    unlink(path);
    expect_int(put(path, "old", 0), 0);
    fail_if(chown(path, (uid_t)-1, gid) != 0, "cannot chgrp %s", path);
    expect_int(put(path, "new", 0), 0);

    fail_if(stat(path, &sb) != 0, "cannot stat %s", path);
    expect_int((int)sb.st_gid, (int)gid);
    unlink(path);
}
END_TEST

/* a write through a symlink replaces its target, as open(O_TRUNC) would
   update it: absolute, a chain, relative to the link's directory, and a
   dangling one, whose target is created */
START_TEST(test_atomic_symlink)
{
    const char *real = NTEST_TMPPATH("natomic-real.txt");
    const char *link1 = NTEST_TMPPATH("natomic-link1.txt");
    const char *link2 = NTEST_TMPPATH("natomic-link2.txt");
    const char *dir = NTEST_TMPPATH("natomic-sub");
    char sublink[PATH_MAX], rel[PATH_MAX];
    tn_atomic *af;

    snprintf(sublink, sizeof(sublink), "%s/link.txt", dir);
    snprintf(rel, sizeof(rel), "../%s", strrchr(real, '/') + 1);

    unlink(real);
    unlink(link1);
    unlink(link2);
    unlink(sublink);
    rmdir(dir);
    expect_int(put(real, "old", 0), 0);

    /* absolute */
    fail_if(symlink(real, link1) != 0, "cannot symlink %s", link1);
    expect_int(put(link1, "abs", 0), 0);
    expect_link(link1);
    expect_file(real, "abs");
    expect_int(tmp_files(real), 0);
    expect_int(tmp_files(link1), 0);

    /* a chain: link2 -> link1 -> real */
    fail_if(symlink(link1, link2) != 0, "cannot symlink %s", link2);
    expect_int(put(link2, "chain", 0), 0);
    expect_link(link2);
    expect_link(link1);
    expect_file(real, "chain");

    /* relative to the link's directory, not to the cwd; the temporary file
       is next to the target, so the rename() does not cross a filesystem */
    fail_if(mkdir(dir, 0755) != 0, "cannot mkdir %s", dir);
    fail_if(symlink(rel, sublink) != 0, "cannot symlink %s", sublink);
    af = n_atomic_open(sublink, 0);
    fail_if(af == NULL, "cannot open %s atomically", sublink);
    expect_int(same_dir(n_atomic_tmpath(af), real), 1);
    write_fd(n_atomic_fd(af), "rel");
    expect_int(n_atomic_commit(af), 0);
    expect_link(sublink);
    expect_file(real, "rel");

    /* dangling */
    unlink(real);
    expect_int(put(link1, "dangling", 0), 0);
    expect_link(link1);
    expect_file(real, "dangling");

    unlink(sublink);
    rmdir(dir);
    unlink(link2);
    unlink(link1);
    unlink(real);
}
END_TEST

START_TEST(test_atomic_open_errors)
{
    const char *a = NTEST_TMPPATH("natomic-loop-a.txt");
    const char *b = NTEST_TMPPATH("natomic-loop-b.txt");
    const char *plain = NTEST_TMPPATH("natomic-errors.txt");
    char longname[PATH_MAX];
    const char *dir = strrchr(plain, '/');

    /* a symlink loop */
    unlink(a);
    unlink(b);
    fail_if(symlink(b, a) != 0 || symlink(a, b) != 0, "cannot symlink");
    errno = 0;
    fail_if(n_atomic_open(a, 0) != NULL, "a symlink loop was opened");
    expect_int(errno, ELOOP);
    expect_int(tmp_files(a), 0);
    expect_int(tmp_files(b), 0);
    unlink(a);
    unlink(b);

    /* a flag that does not exist yet is not silently ignored */
    errno = 0;
    fail_if(n_atomic_open(plain, TN_ATOMIC_SYNC << 1) != NULL,
            "an unknown flag was accepted");
    expect_int(errno, EINVAL);
    expect_int(tmp_files(plain), 0);

    /* a name that fits, but not with .<name>.tmp.XXXXXX around it */
    snprintf(longname, sizeof(longname), "%.*s/%0*d", (int)(dir - plain),
             plain, NAME_MAX - 4, 0);
    errno = 0;
    fail_if(n_atomic_open(longname, 0) != NULL, "%s was opened", longname);
    expect_int(errno, ENAMETOOLONG);

    errno = 0;
    fail_if(n_atomic_open(NTEST_TMPPATH("natomic-no-such-dir/x"), 0) != NULL,
            "opened in a missing directory");
    expect_int(errno, ENOENT);
}
END_TEST

/* path is refused by the open with errno, and nothing is created, next to
   it or in it */
static void expect_refused(const char *path, int err)
{
    char pattern[PATH_MAX + 16];
    glob_t g;
    int n;

    errno = 0;
    fail_if(n_atomic_open(path, 0) != NULL, "\"%s\" was opened", path);
    expect_int(errno, err);
    expect_int(tmp_files(path), 0);    /* for "", ..tmp.* in the cwd */

    snprintf(pattern, sizeof(pattern), "%s/..tmp.??????", path);
    memset(&g, 0, sizeof(g));
    n = glob(pattern, 0, NULL, &g) == 0 ? (int)g.gl_pathc : 0;
    globfree(&g);
    expect_int(n, 0);
}

/* only a regular file, or a new one, can be replaced: a rename() would
   replace the node of a directory or a device instead of writing into it */
START_TEST(test_atomic_target_type)
{
    const char *dir = NTEST_TMPPATH("natomic-dir");
    const char *file = NTEST_TMPPATH("natomic-file.txt");
    const char *dlink = NTEST_TMPPATH("natomic-dirlink");
    const char *fifo = NTEST_TMPPATH("natomic-fifo");
    char slashed[PATH_MAX];
    struct stat sb;

    rmdir(dir);
    unlink(file);
    unlink(dlink);
    unlink(fifo);
    fail_if(mkdir(dir, 0700) != 0, "cannot mkdir %s", dir);
    expect_int(put(file, "x", 0), 0);
    fail_if(symlink(dir, dlink) != 0, "cannot symlink %s", dlink);
    fail_if(mkfifo(fifo, 0600) != 0, "cannot mkfifo %s", fifo);

    expect_refused("", ENOENT);
    expect_refused(dir, EISDIR);
    snprintf(slashed, sizeof(slashed), "%s/", dir);
    expect_refused(slashed, EISDIR);
    expect_refused(dlink, EISDIR);
    snprintf(slashed, sizeof(slashed), "%s/", file);
    expect_refused(slashed, ENOTDIR);
    expect_refused(fifo, EINVAL);
    expect_refused("/dev/null", EINVAL);

    fail_if(stat(dir, &sb) != 0 || !S_ISDIR(sb.st_mode), "%s replaced", dir);
    fail_if(lstat(fifo, &sb) != 0 || !S_ISFIFO(sb.st_mode), "%s replaced",
            fifo);
    expect_file(file, "x");

    rmdir(dir);
    unlink(file);
    unlink(dlink);
    unlink(fifo);
}
END_TEST

/* a failed rename() or close() keeps the old file and leaves no temporary
   one */
START_TEST(test_atomic_commit_failure)
{
    const char *path = NTEST_TMPPATH("natomic-failure.txt");
    tn_atomic *af;

    unlink(path);
    expect_int(put(path, "old", 0), 0);

    rename_fails = 1;
    errno = 0;
    expect_int(put(path, "new", 0), -1);
    expect_int(errno, EIO);
    rename_fails = 0;
    expect_file(path, "old");
    expect_int(tmp_files(path), 0);

    af = n_atomic_open(path, 0);
    fail_if(af == NULL, "cannot open %s atomically", path);
    write_fd(n_atomic_fd(af), "new");
    close_fail_fd = n_atomic_fd(af);
    errno = 0;
    expect_int(n_atomic_commit(af), -1);
    expect_int(errno, EIO);
    expect_file(path, "old");
    expect_int(tmp_files(path), 0);
    unlink(path);
}
END_TEST

/* TN_ATOMIC_SYNC syncs the file before the rename and its directory after
   it, and only with the flag */
START_TEST(test_atomic_sync)
{
    const char *path = NTEST_TMPPATH("natomic-sync.txt");

    unlink(path);
    file_syncs = dir_syncs = 0;
    expect_int(put(path, "old", 0), 0);
    expect_int(file_syncs, 0);
    expect_int(dir_syncs, 0);

    renamed = 0;
    dir_cloexec = 0;
    expect_int(put(path, "new", TN_ATOMIC_SYNC), 0);
    expect_int(file_syncs, 1);
    expect_int(file_synced_renamed, 0);
    expect_int(dir_syncs, 1);
    expect_int(dir_cloexec, 1);
    expect_file(path, "new");

    /* the file's: no rename */
    fsync_fail = 1;
    errno = 0;
    expect_int(put(path, "lost", TN_ATOMIC_SYNC), -1);
    expect_int(errno, EIO);
    expect_file(path, "new");
    expect_int(tmp_files(path), 0);

    /* the directory's: replaced, and reported */
    fsync_fail = 2;
    errno = 0;
    expect_int(put(path, "newer", TN_ATOMIC_SYNC), -1);
    expect_int(errno, EIO);
    expect_file(path, "newer");
    expect_int(tmp_files(path), 0);

    /* a directory that cannot be synced is not a failure */
    fsync_fail = 3;
    expect_int(put(path, "newest", TN_ATOMIC_SYNC), 0);
    expect_file(path, "newest");
    fsync_fail = 0;
    unlink(path);
}
END_TEST

/* the target is left alone; a temporary file already gone is not an
   error, one that cannot be removed is */
START_TEST(test_atomic_abort)
{
    const char *path = NTEST_TMPPATH("natomic-abort.txt");
    char tmpath[PATH_MAX];
    tn_atomic *af;

    unlink(path);
    expect_int(put(path, "old", 0), 0);

    af = n_atomic_open(path, 0);
    fail_if(af == NULL, "cannot open %s atomically", path);
    write_fd(n_atomic_fd(af), "new");
    expect_int(n_atomic_abort(af), 0);
    expect_file(path, "old");
    expect_int(tmp_files(path), 0);

    af = n_atomic_open(path, 0);
    fail_if(af == NULL, "cannot open %s atomically", path);
    unlink(n_atomic_tmpath(af));
    expect_int(n_atomic_abort(af), 0);

    af = n_atomic_open(path, 0);
    fail_if(af == NULL, "cannot open %s atomically", path);
    snprintf(tmpath, sizeof(tmpath), "%s", n_atomic_tmpath(af));
    unlink(tmpath);
    fail_if(mkdir(tmpath, 0700) != 0, "cannot mkdir %s", tmpath);
    errno = 0;
    expect_int(n_atomic_abort(af), -1);
    fail_if(errno != EISDIR && errno != EPERM,
            "unexpected errno %d of a failed unlink", errno);
    rmdir(tmpath);

    expect_file(path, "old");
    unlink(path);
}
END_TEST

NTEST_RUNNER("natomic",
             test_atomic_commit,
             test_atomic_mode,
             test_atomic_owner,
             test_atomic_group,
             test_atomic_symlink,
             test_atomic_open_errors,
             test_atomic_target_type,
             test_atomic_commit_failure,
             test_atomic_sync,
             test_atomic_abort);
