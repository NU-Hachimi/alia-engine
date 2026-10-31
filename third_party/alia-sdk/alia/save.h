/* Cart saves (software plan §5.5). Header-only, C99 / C++, POSIX.
 * Mirrors sdk/python/alia/save.py: same files, same rules, so either side can read
 * what the other wrote.
 *
 * Saves live in $ALIA_SAVE_DIR (/save inside the cart sandbox), or the directory given
 * to alia_save_set_dir(). A write goes to a temp file, is fsynced, renamed over the
 * save and the directory is fsynced; the two previous versions stay as NAME.~1~ and
 * NAME.~2~. Writes that would take the directory over $ALIA_SAVE_QUOTA_KB fail with
 * -ENOSPC. Names: [A-Za-z0-9][A-Za-z0-9._-]{0,63}, not ending in ".tmp".
 *
 * All functions return >= 0 on success, -errno on failure.
 */
#ifndef ALIA_SAVE_H
#define ALIA_SAVE_H

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ALIA_SAVE_NAME_MAX 64
#define ALIA_SAVE_KEEP 2 /* previous versions kept */

static char alia_save_dir_[4096];

/* Use DIR instead of $ALIA_SAVE_DIR (tools, tests, running outside a cart). */
static inline void alia_save_set_dir(const char* dir) {
    snprintf(alia_save_dir_, sizeof alia_save_dir_, "%s", dir ? dir : "");
}

static inline const char* alia_save_dir(void) {
    if (alia_save_dir_[0]) return alia_save_dir_;
    const char* d = getenv("ALIA_SAVE_DIR");
    return d && d[0] ? d : NULL;
}

static inline int alia_save_name_ok_(const char* n) {
    size_t len = strlen(n);
    if (len == 0 || len > ALIA_SAVE_NAME_MAX) return 0;
    for (size_t i = 0; i < len; i++) {
        char c = n[i];
        int alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!alnum && (i == 0 || (c != '.' && c != '_' && c != '-'))) return 0;
    }
    return !(len >= 4 && strcmp(n + len - 4, ".tmp") == 0);
}

/* path of version V of NAME (0 = current) */
static inline int alia_save_path_(char* out, size_t cap, const char* name, int v) {
    const char* d = alia_save_dir();
    if (!d) return -ENOENT;
    if (!alia_save_name_ok_(name)) return -EINVAL;
    int n = v ? snprintf(out, cap, "%s/%s.~%d~", d, name, v) : snprintf(out, cap, "%s/%s", d, name);
    return n < 0 || (size_t)n >= cap ? -ENAMETOOLONG : 0;
}

static inline int alia_save_fsync_dir_(void) {
    int fd = open(alia_save_dir(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) return -errno;
    int r = fsync(fd) ? -errno : 0;
    close(fd);
    return r;
}

static inline long long alia_save_dir_usage_(void) {
    DIR* dir = opendir(alia_save_dir());
    if (!dir) return -errno;
    long long total = 0;
    struct dirent* e;
    struct stat st;
    while ((e = readdir(dir)))
        if (fstatat(dirfd(dir), e->d_name, &st, 0) == 0 && S_ISREG(st.st_mode)) total += st.st_size;
    closedir(dir);
    return total;
}

static inline int alia_save_write(const char* name, const void* data, size_t len) {
    char cur[4200], tmp[4200], a[4200], b[4200];
    int r = alia_save_path_(cur, sizeof cur, name, 0);
    if (r) return r;
    const char* q = getenv("ALIA_SAVE_QUOTA_KB");
    if (q && q[0]) {
        long long quota = atoll(q) * 1024, used = alia_save_dir_usage_();
        struct stat st;
        alia_save_path_(a, sizeof a, name, ALIA_SAVE_KEEP);
        if (used < 0) return (int)used;
        if (stat(a, &st) == 0) used -= st.st_size;
        if (used + (long long)len > quota) return -ENOSPC;
    }
    snprintf(tmp, sizeof tmp, "%s/.%s.tmp", alia_save_dir(), name);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -errno;
    const char* p = (const char*)data;
    size_t left = len;
    while (left) {
        ssize_t n = write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            r = -errno;
            close(fd);
            unlink(tmp);
            return r;
        }
        p += n;
        left -= (size_t)n;
    }
    if (fsync(fd)) {
        r = -errno;
        close(fd);
        unlink(tmp);
        return r;
    }
    close(fd);
    for (int v = ALIA_SAVE_KEEP; v > 0; v--) { /* ~1~ -> ~2~, current -> ~1~ */
        alia_save_path_(a, sizeof a, name, v - 1);
        alia_save_path_(b, sizeof b, name, v);
        if (rename(a, b) && errno != ENOENT) return -errno;
    }
    if (rename(tmp, cur)) return -errno; /* before this, read() falls back to ~1~ */
    return alia_save_fsync_dir_();
}

/* Size of the newest readable version, or -ENOENT. */
static inline long long alia_save_size(const char* name) {
    char p[4200];
    struct stat st;
    for (int v = 0; v <= ALIA_SAVE_KEEP; v++) {
        int r = alia_save_path_(p, sizeof p, name, v);
        if (r) return r;
        if (stat(p, &st) == 0) return st.st_size;
    }
    return -ENOENT;
}

/* Read version V (0 = current; with v == 0, falls back to older versions). Returns the
 * number of bytes read, or -EMSGSIZE if CAP is too small (see alia_save_size). */
static inline long long alia_save_read_version(const char* name, int v, void* buf, size_t cap) {
    char p[4200];
    for (int i = v; i <= (v == 0 ? ALIA_SAVE_KEEP : v); i++) {
        int r = alia_save_path_(p, sizeof p, name, i);
        if (r) return r;
        int fd = open(p, O_RDONLY);
        if (fd < 0) {
            if (errno == ENOENT) continue;
            return -errno;
        }
        struct stat st;
        if (fstat(fd, &st)) {
            r = -errno;
            close(fd);
            return r;
        }
        if ((size_t)st.st_size > cap) {
            close(fd);
            return -EMSGSIZE;
        }
        size_t got = 0;
        while (got < (size_t)st.st_size) {
            ssize_t n = read(fd, (char*)buf + got, (size_t)st.st_size - got);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            got += (size_t)n;
        }
        close(fd);
        return (long long)got;
    }
    return -ENOENT;
}

static inline long long alia_save_read(const char* name, void* buf, size_t cap) {
    return alia_save_read_version(name, 0, buf, cap);
}

/* Names of existing saves, sorted, up to MAX; returns how many there are in total. */
static inline int alia_save_cmp_(const void* a, const void* b) { return strcmp((const char*)a, (const char*)b); }
static inline int alia_save_list(char (*names)[ALIA_SAVE_NAME_MAX + 1], int max) {
    const char* d = alia_save_dir();
    if (!d) return -ENOENT;
    DIR* dir = opendir(d);
    if (!dir) return -errno;
    int count = 0;
    struct dirent* e;
    char n[ALIA_SAVE_NAME_MAX + 8];
    while ((e = readdir(dir))) {
        if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof n) continue;
        snprintf(n, sizeof n, "%s", e->d_name);
        size_t len = strlen(n);
        if (len > 4 && n[len - 1] == '~') { /* NAME.~K~ -> NAME */
            char* t = strrchr(n, '.');
            if (t && t[1] == '~') *t = 0;
            else continue;
        }
        if (!alia_save_name_ok_(n)) continue;
        int dup = 0;
        for (int i = 0; i < count && i < max; i++) dup |= strcmp(names[i], n) == 0;
        if (dup) continue;
        if (count < max) snprintf(names[count], ALIA_SAVE_NAME_MAX + 1, "%s", n);
        count++;
    }
    closedir(dir);
    qsort(names, (size_t)(count < max ? count : max), sizeof names[0], alia_save_cmp_);
    return count;
}

/* Make version V current; the current one becomes ~1~, so a rollback can be undone. */
static inline int alia_save_rollback(const char* name, int v) {
    long long size;
    char p[4200];
    struct stat st;
    int r = alia_save_path_(p, sizeof p, name, v);
    if (r) return r;
    if (stat(p, &st)) return -errno;
    size = st.st_size;
    void* buf = malloc(size ? (size_t)size : 1);
    if (!buf) return -ENOMEM;
    long long got = alia_save_read_version(name, v, buf, (size_t)size);
    r = got < 0 ? (int)got : alia_save_write(name, buf, (size_t)got);
    free(buf);
    return r;
}

static inline int alia_save_delete(const char* name) {
    char p[4200];
    for (int v = 0; v <= ALIA_SAVE_KEEP; v++) {
        int r = alia_save_path_(p, sizeof p, name, v);
        if (r) return r;
        if (unlink(p) && errno != ENOENT) return -errno;
    }
    return alia_save_fsync_dir_();
}

#ifdef __cplusplus
}
#endif

#endif
