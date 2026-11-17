#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <sys/stat.h>
#include <unistd.h>

#include "n_check.h"
#include "ndie.h"
#include "nmalloc.h"
#include "nstream.h"
#include "nstore.h"

void do_test_read_write(const char *tmppath)
{
    char buf[75];
    int i, nitems = 10;
    char *fmt = "line%.12d";
    int nw = 0, nr = 0;

    tn_stream *io = n_stream_open(tmppath, "w", TN_STREAM_UNKNOWN);
    for (i = 0; i < nitems; i++) {
        int n = snprintf(buf, sizeof(buf), fmt, i);
        nw += n_stream_write(io, buf, n);
    }
    expect_int(n_stream_close(io), 0);

    io = n_stream_open(tmppath, "r", TN_STREAM_UNKNOWN);
    for (i = 0; i < nitems; i++) {
        int n = snprintf(buf, sizeof(buf), fmt, i);
        char rbuf[75];

        memset(rbuf, 0, sizeof(rbuf));

        int x = n_stream_read(io, rbuf, n);
        expect_int(x, n);
        expect_int(memcmp(buf, rbuf, n), 0);
        nr += x;
        //printf("read %s %d = %d  => %s\n", buf, n, strlen(buf), rbuf);

    }
    n_stream_close(io);
    expect_int(nw, nr);
    //printf("OK\n");
}

START_TEST(test_read_write)
{
    do_test_read_write(NTEST_TMPPATH("nstream-rw.txt"));
    do_test_read_write(NTEST_TMPPATH("nstream-rw.gz"));
    do_test_read_write(NTEST_TMPPATH("nstream-rw.zst"));
}
END_TEST

static void seek_and_read(tn_stream *io, int nth)
{
    char buf[75], rbuf[75];
    char *fmt = "line%.12d";
    int n, x;


    n_stream_seek(io, nth * 16, SEEK_SET);
    expect_int(n_stream_tell(io), nth * 16);

    n = snprintf(buf, sizeof(buf), fmt, nth);
    x = n_stream_read(io, rbuf, n);

    expect_int(x, n);
    expect_int(memcmp(buf, rbuf, n), 0);
}

void do_test_seek(const char *tmppath)
{
    char buf[75];
    char *fmt = "line%.12d";
    tn_stream *io;
    //char rbuf[75];
    int i;

    io = n_stream_open(tmppath, "w", TN_STREAM_UNKNOWN);
    for (i = 0; i < 100; i++) {
        int n = snprintf(buf, sizeof(buf), fmt, i);
        int nn = n_stream_write(io, buf, n);
        expect_int(nn, n);
    }
    n_stream_close(io);

    io = n_stream_open(tmppath, "r", TN_STREAM_UNKNOWN);
    expect_int(n_stream_tell(io), 0);

    seek_and_read(io, 1);
    expect_int(n_stream_tell(io), 32);

    seek_and_read(io, 0);
    expect_int(n_stream_tell(io), 16);

    seek_and_read(io, 1);
    expect_int(n_stream_tell(io), 32);

    seek_and_read(io, 2);
    expect_int(n_stream_tell(io), 48);
}

START_TEST(test_seek) {
    do_test_seek(NTEST_TMPPATH("nstream-seek.txt"));
    do_test_seek(NTEST_TMPPATH("nstream-seek.gz"));
    do_test_seek(NTEST_TMPPATH("nstream-seek.zst"));
}
END_TEST

/* for a name that does not say how the file is compressed, the type given
   decides; the data is written that way and read back */
static void do_test_open_type(int type, const unsigned char *magic)
{
    const char *path = NTEST_TMPPATH("nstream-type.dat");
    unsigned char head[2];
    char buf[16];
    tn_stream *st;
    FILE *f;

    st = n_stream_open(path, "w", type);
    fail_if(st == NULL, "cannot create %s", path);
    expect_int(n_stream_write(st, "0123456789", 10), 10);
    n_stream_close(st);

    f = fopen(path, "r");
    fail_if(f == NULL, "cannot open %s", path);
    expect_int(fread(head, 1, sizeof(head), f), sizeof(head));
    fclose(f);
    fail_if(memcmp(head, magic, sizeof(head)) != 0, "type %d: written in another format", type);

    st = n_stream_open(path, "r", type);
    fail_if(st == NULL, "cannot open %s", path);
    expect_int(n_stream_read(st, buf, sizeof(buf)), 10);
    expect_int(memcmp(buf, "0123456789", 10), 0);
    n_stream_close(st);
}

START_TEST(test_open_type)
{
    do_test_open_type(TN_STREAM_GZIO, (const unsigned char *) "\x1f\x8b");
    do_test_open_type(TN_STREAM_ZSTDIO, (const unsigned char *) "\x28\xb5");
}
END_TEST


/* n_stream_getline() returns one line at a time and grows its buffer when
   the line does not fit; a line that ends exactly at the buffer boundary
   must not pull in the next one. Run over a plain, a gzip and a zstd
   stream, since st_gets and st_getc are separate wrappers there. */
static void do_test_getline(const char *path, size_t startsize)
{
    static const char *lines[] = {
        "short\n",
        "12345678901234\n",     /* 15 bytes, exactly the buffer size - 1 */
        "a longer line that does not fit and grows the buffer: "
        "0123456789 0123456789\n",
        "last line without a newline",
        NULL
    };
    tn_stream *st;
    char *buf;
    size_t size;
    int i;

    st = n_stream_open(path, "w", TN_STREAM_UNKNOWN);
    fail_if(st == NULL, "cannot create %s", path);
    for (i = 0; lines[i] != NULL; i++)
        n_stream_write(st, lines[i], strlen(lines[i]));
    n_stream_close(st);

    st = n_stream_open(path, "r", TN_STREAM_UNKNOWN);
    fail_if(st == NULL, "cannot open %s", path);

    size = startsize;
    buf = startsize ? n_malloc(startsize) : NULL;
    for (i = 0; lines[i] != NULL; i++) {
        int n = n_stream_getline(st, &buf, &size);

        expect_int(n, strlen(lines[i]));
        expect_str(buf, lines[i]);
    }
    expect_int(n_stream_getline(st, &buf, &size), 0);     /* EOF */

    /* startsize 0 means *bufptr = NULL: the buffer is allocated as
       MAX_CANON right away, so only an explicit buffer can grow here */
    if (startsize > 0)
        fail_unless(size > startsize,
                    "the grown buffer size was not reported: %zu", size);

    n_free(buf);
    n_stream_close(st);
}

START_TEST(test_stream_getline)
{
    do_test_getline(NTEST_TMPPATH("nstream-getline.txt"), 16);
    do_test_getline(NTEST_TMPPATH("nstream-getline.gz"), 16);
    do_test_getline(NTEST_TMPPATH("nstream-getline.zst"), 16);
    do_test_getline(NTEST_TMPPATH("nstream-getline-alloc.txt"), 0);
}
END_TEST

static void do_test_printf_long(const char *path)
{
    /* with ".", one byte more than n_stream_vprintf()'s stack buffer holds */
    char s[32 * 1024], rbuf[sizeof(s) + 16];
    tn_stream *st;
    int i;

    memset(s, 'x', sizeof(s) - 1);
    s[sizeof(s) - 1] = '\0';

    st = n_stream_open(path, "w", TN_STREAM_UNKNOWN);
    fail_if(st == NULL, "cannot create %s", path);
    expect_int(n_stream_printf(st, "%s.", s), sizeof(s));
    n_stream_close(st);

    st = n_stream_open(path, "r", TN_STREAM_UNKNOWN);
    expect_int(n_stream_read(st, rbuf, sizeof(rbuf)), sizeof(s));
    n_stream_close(st);
    for (i = 0; i < (int) sizeof(s) - 1 && rbuf[i] == 'x'; i++)
        ;
    expect_int(i, sizeof(s) - 1);
    expect_int(rbuf[sizeof(s) - 1], '.');
}

START_TEST(test_printf_long)
{
    do_test_printf_long(NTEST_TMPPATH("nstream-printf-long.txt"));
    do_test_printf_long(NTEST_TMPPATH("nstream-printf-long.gz"));
    do_test_printf_long(NTEST_TMPPATH("nstream-printf-long.zst"));
}
END_TEST

static void do_test_printf_format_error(const char *path)
{
    char rbuf[16];
    tn_stream *st;

    st = n_stream_open(path, "w", TN_STREAM_UNKNOWN);
    fail_if(st == NULL, "cannot create %s", path);
    /* the C locale has no multibyte form of U+0100 */
    expect_int(n_stream_printf(st, "%ls", L"\x100"), -1);
    n_stream_close(st);

    st = n_stream_open(path, "r", TN_STREAM_UNKNOWN);
    expect_int(n_stream_read(st, rbuf, sizeof(rbuf)), 0);
    n_stream_close(st);
}

/* a close that cannot write what is left in the buffers fails */
static void do_test_close_error(int type)
{
    int fd = open("/dev/full", O_WRONLY);
    tn_stream *st;

    fail_if(fd == -1, "cannot open /dev/full");
    st = n_stream_dopen(fd, "w", type);
    fail_if(st == NULL, "cannot open /dev/full as stream type %d", type);
    expect_int(n_stream_write(st, "0123456789", 10), 10);
    expect_int(n_stream_close(st), -1);
}

START_TEST(test_close_error)
{
    do_test_close_error(TN_STREAM_STDIO);
    do_test_close_error(TN_STREAM_GZIO);
    do_test_close_error(TN_STREAM_ZSTDIO);
}
END_TEST

/* a write that fails during n_stream_printf(): the data neither compresses
   nor fits in the streams' buffers, so the failure happens before the close */
static void do_test_printf_write_error(int type)
{
    const size_t size = 4 * 1024 * 1024;
    char *buf = n_malloc(size + 1);
    unsigned x = 1;
    tn_stream *st;
    size_t i;
    int fd = open("/dev/full", O_WRONLY);

    fail_if(fd == -1, "cannot open /dev/full");
    fail_if(buf == NULL, "cannot allocate %zu bytes", size + 1);
    for (i = 0; i < size; i++) {
        x = x * 1664525u + 1013904223u;
        buf[i] = (char)((x >> 24) & 0xff);
        if (buf[i] == '\0')
            buf[i] = 'x';
    }
    buf[size] = '\0';

    st = n_stream_dopen(fd, "w", type);
    fail_if(st == NULL, "cannot open /dev/full as stream type %d", type);
    errno = 0;
    expect_int(n_stream_printf(st, "%s", buf), -1);
    expect_int(errno, ENOSPC);
    n_stream_close(st);
    n_free(buf);
}

START_TEST(test_printf_write_error)
{
    do_test_printf_write_error(TN_STREAM_STDIO);
    do_test_printf_write_error(TN_STREAM_GZIO);
    do_test_printf_write_error(TN_STREAM_ZSTDIO);
}
END_TEST

/* a backend that fails without an errno still leaves one for %m */
static int write_nothing(void *stream, const void *buf, size_t size)
{
    (void) stream;
    (void) buf;
    (void) size;
    return 0;
}

START_TEST(test_write_error_sets_errno)
{
    tn_stream st;

    memset(&st, 0, sizeof(st));
    st.st_write = write_nothing;

    errno = 0;
    expect_int(n_stream_write(&st, "abc", 3), 0);
    expect_int(errno, EIO);
    expect_int(st.werr, EIO);

    errno = 0;
    expect_int(n_stream_printf(&st, "%s", "abc"), -1);
    expect_int(errno, EIO);
    expect_int(st.werr, EIO);
}
END_TEST

/* reads the whole file back */
static void expect_content_type(const char *path, const char *content, int type)
{
    char buf[256];
    tn_stream *st;
    int n;

    st = n_stream_open(path, "r", type);
    fail_if(st == NULL, "cannot open %s", path);
    n = n_stream_read(st, buf, sizeof(buf) - 1);
    fail_if(n < 0, "%s: read failed", path);
    buf[n] = '\0';
    expect_str(buf, content);
    n_stream_close(st);
}

static void expect_content(const char *path, const char *content)
{
    expect_content_type(path, content, TN_STREAM_UNKNOWN);
}

static void write_content(const char *path, const char *content)
{
    tn_stream *st = n_stream_open(path, "w", TN_STREAM_UNKNOWN);
    int len = strlen(content);

    fail_if(st == NULL, "cannot create %s", path);
    expect_int(n_stream_write(st, content, len), len);
    expect_int(n_stream_close(st), 0);
}

static int die_calls;
static char die_msg[256];

static void test_die_hook(const char *errmsg)
{
    die_calls++;
    snprintf(die_msg, sizeof(die_msg), "%s", errmsg);
}

/* the path.tmpXXXXXX files a close or an abort must not leave behind */
static int tmp_files(const char *path)
{
    char pattern[4096];
    glob_t g;
    int n;

    snprintf(pattern, sizeof(pattern), "%s.tmp??????", path);
    memset(&g, 0, sizeof(g));
    n = glob(pattern, 0, NULL, &g) == 0 ? (int)g.gl_pathc : 0;
    globfree(&g);
    return n;
}

START_TEST(test_atomic)
{
    const char *path = NTEST_TMPPATH("nstream-atomic.txt");
    const char *rep = NTEST_TMPPATH("nstream-atomic-replace.txt");
    const char *tmpp = NTEST_TMPPATH("nstream-atomic.tmp.txt");
    struct stat sb;
    tn_stream *st;

    unlink(path);
    st = n_stream_open(path, "w", TN_STREAM_UNKNOWN | TN_STREAM_ATOMIC);
    fail_if(st == NULL, "cannot open %s atomically", path);
    expect_int(n_stream_write(st, "hello", 5), 5);
    expect_int(n_stream_close(st), 0);
    expect_content(path, "hello");
    expect_int(tmp_files(path), 0);

    /* TN_STREAM_UNKNOWN was -1: a binary built against the old header
       still passes it */
    unlink(path);
    st = n_stream_open(path, "w", -1);
    fail_if(st == NULL, "cannot open %s with the old TN_STREAM_UNKNOWN", path);
    expect_int(n_stream_write(st, "old", 3), 3);
    expect_int(n_stream_close(st), 0);
    expect_content(path, "old");

    write_content(rep, "old");
    st = n_stream_open(rep, "w", TN_STREAM_ATOMIC);
    fail_if(st == NULL, "cannot open %s atomically", rep);
    expect_int(n_stream_write(st, "new", 3), 3);
    expect_content(rep, "old");         /* renamed only by the close */
    expect_int(n_stream_close(st), 0);
    expect_content(rep, "new");
    expect_int(tmp_files(rep), 0);

    /* nothing written is still a file to publish: an empty stub index */
    unlink(path);
    st = n_stream_open(path, "w", TN_STREAM_ATOMIC);
    fail_if(st == NULL, "cannot open %s atomically", path);
    expect_int(n_stream_close(st), 0);
    fail_if(stat(path, &sb) != 0, "cannot stat %s", path);
    expect_int((int)sb.st_size, 0);

    /* the consumers write through n_stream_printf() */
    unlink(path);
    st = n_stream_open(path, "w", TN_STREAM_ATOMIC);
    fail_if(st == NULL, "cannot open %s atomically", path);
    expect_int(n_stream_printf(st, "line %d\n", 1), 7);
    expect_int(n_stream_printf(st, "line %d\n", 2), 7);
    expect_int(n_stream_close(st), 0);
    expect_content(path, "line 1\nline 2\n");

    /* only the fixed suffix is cut: a ".tmp" in the target name stays */
    unlink(tmpp);
    st = n_stream_open(tmpp, "w", TN_STREAM_ATOMIC);
    fail_if(st == NULL, "cannot open %s atomically", tmpp);
    expect_int(n_stream_write(st, "kept", 4), 4);
    expect_int(n_stream_close(st), 0);
    expect_content(tmpp, "kept");
    expect_int(tmp_files(tmpp), 0);
}
END_TEST

START_TEST(test_atomic_abort)
{
    const char *path = NTEST_TMPPATH("nstream-atomic-abort.txt");
    const char *errpath = NTEST_TMPPATH("nstream-atomic-werr.txt");
    const char *bad = NTEST_TMPPATH("nstream-atomic-abort-bad.txt");
    char bad_tmp[PATH_MAX];
    t_fn_die_hook old_hook;
    tn_stream *st;
    int rc;

    write_content(path, "old");
    st = n_stream_open(path, "w", TN_STREAM_ATOMIC);
    fail_if(st == NULL, "cannot open %s atomically", path);
    expect_int(n_stream_write(st, "new", 3), 3);
    expect_int(n_stream_abort(st), 0);
    expect_content(path, "old");
    expect_int(tmp_files(path), 0);

    /* as if the write had failed after the bytes were taken: the close must
       drop the temporary file, not rename a file known to be cut */
    unlink(errpath);
    st = n_stream_open(errpath, "w", TN_STREAM_ATOMIC);
    fail_if(st == NULL, "cannot open %s atomically", errpath);
    expect_int(n_stream_write(st, "x", 1), 1);
    st->werr = ENOSPC;
    errno = 0;
    expect_int(n_stream_close(st), -1);
    expect_int(errno, ENOSPC);
    fail_if(access(errpath, F_OK) == 0, "%s: a failed write was renamed",
            errpath);
    expect_int(tmp_files(errpath), 0);

    /* a temporary file that cannot be unlinked is an error of its own */
    unlink(bad);
    st = n_stream_open(bad, "w", TN_STREAM_ATOMIC);
    fail_if(st == NULL, "cannot open %s atomically", bad);
    snprintf(bad_tmp, sizeof(bad_tmp), "%s", st->path);
    expect_int(n_stream_write(st, "x", 1), 1);
    unlink(bad_tmp);                /* the open descriptor is unaffected */
    fail_if(mkdir(bad_tmp, 0700) != 0, "cannot mkdir %s", bad_tmp);

    errno = 0;
    expect_int(n_stream_abort(st), -1);
    fail_if(errno != EISDIR && errno != EPERM,
            "unexpected errno %d of a failed unlink", errno);
    rmdir(bad_tmp);

    fail_if(access(bad, F_OK) == 0, "%s: abort published the target", bad);

    /* a stream that is not atomic has nothing to abort: n_die() reports the
       misuse, and the stream is still closed, so what was written is there */
    unlink(path);
    st = n_stream_open(path, "w", TN_STREAM_UNKNOWN);
    fail_if(st == NULL, "cannot open %s", path);
    expect_int(n_stream_write(st, "abc", 3), 3);

    old_hook = n_die_set_hook(test_die_hook);
    die_calls = 0;
    rc = n_stream_abort(st);
    n_die_set_hook(old_hook);

    expect_int(die_calls, 1);
    expect_str(die_msg, "non-atomic stream cannot be aborted\n");
    expect_int(rc, 0);
    expect_content(path, "abc");
}
END_TEST

static void do_test_atomic_type(const char *path, int type, const char *mode,
                                int read_type)
{
    tn_stream *st;
    size_t len = strlen("0123456789");

    unlink(path);
    st = n_stream_open(path, mode, type);
    fail_if(st == NULL, "cannot open %s atomically", path);
    expect_int(n_stream_write(st, "0123456789", len), len);
    expect_int(n_stream_close(st), 0);
    expect_content_type(path, "0123456789", read_type);
    expect_int(tmp_files(path), 0);
}

START_TEST(test_atomic_mode_and_type)
{
    const char *path = NTEST_TMPPATH("nstream-atomic-mode.txt");
    const char *suid = NTEST_TMPPATH("nstream-atomic-mode-suid.txt");
    const char *newp = NTEST_TMPPATH("nstream-atomic-mode-new.txt");
    const char *sec = NTEST_TMPPATH("nstream-atomic-mode-umask077.txt");
    const char *real = NTEST_TMPPATH("nstream-atomic-symlink-real.txt");
    const char *link = NTEST_TMPPATH("nstream-atomic-symlink.txt");
    struct stat sb;
    mode_t mask;
    tn_stream *st;

    /* an existing file keeps its mode, and the temporary file is 0600 until
       the close, so a partial write is never readable by others */
    write_content(path, "x");
    fail_if(chmod(path, 0640) != 0, "cannot chmod %s", path);
    st = n_stream_open(path, "w", TN_STREAM_ATOMIC);
    fail_if(st == NULL, "cannot open %s atomically", path);
    fail_if(stat(st->path, &sb) != 0, "cannot stat the temporary file");
    expect_int((int)(sb.st_mode & 0777), 0600);
    expect_int(n_stream_write(st, "y", 1), 1);
    expect_int(n_stream_close(st), 0);
    fail_if(stat(path, &sb) != 0, "cannot stat %s", path);
    expect_int((int)(sb.st_mode & 0777), 0640);

    /* setuid and setgid are not carried over to the new file */
    write_content(suid, "x");
    fail_if(chmod(suid, 04755) != 0, "cannot chmod %s", suid);
    st = n_stream_open(suid, "w", TN_STREAM_ATOMIC);
    fail_if(st == NULL, "cannot open %s atomically", suid);
    expect_int(n_stream_write(st, "y", 1), 1);
    expect_int(n_stream_close(st), 0);
    fail_if(stat(suid, &sb) != 0, "cannot stat %s", suid);
    expect_int((int)(sb.st_mode & 07777), 0755);

    /* a new file gets 0666 & ~umask */
    unlink(newp);
    mask = umask(022);
    st = n_stream_open(newp, "w", TN_STREAM_ATOMIC);
    umask(mask);
    fail_if(st == NULL, "cannot open %s atomically", newp);
    expect_int(n_stream_write(st, "z", 1), 1);
    expect_int(n_stream_close(st), 0);
    fail_if(stat(newp, &sb) != 0, "cannot stat %s", newp);
    expect_int((int)(sb.st_mode & 0777), 0644);

    /* a tighter umask is honoured too */
    unlink(sec);
    mask = umask(077);
    st = n_stream_open(sec, "w", TN_STREAM_ATOMIC);
    umask(mask);
    fail_if(st == NULL, "cannot open %s atomically", sec);
    expect_int(n_stream_write(st, "z", 1), 1);
    expect_int(n_stream_close(st), 0);
    fail_if(stat(sec, &sb) != 0, "cannot stat %s", sec);
    expect_int((int)(sb.st_mode & 0777), 0600);

    /* a symlink is replaced by the file it pointed at, mode included; the
       file itself it pointed at is left alone */
    write_content(real, "old");
    fail_if(chmod(real, 0640) != 0, "cannot chmod %s", real);
    unlink(link);
    fail_if(symlink(real, link) != 0, "cannot symlink %s", link);
    st = n_stream_open(link, "w", TN_STREAM_ATOMIC);
    fail_if(st == NULL, "cannot open %s atomically", link);
    expect_int(n_stream_write(st, "new", 3), 3);
    expect_int(n_stream_close(st), 0);
    fail_if(lstat(link, &sb) != 0, "cannot lstat %s", link);
    fail_if(S_ISLNK(sb.st_mode), "%s is still a symlink", link);
    expect_int((int)(sb.st_mode & 0777), 0640);
    expect_content(link, "new");
    expect_content(real, "old");
    unlink(link);

    expect_int(tmp_files(path), 0);
    expect_int(tmp_files(suid), 0);
    expect_int(tmp_files(newp), 0);
    expect_int(tmp_files(sec), 0);

    /* the type is guessed from the target name, not from path.tmpXXXXXX */
    do_test_atomic_type(NTEST_TMPPATH("nstream-atomic.gz"), TN_STREAM_ATOMIC,
                        "w", TN_STREAM_UNKNOWN);
    do_test_atomic_type(NTEST_TMPPATH("nstream-atomic.zst"), TN_STREAM_ATOMIC,
                        "w", TN_STREAM_UNKNOWN);

    /* the mode still carries the gz compression level into gzdopen() */
    do_test_atomic_type(NTEST_TMPPATH("nstream-atomic-level.gz"),
                        TN_STREAM_ATOMIC, "wb9", TN_STREAM_UNKNOWN);

    /* an explicit type wins over a name without a compression suffix */
    do_test_atomic_type(NTEST_TMPPATH("nstream-atomic-explicit"),
                        TN_STREAM_GZIO | TN_STREAM_ATOMIC, "w",
                        TN_STREAM_GZIO);
}
END_TEST

START_TEST(test_atomic_errors)
{
    const char *dir = NTEST_TMPPATH("nstream-atomic-dir");
    const char *missing = NTEST_TMPPATH("nstream-no-such-dir/atomic.txt");
    const char *plain = NTEST_TMPPATH("nstream-atomic-errors.txt");
    struct stat sb;
    t_fn_die_hook old_hook;
    tn_stream *st;
    int fd;

    rmdir(dir);
    fail_if(mkdir(dir, 0700) != 0, "cannot mkdir %s", dir);
    st = n_stream_open(dir, "w", TN_STREAM_ATOMIC);
    fail_if(st == NULL, "cannot open %s atomically", dir);
    expect_int(n_stream_write(st, "x", 1), 1);

    expect_int(n_stream_close(st), -1);
    fail_if(stat(dir, &sb) != 0 || !S_ISDIR(sb.st_mode),
            "%s: the directory was replaced", dir);
    expect_int(tmp_files(dir), 0);
    rmdir(dir);

    errno = 0;
    st = n_stream_open(missing, "w", TN_STREAM_ATOMIC);
    fail_if(st != NULL, "atomic open succeeded in a missing directory");
    expect_int(errno, ENOENT);

    /* a mode that would rename an empty file over the target */
    errno = 0;
    st = n_stream_open(plain, "r", TN_STREAM_ATOMIC);
    fail_if(st != NULL, "a read stream was opened atomically");
    expect_int(errno, EINVAL);
    expect_int(tmp_files(plain), 0);

    errno = 0;
    st = n_stream_open(plain, "a", TN_STREAM_ATOMIC);
    fail_if(st != NULL, "an append stream was opened atomically");
    expect_int(errno, EINVAL);
    expect_int(tmp_files(plain), 0);

    /* a descriptor has no name to be renamed to */
    fd = open(plain, O_WRONLY | O_CREAT, 0600);
    fail_if(fd < 0, "cannot create %s", plain);
    errno = 0;
    st = n_stream_dopen(fd, "w", TN_STREAM_STDIO | TN_STREAM_ATOMIC);
    fail_if(st != NULL, "a flag was accepted by n_stream_dopen()");
    expect_int(errno, EINVAL);
    close(fd);
    unlink(plain);

    /* a descriptor the backend cannot open: the stream is freed, not leaked */
    errno = 0;
    st = n_stream_dopen(-1, "w", TN_STREAM_STDIO);
    fail_if(st != NULL, "n_stream_dopen() accepted a closed descriptor");
    expect_int(errno, EBADF);

    /* an unknown type is reported by n_die(); its hook lets the caller keep
       running, and then there is no stream to give back */
    old_hook = n_die_set_hook(test_die_hook);

    die_calls = 0;
    st = n_stream_open(plain, "w", 7);
    n_die_set_hook(old_hook);
    fail_if(st != NULL, "n_stream_open() accepted an unknown type");
    expect_int(die_calls, 1);
    expect_str(die_msg, "7: unknown stream type\n");

    old_hook = n_die_set_hook(test_die_hook);

    die_calls = 0;
    st = n_stream_open(plain, "w", 7 | TN_STREAM_ATOMIC);
    n_die_set_hook(old_hook);
    fail_if(st != NULL, "an unknown type was opened atomically");
    expect_int(die_calls, 1);
    expect_str(die_msg, "7: unknown stream type\n");
    expect_int(tmp_files(plain), 0);   /* and no temporary file is left */

    old_hook = n_die_set_hook(test_die_hook);

    die_calls = 0;
    st = n_stream_dopen(-1, "w", TN_STREAM_UNKNOWN);
    n_die_set_hook(old_hook);
    fail_if(st != NULL, "n_stream_dopen() accepted an unknown type");
    expect_int(die_calls, 1);
    expect_str(die_msg, "0: unknown stream type\n");

    /* the old spelling of TN_STREAM_UNKNOWN reaches the same place */
    old_hook = n_die_set_hook(test_die_hook);

    die_calls = 0;
    st = n_stream_dopen(-1, "w", -1);
    n_die_set_hook(old_hook);
    fail_if(st != NULL, "n_stream_dopen() accepted the old unknown type");
    expect_int(die_calls, 1);
    expect_str(die_msg, "0: unknown stream type\n");
}
END_TEST

/* a read that fails returns -1, not the 0 of the end of the file; a read
   of 0 bytes returns 0 */
static void do_test_read_error(int type)
{
    const char *dir = NTEST_TMPPATH("nstream-read-error.d");
    char buf[16];
    tn_stream *st;
    int fd;

    mkdir(dir, 0700);
    fd = open(dir, O_RDONLY);           /* read(2) on it fails with EISDIR */
    fail_if(fd == -1, "cannot open %s", dir);
    st = n_stream_dopen(fd, "r", type);
    fail_if(st == NULL, "cannot open %s as stream type %d", dir, type);
    expect_int(n_stream_read(st, buf, sizeof(buf)), -1);
    expect_int(n_stream_read(st, buf, 0), 0);
    n_stream_close(st);
}

START_TEST(test_read_error)
{
    do_test_read_error(TN_STREAM_STDIO);
    do_test_read_error(TN_STREAM_GZIO);
    do_test_read_error(TN_STREAM_ZSTDIO);
}
END_TEST

/* The cut stream tests: a stream written in two halves with a flush
   between them, cut at the flush, as a writer that crashed or ran out of
   disk leaves it. Everything before the flush can be decoded, the end of
   the stream is missing. With newlines 0 the data is one long line, so
   n_stream_getline() reads it with getc. CUT_HALF puts the flush where no
   block or buffer of a decoder ends, CUT_SIZE / 2 where many do. */
#define CUT_SIZE (1024 * 1024)
#define CUT_HALF (CUT_SIZE / 2 + 1000)
static char cut_data[CUT_SIZE];

static void fill_cut_data(int newlines)
{
    size_t n = 0;

    while (n < CUT_SIZE) {
        char line[64];
        int len = snprintf(line, sizeof(line), "line %zu of the cut stream%c",
                           n, newlines ? '\n' : ' ');
        size_t left = CUT_SIZE - n;

        memcpy(cut_data + n, line, (size_t) len < left ? (size_t) len : left);
        n += len;
    }
}

static size_t write_cut_stream(const char *path, int newlines, size_t half)
{
    tn_stream *st;
    struct stat sb;

    fill_cut_data(newlines);
    st = n_stream_open(path, "w", TN_STREAM_UNKNOWN);
    fail_if(st == NULL, "cannot create %s", path);
    expect_int(n_stream_write(st, cut_data, half), half);
    expect_int(n_stream_flush(st), 0);
    fail_if(stat(path, &sb) != 0, "cannot stat %s", path);
    expect_int(n_stream_write(st, cut_data + half, CUT_SIZE - half), CUT_SIZE - half);
    expect_int(n_stream_close(st), 0);
    fail_if(truncate(path, sb.st_size) != 0, "cannot cut %s", path);
    return half;
}

/* reads with n_stream_read() up to the cut, checking what it gets */
static size_t read_to_cut(tn_stream *st, const char *path, size_t from)
{
    char buf[4096];
    size_t total = from;
    int n;

    while ((n = n_stream_read(st, buf, sizeof(buf))) > 0) {
        fail_if(total + n > CUT_SIZE || memcmp(buf, cut_data + total, n) != 0,
                "%s: not what was written, at %zu", path, total);
        total += n;
    }
    expect_int(n, -1);
    return total;
}

/* the data up to the cut, then -1, again on each read, and from the close */
static void do_test_cut_read(const char *path, size_t half)
{
    tn_stream *st;
    char buf[16];
    size_t total;

    write_cut_stream(path, 1, half);
    st = n_stream_open(path, "r", TN_STREAM_UNKNOWN);
    fail_if(st == NULL, "cannot open %s", path);
    total = read_to_cut(st, path, 0);
    fail_unless(total >= half, "%s: %zu of the %zu bytes before the cut",
                path, total, half);
    expect_int(n_stream_read(st, buf, 0), 0);
    expect_int(n_stream_read(st, buf, sizeof(buf)), -1);
    expect_int(n_stream_close(st), -1);
}

/* a cut met by n_stream_getline(), by gets or by getc, which cannot tell
   it from the end of the file, is reported by the next n_stream_read() */
static void do_test_cut_getline(const char *path, int newlines)
{
    size_t half = write_cut_stream(path, newlines, CUT_HALF), total = 0;
    tn_stream *st = n_stream_open(path, "r", TN_STREAM_UNKNOWN);
    char *line = NULL, buf[16];
    size_t size = 0;
    int n;

    fail_if(st == NULL, "cannot open %s", path);
    while ((n = n_stream_getline(st, &line, &size)) > 0) {
        fail_if(total + n > CUT_SIZE || memcmp(line, cut_data + total, n) != 0,
                "%s: not what was written, at %zu", path, total);
        total += n;
    }
    fail_unless(total >= half, "%s: %zu of the %zu bytes before the cut",
                path, total, half);
    expect_int(n_stream_read(st, buf, sizeof(buf)), -1);
    expect_int(n_stream_close(st), -1);
    n_free(line);
}

/* after a seek back the data before the cut reads as before and the cut
   is met again; a close before that still reports it */
static void do_test_cut_seek(const char *path)
{
    tn_stream *st;
    char buf[100];

    write_cut_stream(path, 1, CUT_HALF);
    st = n_stream_open(path, "r", TN_STREAM_UNKNOWN);
    fail_if(st == NULL, "cannot open %s", path);
    read_to_cut(st, path, 0);

    fail_if(n_stream_seek(st, 0, SEEK_SET) == -1, "%s: seek back failed", path);
    expect_int(n_stream_read(st, buf, sizeof(buf)), sizeof(buf));
    expect_int(memcmp(buf, cut_data, sizeof(buf)), 0);
    read_to_cut(st, path, sizeof(buf));

    fail_if(n_stream_seek(st, 0, SEEK_SET) == -1, "%s: seek back failed", path);
    expect_int(n_stream_read(st, buf, sizeof(buf)), sizeof(buf));
    expect_int(n_stream_close(st), -1);

    st = n_stream_open(path, "r", TN_STREAM_UNKNOWN);   /* past the cut */
    fail_if(n_stream_seek(st, CUT_SIZE - 10, SEEK_SET) == -1, "%s: seek failed", path);
    expect_int(n_stream_read(st, buf, sizeof(buf)), -1);
    expect_int(n_stream_close(st), -1);
}

/* an empty file reads as an empty stream; so do two whole streams one
   after the other, as one */
static void do_test_cut_not(const char *path)
{
    size_t half = CUT_HALF, total = 0;
    tn_stream *st;
    struct stat sb;
    char buf[4096];
    int fd, n;

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);  /* not even a header */
    fail_if(fd == -1 || close(fd) != 0, "cannot create %s", path);
    fail_if(stat(path, &sb) != 0 || sb.st_size != 0, "%s is not empty", path);
    st = n_stream_open(path, "r", TN_STREAM_UNKNOWN);
    fail_if(st == NULL, "cannot open %s", path);
    expect_int(n_stream_read(st, buf, sizeof(buf)), 0);
    expect_int(n_stream_close(st), 0);

    fill_cut_data(1);
    st = n_stream_open(path, "w", TN_STREAM_UNKNOWN);
    expect_int(n_stream_write(st, cut_data, half), half);
    expect_int(n_stream_close(st), 0);
    st = n_stream_open(path, "a", TN_STREAM_UNKNOWN);
    expect_int(n_stream_write(st, cut_data + half, CUT_SIZE - half), CUT_SIZE - half);
    expect_int(n_stream_close(st), 0);

    st = n_stream_open(path, "r", TN_STREAM_UNKNOWN);
    while ((n = n_stream_read(st, buf, sizeof(buf))) > 0) {
        fail_if(total + n > CUT_SIZE || memcmp(buf, cut_data + total, n) != 0,
                "%s: not what was written, at %zu", path, total);
        total += n;
    }
    expect_int(n, 0);
    expect_int(total, CUT_SIZE);
    expect_int(n_stream_close(st), 0);
}

START_TEST(test_cut_read)
{
    /* gzip only at CUT_HALF: zlib and zlib-ng miss a cut at CUT_SIZE / 2 */
    do_test_cut_read(NTEST_TMPPATH("nstream-cut.gz"), CUT_HALF);
    do_test_cut_read(NTEST_TMPPATH("nstream-cut.ngz"), CUT_HALF);
    do_test_cut_read(NTEST_TMPPATH("nstream-cut.zst"), CUT_HALF);
    do_test_cut_read(NTEST_TMPPATH("nstream-cut.zst"), CUT_SIZE / 2);
}
END_TEST

START_TEST(test_cut_getline)
{
    do_test_cut_getline(NTEST_TMPPATH("nstream-cut-lines.gz"), 1);
    do_test_cut_getline(NTEST_TMPPATH("nstream-cut-line.gz"), 0);
    do_test_cut_getline(NTEST_TMPPATH("nstream-cut-lines.ngz"), 1);
    do_test_cut_getline(NTEST_TMPPATH("nstream-cut-line.ngz"), 0);
    do_test_cut_getline(NTEST_TMPPATH("nstream-cut-lines.zst"), 1);
    do_test_cut_getline(NTEST_TMPPATH("nstream-cut-line.zst"), 0);
}
END_TEST

START_TEST(test_cut_seek)
{
    do_test_cut_seek(NTEST_TMPPATH("nstream-cut-seek.gz"));
    do_test_cut_seek(NTEST_TMPPATH("nstream-cut-seek.ngz"));
    do_test_cut_seek(NTEST_TMPPATH("nstream-cut-seek.zst"));
}
END_TEST

START_TEST(test_cut_not)
{
    do_test_cut_not(NTEST_TMPPATH("nstream-cut-not.gz"));
    do_test_cut_not(NTEST_TMPPATH("nstream-cut-not.ngz"));
    do_test_cut_not(NTEST_TMPPATH("nstream-cut-not.zst"));
}
END_TEST

START_TEST(test_printf_format_error)
{
    do_test_printf_format_error(NTEST_TMPPATH("nstream-printf-err.txt"));
    do_test_printf_format_error(NTEST_TMPPATH("nstream-printf-err.gz"));
    do_test_printf_format_error(NTEST_TMPPATH("nstream-printf-err.zst"));
}
END_TEST

static char hooked[16];
static size_t nhooked = 0;

/* records what it is given; *arg says whether the stream writes it too */
static int record_hook(const void *buf, size_t size, void *arg)
{
    fail_if(nhooked + size > sizeof(hooked), "hooked more than expected");
    memcpy(hooked + nhooked, buf, size);
    nhooked += size;
    return *(int *) arg;
}

START_TEST(test_printf_write_hook)
{
    const char *path = NTEST_TMPPATH("nstream-printf-hook.txt");
    char rbuf[16];
    int write = 0;
    tn_stream *st = n_stream_open(path, "w", TN_STREAM_UNKNOWN);

    fail_if(st == NULL, "cannot create %s", path);
    n_stream_set_write_hook(st, record_hook, &write);
    expect_int(n_stream_printf(st, "%s", "abc"), 3);   /* taken by the hook */
    write = 1;
    expect_int(n_stream_printf(st, "%s", "def"), 3);
    n_stream_close(st);
    expect_int(nhooked, 6);
    expect_int(memcmp(hooked, "abcdef", 6), 0);

    st = n_stream_open(path, "r", TN_STREAM_UNKNOWN);
    expect_int(n_stream_read(st, rbuf, sizeof(rbuf)), 3);
    expect_int(memcmp(rbuf, "def", 3), 0);
    n_stream_close(st);
}
END_TEST

START_TEST(test_open) {
    tn_stream *st = n_stream_open("/tmp/non_existing_file_trurlib_nstream.txt.zst", "r", TN_STREAM_UNKNOWN);
    n_assert(st == NULL);
}
END_TEST


NTEST_RUNNER("nstream",
            test_open,
            test_read_write,
            test_seek,
            test_open_type,
            test_stream_getline,
            test_printf_long,
            test_printf_format_error,
            test_close_error,
            test_printf_write_error,
            test_write_error_sets_errno,
            test_atomic,
            test_atomic_abort,
            test_atomic_mode_and_type,
            test_atomic_errors,
            test_read_error,
            test_cut_read,
            test_cut_getline,
            test_cut_seek,
            test_cut_not,
            test_printf_write_hook);
