/* libwup's system layer under the C library (picolibc): the heap, standard output, files on the pad's
 * filesystem (littlefs) and leaving the app. With these, the whole of <stdio.h>, <stdlib.h>, <string.h>... works
 * as anywhere: malloc, printf (to the log), fopen("save.dat", "wb") (relative paths are in the app's folder). */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "wup.h"

/* ---- the heap: from the end of the app's .bss to the end of its block (wup_ram.h): everything left, in one piece */
static uintptr_t brk_;
extern uint8_t __bss_end[];
void *sbrk(ptrdiff_t n) {
    uintptr_t lo = ((uintptr_t)__bss_end + 31) & ~(uintptr_t)31, hi = WUP_APP_END;
    if (!brk_) brk_ = lo;
    if ((n > 0 && (uintptr_t)n > hi - brk_) || (n < 0 && (uintptr_t)-n > brk_ - lo)) { errno = ENOMEM; return (void *)-1; }
    void *p = (void *)brk_; brk_ += (uintptr_t)n;
    return p;
}
size_t wup_mem_free(void) { return WUP_APP_END - (brk_ ? brk_ : (((uintptr_t)__bss_end + 31) & ~(uintptr_t)31)); }

/* ---- stdout and stderr: the log (the UART, and UDP port 28803 with networking up), a line at a time */
static int log_put(char c, FILE *f) {
    static char line[160]; static int n;
    (void)f;
    line[n++] = c;
    if (c == '\n' || n == (int)sizeof line - 1) { line[n] = 0; wup_log(line); n = 0; }
    return (unsigned char)c;
}
static FILE log_ = FDEV_SETUP_STREAM(log_put, NULL, NULL, _FDEV_SETUP_WRITE);
FILE *const stdin = &log_;
FILE *const stdout = &log_;
FILE *const stderr = &log_;

/* ---- files. Descriptors from FD_FILES up; below are sockets (net.c), which close() hands back to it. */
#define FD_FILES 32
#define NFILES   8
static struct { lfs_file_t f; int used; } files[NFILES];
int (*wup__sock_close)(int fd);                    /* descriptors below FD_FILES are sockets (net.c) */
ssize_t (*wup__sock_read)(int, void *, size_t), (*wup__sock_write)(int, const void *, size_t);
static int lfs_fail(int e) {
    errno = e == LFS_ERR_NOENT ? ENOENT : e == LFS_ERR_EXIST ? EEXIST : e == LFS_ERR_NOSPC ? ENOSPC : e == LFS_ERR_ISDIR ? EISDIR
          : e == LFS_ERR_NOTDIR ? ENOTDIR : e == LFS_ERR_NOMEM ? ENOMEM : e == LFS_ERR_NOTEMPTY ? ENOTEMPTY : e == LFS_ERR_INVAL ? EINVAL : EIO;
    return -1;
}
static const char *full(const char *p, char *out, size_t n) {       /* "x" -> "<the app's folder>/x" */
    if (p[0] == '/') return p;
    snprintf(out, n, "%s/%s", wup_app_dir(), p);
    return out;
}
static lfs_file_t *file(int fd) {
    if (fd < FD_FILES || fd >= FD_FILES + NFILES || !files[fd - FD_FILES].used) { errno = EBADF; return 0; }
    return &files[fd - FD_FILES].f;
}
int open(const char *path, int flags, ...) {
    char b[192]; int i = 0, lf, acc = flags & O_ACCMODE;
    if (!wup_fs_ok()) { errno = EIO; return -1; }
    while (i < NFILES && files[i].used) i++;
    if (i == NFILES) { errno = EMFILE; return -1; }
    if (acc != O_RDONLY) fs__used_refresh();          /* (a write: fs.c erases what is free 64 KB at a time) */
    lf = (acc == O_RDONLY ? LFS_O_RDONLY : acc == O_WRONLY ? LFS_O_WRONLY : LFS_O_RDWR) | (flags & O_CREAT ? LFS_O_CREAT : 0)
       | (flags & O_TRUNC ? LFS_O_TRUNC : 0) | (flags & O_APPEND ? LFS_O_APPEND : 0) | (flags & O_EXCL ? LFS_O_EXCL : 0);
    int r = lfs_file_open(&fs_lfs, &files[i].f, full(path, b, sizeof b), lf);
    if (r) return lfs_fail(r);
    files[i].used = 1;
    return FD_FILES + i;
}
int close(int fd) {
    if (fd < FD_FILES) { if (wup__sock_close) return wup__sock_close(fd); errno = EBADF; return -1; }
    lfs_file_t *f = file(fd); if (!f) return -1;
    int r = lfs_file_close(&fs_lfs, f);
    files[fd - FD_FILES].used = 0;
    return r ? lfs_fail(r) : 0;
}
ssize_t read(int fd, void *buf, size_t n) {
    if (fd >= 3 && fd < FD_FILES && wup__sock_read) return wup__sock_read(fd, buf, n);
    lfs_file_t *f = file(fd); if (!f) return -1;
    lfs_ssize_t r = lfs_file_read(&fs_lfs, f, buf, n);
    return r < 0 ? lfs_fail(r) : r;
}
ssize_t write(int fd, const void *buf, size_t n) {
    if (fd >= 3 && fd < FD_FILES && wup__sock_write) return wup__sock_write(fd, buf, n);
    lfs_file_t *f = file(fd); if (!f) return -1;
    lfs_ssize_t r = lfs_file_write(&fs_lfs, f, buf, n);
    return r < 0 ? lfs_fail(r) : r;
}
off_t lseek(int fd, off_t off, int whence) {
    lfs_file_t *f = file(fd); if (!f) return -1;
    lfs_soff_t r = lfs_file_seek(&fs_lfs, f, (lfs_soff_t)off, whence == SEEK_SET ? LFS_SEEK_SET : whence == SEEK_CUR ? LFS_SEEK_CUR : LFS_SEEK_END);
    return r < 0 ? lfs_fail(r) : r;
}
static void fill(struct stat *st, int dir, uint32_t size) {
    memset(st, 0, sizeof *st);
    st->st_mode = dir ? S_IFDIR | 0755 : S_IFREG | 0644; st->st_size = (off_t)size; st->st_nlink = 1;
}
int stat(const char *path, struct stat *st) {
    char b[192]; struct lfs_info i;
    int r = lfs_stat(&fs_lfs, full(path, b, sizeof b), &i);
    if (r) return lfs_fail(r);
    fill(st, i.type == LFS_TYPE_DIR, i.size);
    return 0;
}
int fstat(int fd, struct stat *st) {
    if (fd < FD_FILES) { memset(st, 0, sizeof *st); st->st_mode = S_IFCHR; return 0; }
    lfs_file_t *f = file(fd); if (!f) return -1;
    fill(st, 0, (uint32_t)lfs_file_size(&fs_lfs, f));
    return 0;
}
int isatty(int fd) { return fd < 3; }
int unlink(const char *path) { char b[192]; int r = lfs_remove(&fs_lfs, full(path, b, sizeof b)); return r ? lfs_fail(r) : 0; }
int rmdir(const char *path) { return unlink(path); }
int mkdir(const char *path, mode_t mode) { char b[192]; (void)mode; int r = lfs_mkdir(&fs_lfs, full(path, b, sizeof b)); return r ? lfs_fail(r) : 0; }
int rename(const char *from, const char *to) {
    char a[192], b[192];
    int r = lfs_rename(&fs_lfs, full(from, a, sizeof a), full(to, b, sizeof b));
    return r ? lfs_fail(r) : 0;
}
int getpid(void) { return 1; }
#include <signal.h>                                /* no signals on the pad: asking for one is fine, none ever comes */
int sigaction(int sig, const struct sigaction *act, struct sigaction *old) { (void)sig; (void)act; if (old) memset(old, 0, sizeof *old); return 0; }
int kill(int pid, int sig) { (void)pid; _exit(128 + sig); }

/* ---- folders: opendir, readdir, closedir */
#include <dirent.h>
#include <stdlib.h>
struct wup__dir { lfs_dir_t d; struct dirent e; };
DIR *opendir(const char *path) {
    char b[192];
    DIR *d = malloc(sizeof *d);
    if (!d) { errno = ENOMEM; return 0; }
    int r = lfs_dir_open(&fs_lfs, &d->d, full(path, b, sizeof b));
    if (r) { free(d); lfs_fail(r); return 0; }
    return d;
}
struct dirent *readdir(DIR *d) {
    struct lfs_info i;
    for (;;) {
        if (lfs_dir_read(&fs_lfs, &d->d, &i) <= 0) return 0;
        if (strcmp(i.name, ".") && strcmp(i.name, "..")) break;
    }
    d->e.d_ino = 1; d->e.d_type = i.type == LFS_TYPE_DIR ? DT_DIR : DT_REG;
    strncpy(d->e.d_name, i.name, sizeof d->e.d_name - 1); d->e.d_name[sizeof d->e.d_name - 1] = 0;
    return &d->e;
}
int closedir(DIR *d) { lfs_dir_close(&fs_lfs, &d->d); free(d); return 0; }
