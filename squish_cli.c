/* Copyright (C) 2026  Paige Julianne Sullivan
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/* squish — command-line front end for libsquish */
#include "squish.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <io.h>
#  include <direct.h>
#  include <wchar.h>
#  define SQ_ISATTY(f) _isatty(_fileno(f))
#else
#  include <unistd.h>
#  include <sys/types.h>
#  include <sys/stat.h>
#  include <dirent.h>
#  define SQ_ISATTY(f) isatty(fileno(f))
#endif

/* wall clock: clock() sums CPU time across threads, useless with -t */
static double now_sec(void) {
#if defined(_WIN32)
    return (double)GetTickCount64() / 1000.0;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

static long long file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
#if defined(_WIN32)
    long long n = _ftelli64(f);     /* ftell is 32-bit here: wrong past 2 GiB */
#else
    long long n = ftello(f);
#endif
    fclose(f);
    return n;
}

static int usage(void) {
    fprintf(stderr,
        "SQUISH %s — context-mixing compressor\n"
        "usage: squish [-q] [-t N] c input output   (compress)\n"
        "       squish [-q] [-t N] d input output   (decompress)\n"
        "       squish [-q] [-t N] s input output   (make self-extracting archive)\n"
        "  -q, --quiet     no progress or summary; errors only\n"
        "  -t, --threads N use N threads, 0 = all cores (compress default: 1,\n"
        "                  which keeps the ratio-optimal single-block format;\n"
        "                  decompress default: all cores)\n"
        "  -b, --block N   with -t: split input into N MiB blocks (default 16;\n"
        "                  smaller = more parallelism, slightly worse ratio)\n"
        "\n"
        "'input' may be a file or a directory. A directory is packed into a\n"
        "single archive stream; 'd' recreates the tree under 'output'.\n"
        "'s' writes a self-extracting executable: running `output` (no squish\n"
        "needed) unpacks the embedded file or directory. On Windows, name it\n"
        "`*.exe`.\n",
        squish_version());
    return 2;
}

/* Live status line, redrawn in place on stderr; only shown on a terminal
 * so redirected output doesn't fill with carriage returns. */
typedef struct {
    const char *verb;
    double      t0;
    int         last_pct;   /* last percent drawn; -1 = nothing drawn yet */
} status;

static void draw_status(uint64_t done, uint64_t total, void *user) {
    status *st = (status *)user;
    int pct = total ? (int)(100.0 * (double)done / (double)total) : 100;
    if (pct == st->last_pct) return;
    st->last_pct = pct;
    double dt = now_sec() - st->t0;
    fprintf(stderr, "\r%s: %3d%%  %.1f / %.1f MB  %.2f MB/s ",
        st->verb, pct, (double)done / 1e6, (double)total / 1e6,
        dt > 0 ? (double)done / 1e6 / dt : 0.0);
    fflush(stderr);
}

static void clear_status(const status *st) {
    if (st->last_pct >= 0) fprintf(stderr, "\r%60s\r", "");
}

/* ============================ self-extracting ============================ *
 * A self-extracting archive is this CLI itself (used as a stub) followed by a
 * compressed payload, the stored original name, and a fixed trailer:
 *
 *     [ stub executable ][ payload ][ name ][ 32-byte trailer ]
 *      0                  off                 filesize - 32
 *
 * The trailer, read from the end of the file, is (all integers little-endian):
 *     magic[8]="SQSFX01\n" | payload_off u64 | payload_len u64 |
 *     name_len u32 | flags u32 (0)
 *
 * At start-up the CLI reads its own trailing 32 bytes: a valid trailer means
 * "I am an archive" and it extracts; otherwise it is the ordinary tool. The
 * CLI is statically linked, so an archive needs no libsquish at run time. */

#define SFX_MAGIC       "SQSFX01\n"
#define SFX_MAGIC_LEN   8u
#define SFX_TRAILER_LEN 32u          /* magic8 + off8 + len8 + name4 + flags4 */
#define SFX_MAX_NAME    4096u

static const char *g_argv0 = NULL;   /* for the /proc-less self-open fallback */

static void put_u32le(unsigned char *p, uint32_t v) {
    p[0]=(unsigned char)v;       p[1]=(unsigned char)(v>>8);
    p[2]=(unsigned char)(v>>16); p[3]=(unsigned char)(v>>24);
}
static void put_u64le(unsigned char *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (unsigned char)(v >> (8*i));
}
static uint32_t get_u32le(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 |
           (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static uint64_t get_u64le(const unsigned char *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8*i);
    return v;
}

/* 64-bit seek/tell (plain fseek/ftell are 32-bit on Windows). */
#if defined(_WIN32)
#  define sq_fseek64(f,o) _fseeki64((f),(o),SEEK_SET)
#  define sq_ftell64(f)   _ftelli64(f)
#else
#  define sq_fseek64(f,o) fseeko((f),(off_t)(o),SEEK_SET)
#  define sq_ftell64(f)   ftello(f)
#endif

/* Open the running executable for reading ("rb"), or NULL on failure. */
static FILE *open_self(void) {
#if defined(_WIN32)
    DWORD cap = 512;
    for (;;) {
        wchar_t *path = (wchar_t *)malloc(cap * sizeof *path);
        if (!path) return NULL;
        DWORD n = GetModuleFileNameW(NULL, path, cap);
        if (n == 0) { free(path); return NULL; }
        if (n < cap) { FILE *f = _wfopen(path, L"rb"); free(path); return f; }
        free(path);                          /* n == cap: truncated, grow */
        if (cap >= (1u << 20)) return NULL;
        cap *= 2;
    }
#else
    char buf[8192];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0 && (size_t)n < sizeof buf) {
        buf[n] = '\0';
        FILE *f = fopen(buf, "rb");
        if (f) return f;
    }
    /* Fallback for systems without /proc: argv[0] if it names a path. */
    return g_argv0 ? fopen(g_argv0, "rb") : NULL;
#endif
}

typedef struct {
    uint64_t  payload_off, payload_len;
    uint32_t  name_len;
    long long filesize;
} sfx_info;

/* True (and fills *info) if the running executable carries a valid trailer. */
static int sfx_probe(sfx_info *info) {
    FILE *f = open_self();
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long long sz = sq_ftell64(f);
    if (sz < (long long)SFX_TRAILER_LEN) { fclose(f); return 0; }
    unsigned char t[SFX_TRAILER_LEN];
    if (sq_fseek64(f, sz - (long long)SFX_TRAILER_LEN) != 0 ||
        fread(t, 1, SFX_TRAILER_LEN, f) != SFX_TRAILER_LEN) {
        fclose(f); return 0;
    }
    fclose(f);
    if (memcmp(t, SFX_MAGIC, SFX_MAGIC_LEN) != 0) return 0;
    uint64_t off  = get_u64le(t + 8);
    uint64_t plen = get_u64le(t + 16);
    uint32_t nlen = get_u32le(t + 24);
    if (off == 0 || plen == 0 || nlen > SFX_MAX_NAME) return 0;
    /* [stub off][payload plen][name nlen][trailer 32] must fill the file. */
    uint64_t tail = plen + (uint64_t)nlen + SFX_TRAILER_LEN;
    if (tail > (uint64_t)sz || off != (uint64_t)sz - tail) return 0;
    info->payload_off = off; info->payload_len = plen;
    info->name_len = nlen;   info->filesize = sz;
    return 1;
}

/* Read [off, off+len) of the running executable into a fresh buffer. */
static int sfx_read_range(uint64_t off, uint64_t len, unsigned char **out) {
    if (len > (uint64_t)(size_t)-1) return -1;
    FILE *f = open_self();
    if (!f) return -1;
    if (sq_fseek64(f, (long long)off) != 0) { fclose(f); return -1; }
    unsigned char *buf = (unsigned char *)malloc(len ? (size_t)len : 1);
    if (!buf) { fclose(f); return -1; }
    if (len && fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf); fclose(f); return -1;
    }
    fclose(f);
    *out = buf;
    return 0;
}

/* Read a whole file into a fresh buffer (self for the stub, or the input). */
static int read_file_all(FILE *f, unsigned char **out, size_t *out_len) {
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long long n = sq_ftell64(f);
    if (n < 0 || (unsigned long long)n > (size_t)-1) { fclose(f); return -1; }
    rewind(f);
    unsigned char *buf = (unsigned char *)malloc(n ? (size_t)n : 1);
    if (!buf) { fclose(f); return -1; }
    if (n && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf); fclose(f); return -1;
    }
    fclose(f);
    *out = buf; *out_len = (size_t)n;
    return 0;
}

/* Last path component, with no directory or drive prefix; never traverses. */
static const char *sfx_basename(const char *name) {
    const char *b = name;
    for (const char *p = name; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':') b = p + 1;
    if (!*b || !strcmp(b, ".") || !strcmp(b, "..")) return "extracted.out";
    return b;
}

/* ============================ directory archive ========================== *
 * squish compresses a directory by first serializing its whole tree into a
 * single "SQAR" byte stream (built here), then handing that stream to the
 * ordinary compressor — so the compression engine never has to know about
 * files or directories. On decompression the CLI decompresses to a buffer
 * and, if it begins with the SQAR magic, unpacks the tree; otherwise the
 * buffer is a single file and is written out verbatim, exactly as before.
 *
 * Layout (all integers little-endian). Header:
 *     magic[8]="SQAR01\n\x1a" | version u32 (1) | flags u32 (0) | count u64
 * then `count` entries, each:
 *     type u8 (0=file,1=dir) | mode u32 | path_len u32 | data_len u64 |
 *     path[path_len] (relative, '/'-separated, UTF-8) | data[data_len]
 * Directories carry data_len 0. Full spec: docs/FORMAT.md §12. */

#define SQAR_VERSION  1u
#define SQAR_HDR_LEN  24u              /* magic8 + version4 + flags4 + count8 */
#define SQAR_ENT_LEN  17u              /* type1 + mode4 + plen4 + dlen8       */
#define SQAR_MAX_PATH 65535u
static const unsigned char SQAR_MAGIC[8] =
    { 'S','Q','A','R','0','1','\n','\x1a' };

/* --- growable byte buffer (used to accumulate the archive) --------------- */
typedef struct { unsigned char *p; size_t len, cap; } gbuf;

static int gbuf_put(gbuf *b, const void *d, size_t n) {
    if (n) {
        if (b->len + n < b->len) return -1;            /* size_t overflow */
        if (b->len + n > b->cap) {
            size_t nc = b->cap ? b->cap : 4096;
            while (nc < b->len + n) {
                if (nc > (size_t)-1 / 2) { nc = b->len + n; break; }
                nc *= 2;
            }
            unsigned char *np = (unsigned char *)realloc(b->p, nc);
            if (!np) return -1;
            b->p = np; b->cap = nc;
        }
        memcpy(b->p + b->len, d, n);
        b->len += n;
    }
    return 0;
}
static int gbuf_u8 (gbuf *b, unsigned char v) { return gbuf_put(b, &v, 1); }
static int gbuf_u32(gbuf *b, uint32_t v) {
    unsigned char t[4]; put_u32le(t, v); return gbuf_put(b, t, 4);
}
static int gbuf_u64(gbuf *b, uint64_t v) {
    unsigned char t[8]; put_u64le(t, v); return gbuf_put(b, t, 8);
}

/* --- small path helpers -------------------------------------------------- */

/* malloc "<a>/<b>", or a plain copy of b when a is empty/NULL. NULL on OOM. */
static char *path_join(const char *a, const char *b) {
    size_t la = a ? strlen(a) : 0, lb = strlen(b);
    char *r = (char *)malloc(la + 1 + lb + 1);
    if (!r) return NULL;
    if (la) { memcpy(r, a, la); r[la] = '/'; memcpy(r + la + 1, b, lb + 1); }
    else    { memcpy(r, b, lb + 1); }
    return r;
}

/* malloc'd copy of `path` with trailing '/' and '\\' trimmed (never to
 * empty). NULL on OOM. */
static char *strip_trailing_sep(const char *path) {
    size_t n = strlen(path);
    while (n > 1 && (path[n-1] == '/' || path[n-1] == '\\')) n--;
    char *r = (char *)malloc(n + 1);
    if (!r) return NULL;
    memcpy(r, path, n); r[n] = '\0';
    return r;
}

/* Filesystem probes. On Windows mode bits are synthesized (there are no unix
 * permissions); stat() on POSIX follows symlinks, so a link is archived as
 * whatever it points at and a dangling link is silently skipped. */
static int path_is_dir(const char *p) {
#if defined(_WIN32)
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat s;
    return stat(p, &s) == 0 && S_ISDIR(s.st_mode);
#endif
}
static int path_is_regular(const char *p) {
#if defined(_WIN32)
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    struct stat s;
    return stat(p, &s) == 0 && S_ISREG(s.st_mode);
#endif
}
static unsigned path_mode(const char *p, unsigned dflt) {
#if defined(_WIN32)
    (void)p; return dflt;
#else
    struct stat s;
    return stat(p, &s) == 0 ? (unsigned)(s.st_mode & 0777) : dflt;
#endif
}

/* Create one directory; success if it already exists. */
static int make_dir(const char *p) {
#if defined(_WIN32)
    if (_mkdir(p) == 0) return 0;
#else
    if (mkdir(p, 0777) == 0) return 0;
#endif
    return errno == EEXIST ? 0 : -1;
}

/* Create out_root and every directory along the relative path `rel`. When
 * include_last is 0, stop before rel's final component (create parents only,
 * for a file); when 1, create rel itself too (for a directory entry). */
static int make_dirs(const char *out_root, const char *rel, int include_last) {
    if (make_dir(out_root) != 0) return -1;
    size_t rl = strlen(out_root);
    char *w = (char *)malloc(rl + 1 + strlen(rel) + 1);
    if (!w) return -1;
    memcpy(w, out_root, rl); w[rl] = '\0';
    size_t wl = rl;
    int rc = 0;
    for (const char *c = rel; *c; ) {
        const char *slash = strchr(c, '/');
        size_t clen = slash ? (size_t)(slash - c) : strlen(c);
        if (!slash && !include_last) break;            /* final component */
        w[wl++] = '/';
        memcpy(w + wl, c, clen); wl += clen; w[wl] = '\0';
        if (make_dir(w) != 0) { rc = -1; break; }
        if (!slash) break;
        c = slash + 1;
    }
    free(w);
    return rc;
}

/* A stored path is safe iff it is relative and every component is non-empty,
 * not "." or "..", and free of ':' and '\\' — so an archive can never write
 * outside out_root (no absolute paths, drive letters, or traversal). */
static int arc_path_safe(const char *p, size_t n) {
    if (n == 0 || p[0] == '/') return 0;
    for (size_t i = 0; i < n; ) {
        size_t j = i;
        while (j < n && p[j] != '/') j++;
        size_t clen = j - i;
        if (clen == 0) return 0;                                  /* //, or trailing / */
        if (clen == 1 && p[i] == '.') return 0;
        if (clen == 2 && p[i] == '.' && p[i+1] == '.') return 0;
        for (size_t k = i; k < j; k++)
            if (p[k] == '\\' || p[k] == ':' || p[k] == '\0') return 0;
        i = (j < n) ? j + 1 : j;
    }
    return 1;
}

/* --- listing a directory (sorted, so archives are reproducible) ---------- */
static int name_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}
static void free_names(char **names, size_t n) {
    for (size_t i = 0; i < n; i++) free(names[i]);
    free(names);
}
static int add_name(char ***a, size_t *n, size_t *cap, const char *nm) {
    if (*n == *cap) {
        size_t nc = *cap ? *cap * 2 : 16;
        char **np = (char **)realloc(*a, nc * sizeof *np);
        if (!np) return -1;
        *a = np; *cap = nc;
    }
    size_t l = strlen(nm);
    char *copy = (char *)malloc(l + 1);
    if (!copy) return -1;
    memcpy(copy, nm, l + 1);
    (*a)[(*n)++] = copy;
    return 0;
}

/* Names in `dir` (excluding "." and ".."), sorted. Caller frees with
 * free_names. Returns 0 on success (including an empty directory), -1 on I/O
 * or allocation failure. */
static int list_dir(const char *dir, char ***out, size_t *out_n) {
    char **names = NULL; size_t n = 0, cap = 0;
    *out = NULL; *out_n = 0;
#if defined(_WIN32)
    char *pat = path_join(dir, "*");
    if (!pat) return -1;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    free(pat);
    if (h == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : -1;  /* empty dir */
    do {
        const char *nm = fd.cFileName;
        if (!strcmp(nm, ".") || !strcmp(nm, "..")) continue;
        if (add_name(&names, &n, &cap, nm) != 0) {
            FindClose(h); free_names(names, n); return -1;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *nm = e->d_name;
        if (!strcmp(nm, ".") || !strcmp(nm, "..")) continue;
        if (add_name(&names, &n, &cap, nm) != 0) {
            closedir(d); free_names(names, n); return -1;
        }
    }
    closedir(d);
#endif
    if (n > 1) qsort(names, n, sizeof *names, name_cmp);
    *out = names; *out_n = n;
    return 0;
}

/* Append the subtree rooted at fs_dir to the archive, giving each entry the
 * archive-relative path arc_pre/<name> ("" at the top level). Directories are
 * emitted before their contents so empty ones survive. Returns 0 on success. */
static int arc_add_dir(gbuf *a, const char *fs_dir, const char *arc_pre,
                       uint64_t *count, int quiet) {
    char **names; size_t n;
    if (list_dir(fs_dir, &names, &n) != 0) {
        fprintf(stderr, "squish: %s: cannot read directory\n", fs_dir);
        return -1;
    }
    int rc = 0;
    for (size_t i = 0; i < n && rc == 0; i++) {
        char *fs  = path_join(fs_dir, names[i]);
        char *arc = path_join(arc_pre, names[i]);      /* arc_pre="" -> name */
        if (!fs || !arc) { free(fs); free(arc); rc = -1; break; }
        size_t arclen = strlen(arc);
        if (arclen > SQAR_MAX_PATH) {
            fprintf(stderr, "squish: %s: path too long for archive\n", arc);
            free(fs); free(arc); rc = -1; break;
        }
        if (path_is_dir(fs)) {
            if (gbuf_u8(a, 1) || gbuf_u32(a, path_mode(fs, 0755)) ||
                gbuf_u32(a, (uint32_t)arclen) || gbuf_u64(a, 0) ||
                gbuf_put(a, arc, arclen))
                rc = -1;
            else { (*count)++; rc = arc_add_dir(a, fs, arc, count, quiet); }
        } else if (path_is_regular(fs)) {
            unsigned char *data; size_t dl;
            if (read_file_all(fopen(fs, "rb"), &data, &dl) != 0) {
                fprintf(stderr, "squish: %s: %s\n", fs,
                        squish_strerror(SQUISH_E_IO));
                rc = -1;
            } else {
                if (gbuf_u8(a, 0) || gbuf_u32(a, path_mode(fs, 0644)) ||
                    gbuf_u32(a, (uint32_t)arclen) || gbuf_u64(a, (uint64_t)dl) ||
                    gbuf_put(a, arc, arclen) || gbuf_put(a, data, dl))
                    rc = -1;
                else (*count)++;
                free(data);
            }
        } else if (!quiet) {
            fprintf(stderr, "squish: %s: skipping (not a regular file)\n", fs);
        }
        free(fs); free(arc);
    }
    free_names(names, n);
    return rc;
}

/* Serialize directory `dir` into a fresh SQAR buffer (caller frees *out with
 * free). *entries, if non-NULL, receives the entry count. */
static int build_archive(const char *dir, unsigned char **out, size_t *out_len,
                         uint64_t *entries, int quiet) {
    gbuf a = { NULL, 0, 0 };
    if (gbuf_put(&a, SQAR_MAGIC, 8) || gbuf_u32(&a, SQAR_VERSION) ||
        gbuf_u32(&a, 0) || gbuf_u64(&a, 0)) { free(a.p); return -1; }
    uint64_t count = 0;
    if (arc_add_dir(&a, dir, "", &count, quiet) != 0) { free(a.p); return -1; }
    put_u64le(a.p + 16, count);                        /* patch entry_count */
    *out = a.p; *out_len = a.len;
    if (entries) *entries = count;
    return 0;
}

/* True if `buf` is (at least on its face) a serialized archive. */
static int is_archive(const unsigned char *buf, size_t len) {
    return len >= SQAR_HDR_LEN && memcmp(buf, SQAR_MAGIC, 8) == 0 &&
           get_u32le(buf + 8) == SQAR_VERSION;
}

/* Write `n` bytes to `path`, applying unix `mode` where supported. 0 on ok. */
static int write_file_bytes(const char *path, const unsigned char *d, size_t n,
                            unsigned mode) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int ok = !(n && fwrite(d, 1, n, f) != n);
    if (fclose(f) != 0) ok = 0;
    if (!ok) { remove(path); return -1; }
#if !defined(_WIN32)
    if (mode) chmod(path, (mode_t)(mode & 0777));
#else
    (void)mode;
#endif
    return 0;
}

/* Unpack a validated SQAR buffer into directory out_root (created if needed).
 * Every length and path is bounds-checked and traversal-guarded; a malformed
 * archive returns SQUISH_E_FORMAT, an I/O failure SQUISH_E_IO. */
static int unpack_archive(const unsigned char *b, size_t len,
                          const char *out_root, uint64_t *nfiles,
                          uint64_t *nbytes) {
    if (!is_archive(b, len)) return SQUISH_E_FORMAT;
    uint64_t count = get_u64le(b + 16);
    size_t off = SQAR_HDR_LEN;
    uint64_t files = 0, bytes = 0;
    int rc = SQUISH_OK;
    if (make_dir(out_root) != 0) {
        fprintf(stderr, "squish: %s: cannot create directory\n", out_root);
        return SQUISH_E_IO;
    }
    for (uint64_t i = 0; i < count && rc == SQUISH_OK; i++) {
        if (len - off < SQAR_ENT_LEN) return SQUISH_E_FORMAT;
        unsigned char type = b[off];
        unsigned  mode = get_u32le(b + off + 1);
        uint32_t  plen = get_u32le(b + off + 5);
        uint64_t  dlen = get_u64le(b + off + 9);
        off += SQAR_ENT_LEN;
        if (plen == 0 || plen > SQAR_MAX_PATH || plen > len - off)
            return SQUISH_E_FORMAT;
        const char *path = (const char *)(b + off);
        if (!arc_path_safe(path, plen)) return SQUISH_E_FORMAT;
        char *rel = (char *)malloc((size_t)plen + 1);
        if (!rel) return SQUISH_E_NOMEM;
        memcpy(rel, path, plen); rel[plen] = '\0';
        off += plen;

        if (type == 1) {                               /* directory */
            if (dlen != 0) { free(rel); return SQUISH_E_FORMAT; }
            if (make_dirs(out_root, rel, 1) != 0) {
                fprintf(stderr, "squish: %s: cannot create directory\n", rel);
                rc = SQUISH_E_IO;
            }
        } else if (type == 0) {                        /* regular file */
            if (dlen > len - off) { free(rel); return SQUISH_E_FORMAT; }
            char *full = path_join(out_root, rel);
            if (!full) { free(rel); return SQUISH_E_NOMEM; }
            if (make_dirs(out_root, rel, 0) != 0 ||
                write_file_bytes(full, b + off, (size_t)dlen, mode) != 0) {
                fprintf(stderr, "squish: %s: %s\n", full,
                        squish_strerror(SQUISH_E_IO));
                rc = SQUISH_E_IO;
            }
            free(full);
            off += (size_t)dlen;
            files++; bytes += dlen;
        } else {
            rc = SQUISH_E_FORMAT;
        }
        free(rel);
    }
    if (rc == SQUISH_OK && off != len) return SQUISH_E_FORMAT;  /* must tile */
    if (nfiles) *nfiles = files;
    if (nbytes) *nbytes = bytes;
    return rc;
}

/* Build a self-extracting archive: stub || payload || name || trailer. */
static int sfx_create(const char *in_path, const char *out_path,
                      int threads, size_t block, int quiet,
                      squish_progress_fn cb, status *st) {
    unsigned char *stub; size_t stub_len;
    if (read_file_all(open_self(), &stub, &stub_len) != 0) {
        fprintf(stderr, "squish: cannot read own executable to use as SFX stub\n");
        return 1;
    }
    /* A directory input is serialized into an archive stream first (§ archive
     * above); a plain file is embedded as-is. Either way the payload is an
     * ordinary compressed stream the SFX stub already knows how to decode. */
    int is_dir = path_is_dir(in_path);
    char *in_disp = strip_trailing_sep(in_path);   /* tidy name + summary */
    if (!in_disp) { free(stub); fprintf(stderr, "squish: out of memory\n"); return 1; }
    unsigned char *in; size_t in_len;
    int rd = is_dir ? build_archive(in_disp, &in, &in_len, NULL, quiet)
                    : read_file_all(fopen(in_disp, "rb"), &in, &in_len);
    if (rd != 0) {
        free(stub); free(in_disp);
        fprintf(stderr, "squish: %s: %s\n", in_path, squish_strerror(SQUISH_E_IO));
        return 1;
    }

    double t0 = now_sec();
    void *payload; size_t payload_len;
    int rc = squish_compress_alloc_mt(in, in_len, &payload, &payload_len,
                                      threads, block, cb, st);
    double dt = now_sec() - t0;
    clear_status(st);
    free(in);
    if (rc != SQUISH_OK) {
        free(stub); free(in_disp);
        fprintf(stderr, "squish: %s: %s\n", in_path, squish_strerror(rc));
        return 1;
    }

    const char *name = sfx_basename(in_disp);
    size_t name_len = strlen(name);
    if (name_len > SFX_MAX_NAME) name_len = SFX_MAX_NAME;

    unsigned char tr[SFX_TRAILER_LEN];
    memcpy(tr, SFX_MAGIC, SFX_MAGIC_LEN);
    put_u64le(tr + 8,  (uint64_t)stub_len);
    put_u64le(tr + 16, (uint64_t)payload_len);
    put_u32le(tr + 24, (uint32_t)name_len);
    put_u32le(tr + 28, 0);

    FILE *o = fopen(out_path, "wb");
    int ok = o != NULL;
    if (ok && stub_len    && fwrite(stub, 1, stub_len, o) != stub_len)          ok = 0;
    if (ok && payload_len && fwrite(payload, 1, payload_len, o) != payload_len) ok = 0;
    if (ok && name_len    && fwrite(name, 1, name_len, o) != name_len)          ok = 0;
    if (ok && fwrite(tr, 1, SFX_TRAILER_LEN, o) != SFX_TRAILER_LEN)             ok = 0;
    if (o && fclose(o) != 0) ok = 0;
    free(stub);
    free(in_disp);
    squish_free(payload);
    if (!ok) {
        fprintf(stderr, "squish: %s: %s\n", out_path, squish_strerror(SQUISH_E_IO));
        if (o) remove(out_path);
        return 1;
    }

#if !defined(_WIN32)
    chmod(out_path, 0755);           /* make the archive runnable */
#endif

    if (!quiet) {
        long long outsz = file_size(out_path);
        fprintf(stderr,
            "%s%s -> %s: %lld -> %lld bytes self-extracting"
            " (payload %llu, %.3f bpb, %.2f MB/s)\n",
            in_path, is_dir ? "/" : "", out_path, (long long)in_len, outsz,
            (unsigned long long)payload_len,
            in_len ? 8.0 * (double)payload_len / (double)in_len : 0.0,
            dt > 0 && in_len ? (double)in_len / 1e6 / dt : 0.0);
    }
    return 0;
}

static int sfx_usage(void) {
    fprintf(stderr,
        "self-extracting SQUISH archive\n"
        "usage: %s [-f] [-q] [-t N] [output]\n"
        "  Extracts the embedded file (or directory tree) to the current\n"
        "  directory under its original name, or to <output> if given.\n"
        "  -f, --force      overwrite an existing output file\n"
        "  -q, --quiet      errors only\n"
        "  -t, --threads N  worker threads, 0 = all cores (default)\n",
        g_argv0 && *g_argv0 ? g_argv0 : "archive");
    return 2;
}

/* Run when the executable is itself a self-extracting archive. */
static int sfx_run(int argc, char **argv, const sfx_info *info) {
    int quiet = 0, force = 0, threads = 0;   /* extract: all cores by default */
    const char *target = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-q") || !strcmp(argv[i], "--quiet")) quiet = 1;
        else if (!strcmp(argv[i], "-f") || !strcmp(argv[i], "--force")) force = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            sfx_usage(); return 0;
        }
        else if (!strcmp(argv[i], "-t") || !strcmp(argv[i], "--threads")) {
            char *end;
            if (++i >= argc) return sfx_usage();
            long v = strtol(argv[i], &end, 10);
            if (*end || v < 0 || v > 4096) return sfx_usage();
            threads = (int)v;
        }
        else if (argv[i][0] == '-' && argv[i][1]) return sfx_usage();
        else if (!target) target = argv[i];
        else return sfx_usage();
    }

    /* Stored original name -> a safe basename we may extract to. */
    unsigned char *namebuf;
    uint64_t name_off = (uint64_t)info->filesize - SFX_TRAILER_LEN - info->name_len;
    if (sfx_read_range(name_off, info->name_len, &namebuf) != 0) {
        fprintf(stderr, "squish: cannot read embedded name\n");
        return 1;
    }
    char stored[SFX_MAX_NAME + 1];
    size_t nl = info->name_len;              /* <= SFX_MAX_NAME (probe checked) */
    memcpy(stored, namebuf, nl);
    stored[nl] = '\0';
    free(namebuf);
    const char *out = target ? target : sfx_basename(stored);

    /* Guard single-file overwrite (an archive payload merges into the target
     * directory instead, so only a colliding *file* is a conflict here). */
    if (!force && path_is_regular(out)) {
        fprintf(stderr, "squish: %s already exists (use -f to overwrite)\n", out);
        return 1;
    }

    unsigned char *payload;
    if (sfx_read_range(info->payload_off, info->payload_len, &payload) != 0) {
        fprintf(stderr, "squish: cannot read embedded payload\n");
        return 1;
    }

    status st = { "extracting", now_sec(), -1 };
    squish_progress_fn cb = (!quiet && SQ_ISATTY(stderr)) ? draw_status : NULL;
    double t0 = now_sec();
    void *outbuf; size_t outn;
    int rc = squish_decompress_alloc_mt(payload, (size_t)info->payload_len,
                                        &outbuf, &outn, threads, cb, &st);
    double dt = now_sec() - t0;
    clear_status(&st);
    free(payload);
    if (rc != SQUISH_OK) {
        fprintf(stderr, "squish: %s\n", squish_strerror(rc));
        return 1;
    }

    /* A directory archive unpacks into `out`; a plain payload is one file. */
    if (is_archive(outbuf, outn)) {
        uint64_t nf = 0, nb = 0;
        int urc = unpack_archive(outbuf, outn, out, &nf, &nb);
        squish_free(outbuf);
        if (urc != SQUISH_OK) {
            fprintf(stderr, "squish: %s: %s\n", out, squish_strerror(urc));
            return 1;
        }
        if (!quiet)
            fprintf(stderr, "extracted %s/: %llu files, %llu bytes (%.2f MB/s)\n",
                    out, (unsigned long long)nf, (unsigned long long)nb,
                    dt > 0 && nb ? (double)nb / 1e6 / dt : 0.0);
        return 0;
    }

    FILE *o = fopen(out, "wb");
    int ok = o != NULL;
    if (ok && outn && fwrite(outbuf, 1, outn, o) != outn) ok = 0;
    if (o && fclose(o) != 0) ok = 0;
    squish_free(outbuf);
    if (!ok) {
        fprintf(stderr, "squish: %s: %s\n", out, squish_strerror(SQUISH_E_IO));
        if (o) remove(out);
        return 1;
    }

    if (!quiet)
        fprintf(stderr, "extracted %s: %llu bytes (%.2f MB/s)\n",
                out, (unsigned long long)outn,
                dt > 0 && outn ? (double)outn / 1e6 / dt : 0.0);
    return 0;
}

/* ============================ top-level commands ========================= */

/* Compress directory `dir` into `out` as a single archive stream. */
static int compress_dir_cmd(const char *dir, const char *out, int threads,
                            size_t block, int quiet, squish_progress_fn cb,
                            status *st) {
    char *d = strip_trailing_sep(dir);
    if (!d) { fprintf(stderr, "squish: out of memory\n"); return 1; }
    unsigned char *blob; size_t blob_len; uint64_t entries = 0;
    if (build_archive(d, &blob, &blob_len, &entries, quiet) != 0) {
        fprintf(stderr, "squish: %s: failed to read directory tree\n", d);
        free(d); return 1;
    }
    double t0 = now_sec();
    void *comp; size_t comp_len;
    int rc = squish_compress_alloc_mt(blob, blob_len, &comp, &comp_len,
                                      threads, block, cb, st);
    double dt = now_sec() - t0;
    clear_status(st);
    free(blob);
    if (rc != SQUISH_OK) {
        fprintf(stderr, "squish: %s: %s\n", d, squish_strerror(rc));
        free(d); return 1;
    }
    int wr = write_file_bytes(out, (const unsigned char *)comp, comp_len, 0);
    squish_free(comp);
    if (wr != 0) {
        fprintf(stderr, "squish: %s: %s\n", out, squish_strerror(SQUISH_E_IO));
        free(d); return 1;
    }
    if (!quiet) {
        long long outsz = file_size(out);
        fprintf(stderr,
            "%s/ -> %s: %llu entries, %llu -> %lld bytes (%.3f bpb, %.2f MB/s)\n",
            d, out, (unsigned long long)entries,
            (unsigned long long)blob_len, outsz,
            blob_len ? 8.0 * (double)outsz / (double)blob_len : 0.0,
            dt > 0 && blob_len ? (double)blob_len / 1e6 / dt : 0.0);
    }
    free(d);
    return 0;
}

/* Decompress `in` into `out`: a single file, or — when the stream carries an
 * archive — a directory tree recreated under `out`. */
static int decompress_cmd(const char *in, const char *out, int threads,
                          int quiet, squish_progress_fn cb, status *st) {
    unsigned char *comp; size_t comp_len;
    if (read_file_all(fopen(in, "rb"), &comp, &comp_len) != 0) {
        fprintf(stderr, "squish: %s: %s\n", in, squish_strerror(SQUISH_E_IO));
        return 1;
    }
    double t0 = now_sec();
    void *buf; size_t n;
    int rc = squish_decompress_alloc_mt(comp, comp_len, &buf, &n,
                                        threads, cb, st);
    double dt = now_sec() - t0;
    clear_status(st);
    free(comp);
    if (rc != SQUISH_OK) {
        fprintf(stderr, "squish: %s: %s\n", in, squish_strerror(rc));
        return 1;
    }
    if (is_archive((unsigned char *)buf, n)) {
        uint64_t nf = 0, nb = 0;
        int urc = unpack_archive((unsigned char *)buf, n, out, &nf, &nb);
        squish_free(buf);
        if (urc != SQUISH_OK) {
            fprintf(stderr, "squish: %s: %s\n", out, squish_strerror(urc));
            return 1;
        }
        if (!quiet)
            fprintf(stderr, "%s -> %s/: %llu files, %llu bytes (%.2f MB/s)\n",
                in, out, (unsigned long long)nf, (unsigned long long)nb,
                dt > 0 && nb ? (double)nb / 1e6 / dt : 0.0);
        return 0;
    }
    int wr = write_file_bytes(out, (const unsigned char *)buf, n, 0);
    squish_free(buf);
    if (wr != 0) {
        fprintf(stderr, "squish: %s: %s\n", out, squish_strerror(SQUISH_E_IO));
        return 1;
    }
    if (!quiet) {
        long long insz = file_size(in);
        fprintf(stderr, "%s -> %s: %lld -> %llu bytes (%.3f bpb, %.2f MB/s)\n",
            in, out, insz, (unsigned long long)n,
            n ? 8.0 * (double)insz / (double)n : 0.0,
            dt > 0 && n ? (double)n / 1e6 / dt : 0.0);
    }
    return 0;
}

int main(int argc, char **argv) {
    g_argv0 = argv[0];
    { sfx_info sfxi; if (sfx_probe(&sfxi)) return sfx_run(argc, argv, &sfxi); }

    int quiet = 0, threads = -1;    /* -1 = unset: per-direction default */
    size_t block = 0;               /* 0 = library default */
    const char *pos[3] = {0};
    int npos = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-q") || !strcmp(argv[i], "--quiet")) quiet = 1;
        else if (!strcmp(argv[i], "-t") || !strcmp(argv[i], "--threads")) {
            char *end;
            if (++i >= argc) return usage();
            long v = strtol(argv[i], &end, 10);
            if (*end || v < 0 || v > 4096) return usage();
            threads = (int)v;
        }
        else if (!strcmp(argv[i], "-b") || !strcmp(argv[i], "--block")) {
            char *end;
            if (++i >= argc) return usage();
            long v = strtol(argv[i], &end, 10);
            if (*end || v < 1 || v > 4096) return usage();
            if ((unsigned long)v > ((size_t)-1 >> 20)) return usage();
            block = (size_t)v << 20;    /* MiB -> bytes, shift can't wrap */
        }
        else if (argv[i][0] == '-' && argv[i][1]) return usage();
        else if (npos < 3) pos[npos++] = argv[i];
        else return usage();
    }
    if (npos != 3 ||
        (pos[0][0] != 'c' && pos[0][0] != 'd' && pos[0][0] != 's') || pos[0][1])
        return usage();
    int compress = (pos[0][0] == 'c');
    int sfx      = (pos[0][0] == 's');
    if (threads < 0) threads = (compress || sfx) ? 1 : 0;    /* 0 = all cores */

    status st = { compress || sfx ? "compressing" : "decompressing",
                  now_sec(), -1 };
    squish_progress_fn cb =
        (!quiet && SQ_ISATTY(stderr)) ? draw_status : NULL;

    if (sfx) return sfx_create(pos[1], pos[2], threads, block, quiet, cb, &st);

    if (!compress)     /* decompress auto-detects a file vs. an archive stream */
        return decompress_cmd(pos[1], pos[2], threads, quiet, cb, &st);

    /* A directory input is packed into an archive stream; a file is unchanged
     * (a plain SQ02/SQ01 stream, byte-for-byte as before). */
    if (path_is_dir(pos[1]))
        return compress_dir_cmd(pos[1], pos[2], threads, block, quiet, cb, &st);

    double t0 = now_sec();
    int rc = (threads == 1 && !block)
        ? squish_compress_file2(pos[1], pos[2], cb, &st)
        : squish_compress_file_mt(pos[1], pos[2], threads, block, cb, &st);
    double dt = now_sec() - t0;
    clear_status(&st);
    if (rc != SQUISH_OK) {
        fprintf(stderr, "squish: %s: %s\n", pos[1], squish_strerror(rc));
        return 1;
    }
    if (quiet) return 0;
    long long in = file_size(pos[1]), out = file_size(pos[2]);
    fprintf(stderr, "%s -> %s: %lld -> %lld bytes (%.3f bpb, %.2f MB/s)\n",
        pos[1], pos[2], in, out,
        in > 0 ? 8.0 * (double)out / (double)in : 0.0,
        dt > 0 && in > 0 ? (double)in / 1e6 / dt : 0.0);
    return 0;
}
