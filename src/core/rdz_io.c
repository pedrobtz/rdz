/* 64-bit file offsets on 32-bit glibc, and POSIX names under strict C99.
   Both must precede the first system header. */
#ifndef _WIN32
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "rdz_io.h"

/* Temporary and backup names: R thread only (rdz_io.h). */
static unsigned long rdz_name_counter;

#ifdef _WIN32
static wchar_t *rdz_widen(const char *path)
{
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    wchar_t *w;
    if (n <= 0) return NULL;
    w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w) return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, w, n) <= 0) {
        free(w);
        return NULL;
    }
    return w;
}

static FILE *rdz_fopen_read(const char *path)
{
    wchar_t *w = rdz_widen(path);
    FILE *fp;
    if (!w) {
        errno = EINVAL;
        return NULL;
    }
    fp = _wfopen(w, L"rb");
    free(w);
    return fp;
}

static int rdz_seek(FILE *fp, uint64_t offset, int whence)
{
    if (offset > (uint64_t)INT64_MAX) return -1;
    return _fseeki64(fp, (__int64)offset, whence);
}

static int64_t rdz_tell(FILE *fp)
{
    return (int64_t)_ftelli64(fp);
}

static int rdz_remove(const char *path)
{
    wchar_t *w = rdz_widen(path);
    int r;
    if (!w) return -1;
    r = _wremove(w);
    free(w);
    return r;
}

static int rdz_rename(const char *from, const char *to)
{
    wchar_t *a = rdz_widen(from), *b = rdz_widen(to);
    int r = -1;
    if (a && b) r = _wrename(a, b);
    free(a);
    free(b);
    return r;
}

static int rdz_replace(const char *from, const char *to)
{
    wchar_t *a = rdz_widen(from), *b = rdz_widen(to);
    int r = -1;
    if (a && b && MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING)) {
        r = 0;
    } else {
        switch (GetLastError()) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND: errno = ENOENT; break;
        case ERROR_NOT_SAME_DEVICE: errno = EXDEV; break;
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL: errno = ENOSPC; break;
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY: errno = ENOMEM; break;
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION: errno = EACCES; break;
        default: errno = EIO; break;
        }
    }
    free(a);
    free(b);
    return r;
}

/* 1 if path exists, 0 if not, -1 (errno set) if it cannot be told. Sets
   *is_dir. */
static int rdz_exists(const char *path, int *is_dir, unsigned *mode)
{
    wchar_t *w = rdz_widen(path);
    struct _stat64 st;
    int r;
    if (!w) {
        errno = EINVAL;
        return -1;
    }
    r = _wstat64(w, &st);
    free(w);
    if (r != 0) return errno == ENOENT ? 0 : -1;
    *is_dir = (st.st_mode & _S_IFDIR) != 0;
    *mode = (unsigned)st.st_mode;
    return 1;
}

static int rdz_create_exclusive(const char *path, FILE **out)
{
    wchar_t *w = rdz_widen(path);
    int fd;
    if (!w) {
        errno = EINVAL;
        return -1;
    }
    fd = _wopen(w, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
    free(w);
    if (fd < 0) return -1;
    *out = _fdopen(fd, "wb");
    if (!*out) {
        int saved = errno;
        _close(fd);
        rdz_remove(path);
        errno = saved;
        return -1;
    }
    return 0;
}

static unsigned long rdz_pid(void)
{
    return (unsigned long)_getpid();
}

/* A symbolic link's target, to write in its place: not on Windows, where
   rdz replaces what the path names. */
static char *rdz_link_target(const char *path)
{
    (void)path;
    return NULL;
}
#else
static FILE *rdz_fopen_read(const char *path)
{
    return fopen(path, "rb");
}

static int rdz_seek(FILE *fp, uint64_t offset, int whence)
{
    /* off_t is 64 bits here (_FILE_OFFSET_BITS above). */
    if (offset > (uint64_t)INT64_MAX) return -1;
    return fseeko(fp, (off_t)offset, whence);
}

static int64_t rdz_tell(FILE *fp)
{
    return (int64_t)ftello(fp);
}

static int rdz_remove(const char *path)
{
    return remove(path);
}

static int rdz_rename(const char *from, const char *to)
{
    return rename(from, to);
}

static int rdz_replace(const char *from, const char *to)
{
    return rename(from, to);
}

/* A symbolic link's target, so that writing through the link replaces the
   file it names, not the link: one level (rdz_outfile_open() follows a
   chain), relative to the link's directory (malloc()ed; NULL for anything
   but a readable link). */
static char *rdz_link_target(const char *path)
{
    struct stat st;
    char buf[4096], *out;
    ssize_t n;
    size_t dir = strlen(path);
    if (lstat(path, &st) != 0 || !S_ISLNK(st.st_mode)) return NULL;
    n = readlink(path, buf, sizeof buf);
    if (n <= 0 || (size_t)n >= sizeof buf) return NULL;
    if (buf[0] == '/') dir = 0;
    else while (dir > 0 && path[dir - 1] != '/') dir--;
    out = (char *)malloc(dir + (size_t)n + 1);
    if (!out) return NULL;
    memcpy(out, path, dir);
    memcpy(out + dir, buf, (size_t)n);
    out[dir + (size_t)n] = 0;
    return out;
}

static int rdz_exists(const char *path, int *is_dir, unsigned *mode)
{
    struct stat st;
    if (stat(path, &st) != 0) return errno == ENOENT ? 0 : -1;
    *is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
    *mode = (unsigned)st.st_mode;
    return 1;
}

static int rdz_create_exclusive(const char *path, FILE **out)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0) return -1;
    *out = fdopen(fd, "wb");
    if (!*out) {
        int saved = errno;
        close(fd);
        remove(path);
        errno = saved;
        return -1;
    }
    return 0;
}

static unsigned long rdz_pid(void)
{
    return (unsigned long)getpid();
}
#endif

int rdz_infile_open(rdz_infile *f, const char *path, rdz_error *e)
{
    int64_t end;
    f->size = 0;
    f->mem = NULL;
    f->fp = rdz_fopen_read(path);
    if (!f->fp) return rdz_io_errno(e, errno);
    if (rdz_seek(f->fp, 0, SEEK_END) != 0 || (end = rdz_tell(f->fp)) < 0) {
        int saved = errno;
        rdz_infile_close(f);
        return rdz_io_errno(e, saved);
    }
    f->size = (uint64_t)end;
    return 0;
}

void rdz_infile_open_memory(rdz_infile *f, const uint8_t *data, size_t n)
{
    f->fp = NULL;
    f->mem = data;
    f->size = (uint64_t)n;
}

int rdz_infile_read_at(rdz_infile *f, uint64_t offset, void *buf, size_t n, rdz_error *e)
{
    if (n == 0) return 0;
    if (!f->fp) {
        if (!f->mem || offset > f->size || (uint64_t)n > f->size - offset) {
            return rdz_invalid(e, "file is truncated");
        }
        memcpy(buf, f->mem + offset, n);
        return 0;
    }
    if (rdz_seek(f->fp, offset, SEEK_SET) != 0) return rdz_io_errno(e, errno);
    if (fread(buf, 1, n, f->fp) != n) {
        if (ferror(f->fp)) return rdz_io_errno(e, errno);
        return rdz_invalid(e, "file is truncated");
    }
    return 0;
}

void rdz_infile_close(rdz_infile *f)
{
    if (f->fp) fclose(f->fp);
    f->fp = NULL;
    f->mem = NULL;
}

void rdz_outfile_init(rdz_outfile *f)
{
    f->fp = NULL;
    f->destination = NULL;
    f->temporary = NULL;
    f->position = 0;
    f->have_mode = 0;
    f->links = 0;
    f->mode = 0;
    f->to_memory = 0;
    zb_buf_init(&f->memory);
}

static int rdz_is_separator(char c)
{
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

/* dir + "." + base + suffix, where dir keeps its trailing separator. */
static char *rdz_sibling(const char *path, const char *suffix)
{
    size_t n = strlen(path), cut = n, len;
    char *out;
    while (cut > 0 && !rdz_is_separator(path[cut - 1])) cut--;
    len = n + 1 + strlen(suffix) + 1;
    out = (char *)malloc(len);
    if (!out) return NULL;
    memcpy(out, path, cut);
    out[cut] = '.';
    memcpy(out + cut + 1, path + cut, n - cut);
    strcpy(out + 1 + n, suffix);
    return out;
}

int rdz_outfile_open(rdz_outfile *f, const char *path, rdz_error *e)
{
    int is_dir = 0, attempt, exists, failed;
    unsigned mode = 0;
    size_t n;
    char *target;
    if (!path) {
        f->to_memory = 1;
        if (zb_buf_alloc(&f->memory, 0, 0)) return rdz_memory(e, "the output");
        return 0;
    }
    target = f->links < 32 ? rdz_link_target(path) : NULL;
    if (target) {
        f->links++;
        failed = rdz_outfile_open(f, target, e);
        free(target);
        return failed;
    }
    exists = rdz_exists(path, &is_dir, &mode);
    n = strlen(path);

    if (n == 0 || rdz_is_separator(path[n - 1])) {
        return rdz_invalid(e, "destination has no file name");
    }
    if (exists < 0) return rdz_io_errno(e, errno);
    if (exists && is_dir) return rdz_invalid(e, "destination is a directory");
    f->have_mode = exists;
    f->mode = mode & 0777u; /* not setuid, setgid or sticky */
    f->destination = (char *)malloc(n + 1);
    if (!f->destination) return rdz_memory(e, "the output path");
    memcpy(f->destination, path, n + 1);

    for (attempt = 0; attempt < 128; attempt++) {
        char suffix[64];
        snprintf(suffix, sizeof suffix, "-rdz-%lu-%lu.tmp", rdz_pid(), rdz_name_counter++);
        f->temporary = rdz_sibling(path, suffix);
        if (!f->temporary) return rdz_memory(e, "the output path");
        if (rdz_create_exclusive(f->temporary, &f->fp) == 0) {
            /* one write per MiB, as a block's worth, not one per small block */
            setvbuf(f->fp, NULL, _IOFBF, (size_t)1 << 20);
            return 0;
        }
        {
            int saved = errno;
            free(f->temporary);
            f->temporary = NULL;
            if (saved != EEXIST) return rdz_io_errno(e, saved);
        }
    }
    return rdz_invalid(e, "could not create a unique temporary output file");
}

int rdz_outfile_write(rdz_outfile *f, const void *data, size_t n, rdz_error *e)
{
    if (n == 0) return 0;
    if (f->to_memory) {
        if (zb_put_bytes(&f->memory, data, n)) return rdz_memory(e, "the output");
    } else if (fwrite(data, 1, n, f->fp) != n) {
        return rdz_io_errno(e, errno);
    }
    if (rdz_add_u64(f->position, (uint64_t)n, &f->position, e)) return 1;
    return 0;
}

int rdz_outfile_commit(rdz_outfile *f, rdz_error *e)
{
    int failed, saved, is_dir = 0, exists;
    unsigned mode = 0;
    if (f->to_memory) return 0; /* the bytes stay in f->memory */
    failed = fflush(f->fp) != 0 || ferror(f->fp);
    saved = errno;
    if (fclose(f->fp) != 0 && !failed) {
        failed = 1;
        saved = errno;
    }
    f->fp = NULL;
    if (failed) return rdz_io_errno(e, saved);
#ifndef _WIN32
    if (f->have_mode && chmod(f->temporary, (mode_t)f->mode) != 0) return rdz_io_errno(e, errno);
#endif
    if (rdz_replace(f->temporary, f->destination) == 0) goto committed;
    saved = errno;
    exists = rdz_exists(f->destination, &is_dir, &mode);
    if (exists != 1) return rdz_io_errno(e, saved);
    {
        /* A rename that cannot replace: move the destination aside first. */
        char *backup = NULL;
        int attempt;
        for (attempt = 0; attempt < 128; attempt++) {
            char suffix[48];
            snprintf(suffix, sizeof suffix, "-rdz-%lu.backup", rdz_name_counter++);
            free(backup);
            backup = rdz_sibling(f->destination, suffix);
            if (!backup) return rdz_memory(e, "the backup path");
            if (rdz_exists(backup, &is_dir, &mode) == 0) break;
        }
        if (attempt == 128) {
            free(backup);
            return rdz_invalid(e, "could not create a unique replacement backup path");
        }
        if (rdz_rename(f->destination, backup) != 0) {
            saved = errno;
            free(backup);
            return rdz_io_errno(e, saved);
        }
        if (rdz_rename(f->temporary, f->destination) != 0) {
            saved = errno;
            rdz_rename(backup, f->destination);
            free(backup);
            return rdz_io_errno(e, saved);
        }
        rdz_remove(backup);
        free(backup);
    }
committed:
    free(f->temporary);
    f->temporary = NULL;
    return 0;
}

void rdz_outfile_discard(rdz_outfile *f)
{
    zb_buf_release(&f->memory);
    f->to_memory = 0;
    if (f->fp) fclose(f->fp);
    f->fp = NULL;
    if (f->temporary) rdz_remove(f->temporary);
    free(f->temporary);
    f->temporary = NULL;
    free(f->destination);
    f->destination = NULL;
}
