/*
  TRURLib
  Replacing a file atomically (not the atomic operations of <stdatomic.h>)
*/

#ifndef TRURL_ATOMIC_H
#define TRURL_ATOMIC_H

/* The new contents are written to the hidden .<name>.tmp.XXXXXX next to
   the target and n_atomic_commit() renames it into place, so the target
   is replaced only by a whole file and a failure leaves it as it was.

   - a symlink is followed to its target, which is replaced, as a write
     through open(O_TRUNC) would update it; a hard link is split, the
     other names keep the old content;
   - the target is a regular file or a new one: a directory is EISDIR,
     a device, a fifo or a socket EINVAL;
   - the directory must be writable, unlike for a plain open();
   - the new file gets the mode (without setuid/setgid) and, when the
     writer may give it, the owner of the file it replaces; a new one gets
     0666 & ~umask, the umask of the n_atomic_open();
   - a killed writer leaves its .<name>.tmp.XXXXXX behind. */
typedef struct trurl_atomic_private tn_atomic;

/* fsync the new file before the rename and its directory after it, so
   that a crash leaves either the old file or the new one on disk.  The
   commit waits for the device; a failed last sync is reported although
   the file is already replaced. */
#define TN_ATOMIC_SYNC  (1 << 0)

/* NULL with errno set: EINVAL for an unknown flag or a special file,
   EISDIR, ENOENT for "", ELOOP, ENAMETOOLONG, or what lstat(), readlink()
   and mkostemp() gave */
tn_atomic *n_atomic_open(const char *path, unsigned flags);

/* the descriptor of the temporary file, 0600 and close-on-exec until the
   commit; it is closed by the commit or the abort, so a stream opened on
   it needs a dup of it (F_DUPFD_CLOEXEC, a dup() loses the flag) */
int n_atomic_fd(const tn_atomic *af);

/* the temporary file's name */
const char *n_atomic_tmpath(const tn_atomic *af);

/* sets the mode and owner, closes and renames; on a failure before the
   rename the temporary file is removed.  af is freed either way.
   0, or -1 with errno set */
int n_atomic_commit(tn_atomic *af);

/* closes and removes the temporary file, the target is left alone; af is
   freed.  0, or -1 with errno set when the close or the unlink failed */
int n_atomic_abort(tn_atomic *af);

#endif
