/*
 * ext-rdump: in-process memory dump in reli's RDUMP format.
 *
 * reli (reliforp/reli-prof) normally snapshots a PHP process from the
 * outside via process_vm_readv / ptrace. That is convenient (no prior
 * setup on the target) but needs ptrace capability granted to the PHP
 * runtime, which is awkward to keep resident in production.
 *
 * This extension produces the very same dump file from *inside* the
 * process: it resolves executor_globals / compiler_globals / basic_globals
 * by their real addresses, reads /proc/self/maps for the VMA list, and
 * copies the relevant regions directly out of its own address space with
 * a plain memcpy -- no ptrace, no process_vm_readv. The resulting file is
 * fed to `reli inspector:memory:analyze` / `:dump:inspect` offline.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "php.h"
#include "ext/standard/info.h"
#include "Zend/zend_globals.h"
#include "Zend/zend_globals_macros.h"
#include "ext/standard/basic_functions.h"
#include "php_rdump.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <dirent.h>

/* The dump records native pointers/sizes as 64-bit and the file is little-
 * endian; the resolver and /proc parsing assume Linux. Refuse to build on a
 * target that would silently produce a malformed or wrong-width dump. */
#ifndef __linux__
# error "ext-rdump is Linux-only (uses /proc/self/maps and /proc/self/mem)"
#endif
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ != 8
# error "ext-rdump targets 64-bit builds (pointer width must be 8 bytes)"
#endif
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) \
    && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
# error "ext-rdump targets little-endian builds"
#endif

ZEND_DECLARE_MODULE_GLOBALS(rdump)

/* ------------------------------------------------------------------ */
/* Little-endian writers (match PHP pack() 'V'/'P'/'q' on LE targets). */
/* ------------------------------------------------------------------ */

static int rdump_wr(FILE *fp, const void *p, size_t n)
{
    return fwrite(p, 1, n, fp) == n ? 0 : -1;
}

static int rdump_wr_u32(FILE *fp, uint32_t v)
{
    unsigned char b[4];
    b[0] = (unsigned char)(v & 0xff);
    b[1] = (unsigned char)((v >> 8) & 0xff);
    b[2] = (unsigned char)((v >> 16) & 0xff);
    b[3] = (unsigned char)((v >> 24) & 0xff);
    return rdump_wr(fp, b, 4);
}

static int rdump_wr_u64(FILE *fp, uint64_t v)
{
    unsigned char b[8];
    int i;
    for (i = 0; i < 8; i++) {
        b[i] = (unsigned char)(v & 0xff);
        v >>= 8;
    }
    return rdump_wr(fp, b, 8);
}

static int rdump_wr_str(FILE *fp, const char *s, size_t len)
{
    if (rdump_wr_u32(fp, (uint32_t)len) != 0) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    return rdump_wr(fp, s, len);
}

/* ------------------------------------------------------------------ */
/* /proc/self/maps parsing.                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t start;
    uint64_t end;
    uint64_t offset;
    uint64_t inode;
    char begin_hex[20];   /* original hex token from maps */
    char end_hex[20];
    char offset_hex[20];
    char dev[32];
    char perms[8];        /* "rw-p" */
    const char *name;     /* points into maps_buf, NUL-terminated */
    int capture;
} rdump_map_entry;

/* Kernel-special pseudo-mappings that we never copy region bytes for:
 * some are unreadable (vsyscall) and none carry PHP state. They are still
 * listed in the memory map so the file stays a faithful maps snapshot. */
static int rdump_is_kernel_special(const char *name)
{
    return strcmp(name, "[vvar]") == 0
        || strcmp(name, "[vdso]") == 0
        || strcmp(name, "[vsyscall]") == 0
        || strcmp(name, "[vvar_vclock]") == 0;
}

/* Read the whole of /proc/self/maps into a malloc'd, NUL-terminated buffer.
 * Uses raw read() (not emalloc) so the act of snapshotting perturbs the
 * Zend memory manager heap -- the very thing we are about to dump -- as
 * little as possible. Caller free()s. Returns NULL on failure. */
static char *rdump_read_maps(void)
{
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return NULL;
    }

    size_t cap = 64 * 1024;
    size_t len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        close(fd);
        return NULL;
    }

    for (;;) {
        if (len + 4096 + 1 > cap) {
            size_t new_cap = cap * 2;
            char *nb = (char *)realloc(buf, new_cap);
            if (!nb) {
                free(buf);
                close(fd);
                return NULL;
            }
            buf = nb;
            cap = new_cap;
        }
        ssize_t r = read(fd, buf + len, 4096);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(buf);
            close(fd);
            return NULL;
        }
        if (r == 0) {
            break;
        }
        len += (size_t)r;
    }
    close(fd);
    buf[len] = '\0';
    return buf;
}

static void rdump_copy_token(char *dst, size_t dst_sz, const char *src, size_t n)
{
    if (n >= dst_sz) {
        n = dst_sz - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Parse maps_buf into a malloc'd array of entries. On return *out_entries
 * holds the array (caller free()s) and the function returns the count, or
 * -1 on allocation failure. maps_buf is mutated in place (newlines -> NUL)
 * and must outlive the entries (entry->name points into it). */
static long rdump_parse_maps(char *maps_buf, int full, rdump_map_entry **out_entries)
{
    size_t cap = 256;
    long count = 0;
    rdump_map_entry *entries = (rdump_map_entry *)malloc(cap * sizeof(*entries));
    if (!entries) {
        return -1;
    }

    char *line = maps_buf;
    while (*line) {
        char *nl = strchr(line, '\n');
        if (nl) {
            *nl = '\0';
        }

        unsigned long long start = 0, end = 0, off = 0, inode = 0;
        char perms[8] = {0};
        char dev[32] = {0};
        int consumed = 0;
        int matched = sscanf(line, "%llx-%llx %7s %llx %31s %llu%n",
                             &start, &end, perms, &off, dev, &inode, &consumed);
        if (matched < 6) {
            if (!nl) {
                break;
            }
            line = nl + 1;
            continue;
        }

        const char *name = line + consumed;
        while (*name == ' ' || *name == '\t') {
            name++;
        }

        if (count >= (long)cap) {
            size_t new_cap = cap * 2;
            rdump_map_entry *ne =
                (rdump_map_entry *)realloc(entries, new_cap * sizeof(*entries));
            if (!ne) {
                free(entries);
                return -1;
            }
            entries = ne;
            cap = new_cap;
        }

        rdump_map_entry *e = &entries[count];
        e->start = (uint64_t)start;
        e->end = (uint64_t)end;
        e->offset = (uint64_t)off;
        e->inode = (uint64_t)inode;
        /* Re-render begin/end/offset as bare lowercase hex, mirroring how
         * /proc/<pid>/maps presents them and what reli's parser expects. */
        snprintf(e->begin_hex, sizeof(e->begin_hex), "%llx", start);
        snprintf(e->end_hex, sizeof(e->end_hex), "%llx", end);
        snprintf(e->offset_hex, sizeof(e->offset_hex), "%llx", off);
        rdump_copy_token(e->dev, sizeof(e->dev), dev, strlen(dev));
        rdump_copy_token(e->perms, sizeof(e->perms), perms, strlen(perms));
        e->name = name;

        int readable = perms[0] == 'r';
        int writable = perms[1] == 'w';
        int is_anon = (name[0] == '\0');
        int is_shm = strstr(name, "/dev/zero") != NULL;
        int is_special = rdump_is_kernel_special(name);

        e->capture = 0;
        if (readable && !is_special && end > start) {
            /* Always capture volatile memory: anonymous (ZendMM chunks,
             * huge allocations, VM stacks, object buckets), the brk/[heap]
             * and [stack] and writable file-backed segments (lib/php
             * .data/.bss, where NTS EG/CG/BG live), plus opcache shared
             * memory (interned strings / cached scripts). With $full also
             * embed read-only file-backed segments so the dump is fully
             * self-contained instead of relying on the on-disk binaries. */
            if (writable || is_anon || is_shm || full) {
                e->capture = 1;
            }
        }

        count++;
        if (!nl) {
            break;
        }
        line = nl + 1;
    }

    *out_entries = entries;
    return count;
}

/* ------------------------------------------------------------------ */
/* Globals address resolution (NTS and ZTS).                          */
/* ------------------------------------------------------------------ */

/* Base address of the per-(thread-)globals structs. Works for both NTS
 * (EG/CG/BG resolve to a static symbol member) and ZTS (TSRM per-thread
 * storage member): take the address of a stable member and subtract its
 * offset within the struct. symbol_table / function_table /
 * user_shutdown_function_names have existed since PHP 7.0. */
static uint64_t rdump_eg_address(void)
{
    return (uint64_t)(uintptr_t)((char *)&EG(symbol_table)
        - XtOffsetOf(zend_executor_globals, symbol_table));
}

static uint64_t rdump_cg_address(void)
{
    return (uint64_t)(uintptr_t)((char *)&CG(function_table)
        - XtOffsetOf(zend_compiler_globals, function_table));
}

static uint64_t rdump_bg_address(void)
{
    return (uint64_t)(uintptr_t)((char *)&BG(user_shutdown_function_names)
        - XtOffsetOf(php_basic_globals, user_shutdown_function_names));
}

/* Resident set size in bytes from /proc/self/statm, or -1 if unreadable. */
static int64_t rdump_rss_bytes(void)
{
    FILE *fp = fopen("/proc/self/statm", "r");
    if (!fp) {
        return -1;
    }
    unsigned long long total = 0, resident = 0;
    int n = fscanf(fp, "%llu %llu", &total, &resident);
    fclose(fp);
    if (n < 2) {
        return -1;
    }
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) {
        page = 4096;
    }
    return (int64_t)(resident * (unsigned long long)page);
}

/* Parse a byte quantity like "500", "16K", "256M", "2G" (binary multiples,
 * case-insensitive suffix) into bytes. Lenient: a leading integer with an
 * optional suffix, any trailing characters ignored (so "10foo" is 10); a value
 * that doesn't start with a non-negative number yields 0. An absurdly large
 * value (suffix overflow, or beyond zend_long on a 32-bit build) is clamped to
 * ZEND_LONG_MAX rather than wrapping to a bogus/negative size. */
static zend_long rdump_parse_bytes(const char *s)
{
    if (s == NULL) {
        return 0;
    }
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    char *end = NULL;
    long long v = strtoll(s, &end, 10);
    if (end == s || v < 0) {
        return 0;
    }
    while (*end == ' ' || *end == '\t') {
        end++;
    }
    long long mult = 1;
    switch (*end) {
        case 'k': case 'K': mult = 1024LL; break;
        case 'm': case 'M': mult = 1024LL * 1024; break;
        case 'g': case 'G': mult = 1024LL * 1024 * 1024; break;
        case 't': case 'T': mult = 1024LL * 1024 * 1024 * 1024; break;
        default: break;
    }
    if (v > LLONG_MAX / mult) {
        return ZEND_LONG_MAX;
    }
    v *= mult;
    return v > (long long)ZEND_LONG_MAX ? ZEND_LONG_MAX : (zend_long)v;
}

/* Sum the sizes of every *.rdump file in dir, skipping except_base (the file
 * an overwrite would replace, so it must not count toward the new total).
 * Best-effort: unreadable entries are ignored. Used to enforce a disk budget
 * shared across all workers writing OOM dumps into the same directory. */
static uint64_t rdump_dir_rdump_total(const char *dir, const char *except_base)
{
    DIR *d = opendir(dir);
    if (!d) {
        return 0;
    }
    uint64_t total = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *n = ent->d_name;
        size_t ln = strlen(n);
        if (ln < 6 || strcmp(n + ln - 6, ".rdump") != 0) {
            continue;
        }
        if (except_base != NULL && strcmp(n, except_base) == 0) {
            continue;
        }
        char full[4096];
        if ((size_t)snprintf(full, sizeof(full), "%s/%s", dir, n) >= sizeof(full)) {
            continue;
        }
        /* lstat, not stat: a symlink named *.rdump in a writable dump dir must
         * not let its target's size (or a device) skew the budget. */
        struct stat st;
        if (lstat(full, &st) == 0 && S_ISREG(st.st_mode)) {
            total += (uint64_t)st.st_size;
        }
    }
    closedir(d);
    return total;
}

/* ------------------------------------------------------------------ */
/* Dump writer.                                                       */
/* ------------------------------------------------------------------ */

static const char rdump_magic[8] = {'R', 'D', 'U', 'M', 'P', '\0', '\0', '\0'};

/* Copy [start, start+size) into fp by reading through /proc/self/mem with
 * pread instead of dereferencing the address directly. If another thread
 * unmaps or mprotects the region mid-dump (the real hazard under heavy ZTS
 * load), pread returns EFAULT/short rather than crashing the process; the
 * unreadable bytes are zero-filled so the region keeps its declared length. */
static int rdump_write_region_safe(FILE *fp, int mem_fd, uint64_t start,
                                   uint64_t size, char *buf, size_t bufsz)
{
    long pg = sysconf(_SC_PAGESIZE);
    size_t page = (pg > 0) ? (size_t)pg : 4096;
    uint64_t pos = 0;
    while (pos < size) {
        size_t want = (size - pos) > (uint64_t)bufsz ? bufsz : (size_t)(size - pos);
        size_t filled = 0;
        while (filled < want) {
            ssize_t got = pread(mem_fd, buf + filled, want - filled,
                                (off_t)(start + pos + filled));
            if (got > 0) {
                filled += (size_t)got;
                continue;
            }
            if (got < 0 && errno == EINTR) {
                continue;   /* transient interruption: retry the same offset */
            }
            /* EFAULT (a concurrently unmapped page) or EOF: zero just one page
             * and resume past it, so a single hole doesn't blank the rest of
             * the block when later pages are still readable. */
            {
                size_t step = (want - filled) < page ? (want - filled) : page;
                memset(buf + filled, 0, step);
                filled += step;
            }
        }
        if (rdump_wr(fp, buf, want) != 0) {
            return -1;
        }
        pos += want;
    }
    return 0;
}

static int rdump_write_file(const char *path, int full, char **err)
{
    char php_version[8];
    snprintf(php_version, sizeof(php_version), "v%d%d",
             PHP_MAJOR_VERSION, PHP_MINOR_VERSION);

    char *maps_buf = rdump_read_maps();
    if (!maps_buf) {
        *err = "failed to read /proc/self/maps";
        return -1;
    }

    rdump_map_entry *entries = NULL;
    long entry_count = rdump_parse_maps(maps_buf, full, &entries);
    if (entry_count < 0) {
        free(maps_buf);
        *err = "out of memory while parsing memory map";
        return -1;
    }

    uint32_t region_count = 0;
    long i;
    for (i = 0; i < entry_count; i++) {
        if (entries[i].capture) {
            region_count++;
        }
    }

    uint64_t eg_address = rdump_eg_address();
    uint64_t cg_address = rdump_cg_address();
    uint64_t bg_address = rdump_bg_address();
    int64_t rss = rdump_rss_bytes();

    /* A dump is a verbatim copy of process memory -- secrets, keys, request
     * bodies and all -- so it must never be born group/world-readable.
     * O_CREAT's 0600 only applies when we create the file, so fchmod() also
     * tightens a pre-existing one. O_NOFOLLOW refuses to follow a symlink
     * planted at the path; O_CLOEXEC keeps the fd out of any child. */
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    if (fd < 0) {
        free(entries);
        free(maps_buf);
        *err = "failed to open output file for writing";
        return -1;
    }
    /* The target must be a regular file: O_NOFOLLOW blocks a symlink,
     * O_NONBLOCK keeps a FIFO from blocking the open (which would hang the OOM
     * death path), and S_ISREG rejects FIFOs/devices/sockets outright. A new
     * file is already 0600 (open's mode); only tighten a pre-existing one, and
     * abort if we can't, so secret memory never lands in a wider-readable file. */
    {
        struct stat st;
        if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
            close(fd);
            free(entries);
            free(maps_buf);
            *err = "output path is not a regular file";
            return -1;
        }
        if ((st.st_mode & 07777) != 0600 && fchmod(fd, 0600) != 0) {
            close(fd);
            free(entries);
            free(maps_buf);
            *err = "failed to secure output file to 0600";
            return -1;
        }
    }

    FILE *fp = fdopen(fd, "wb");
    if (!fp) {
        close(fd);
        free(entries);
        free(maps_buf);
        *err = "failed to open output file for writing";
        return -1;
    }

    int rc = 0;
    /* Crash-safe region reads via /proc/self/mem (opt-in via rdump.safe_read,
     * off by default). Declared before the first RDUMP_TRY so they're valid at
     * the done: label even on an early jump. */
    int mem_fd = -1;
    char *region_buf = NULL;
    size_t region_bufsz = 0;
    if (RDUMP_G(safe_read)) {
        mem_fd = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
        if (mem_fd >= 0) {
            /* Try progressively smaller buffers so a tight heap (e.g. mid-OOM)
             * can still get a working safe-read path. */
            static const size_t try_sizes[] = { 1024 * 1024, 256 * 1024, 64 * 1024, 4096 };
            size_t k;
            for (k = 0; k < sizeof(try_sizes) / sizeof(try_sizes[0]); k++) {
                region_buf = (char *)malloc(try_sizes[k]);
                if (region_buf != NULL) {
                    region_bufsz = try_sizes[k];
                    break;
                }
            }
        }
        if (mem_fd < 0 || region_buf == NULL) {
            /* safe_read was asked for but can't be honored. Don't silently fall
             * back to the crash-prone direct copy -- fail loudly instead. */
            if (mem_fd >= 0) {
                close(mem_fd);
            }
            free(region_buf);
            fclose(fp);
            free(entries);
            free(maps_buf);
            *err = "rdump.safe_read is on but /proc/self/mem could not be set up";
            return -1;
        }
    }
#define RDUMP_TRY(expr) do { if ((expr) != 0) { rc = -1; goto done; } } while (0)

    /* Header */
    RDUMP_TRY(rdump_wr(fp, rdump_magic, sizeof(rdump_magic)));
    RDUMP_TRY(rdump_wr_u32(fp, RDUMP_FORMAT_VERSION));
    RDUMP_TRY(rdump_wr_str(fp, php_version, strlen(php_version)));
    RDUMP_TRY(rdump_wr_u64(fp, (uint64_t)getpid()));
    RDUMP_TRY(rdump_wr_u64(fp, eg_address));
    RDUMP_TRY(rdump_wr_u64(fp, cg_address));
    RDUMP_TRY(rdump_wr_u64(fp, (uint64_t)rss)); /* 'q' (-1 == unavailable) */

    /* v3 module-globals map: basic_globals lets reli walk the
     * user-shutdown-function table offline. */
    RDUMP_TRY(rdump_wr_u32(fp, 1));
    RDUMP_TRY(rdump_wr_str(fp, "basic_globals", strlen("basic_globals")));
    RDUMP_TRY(rdump_wr_u64(fp, bg_address));

    RDUMP_TRY(rdump_wr_u32(fp, (uint32_t)entry_count));
    RDUMP_TRY(rdump_wr_u32(fp, region_count));

    /* Memory map: every VMA, captured or not (read-only file-backed
     * segments are recovered from the on-disk binaries during analysis). */
    for (i = 0; i < entry_count; i++) {
        rdump_map_entry *e = &entries[i];
        unsigned char attrs[4];
        attrs[0] = (unsigned char)(e->perms[0] == 'r');
        attrs[1] = (unsigned char)(e->perms[1] == 'w');
        attrs[2] = (unsigned char)(e->perms[2] == 'x');
        attrs[3] = (unsigned char)(e->perms[3] == 'p');
        RDUMP_TRY(rdump_wr_str(fp, e->begin_hex, strlen(e->begin_hex)));
        RDUMP_TRY(rdump_wr_str(fp, e->end_hex, strlen(e->end_hex)));
        RDUMP_TRY(rdump_wr_str(fp, e->offset_hex, strlen(e->offset_hex)));
        RDUMP_TRY(rdump_wr(fp, attrs, 4));
        RDUMP_TRY(rdump_wr_str(fp, e->dev, strlen(e->dev)));
        RDUMP_TRY(rdump_wr_u64(fp, e->inode));
        RDUMP_TRY(rdump_wr_str(fp, e->name, strlen(e->name)));
    }

    /* Regions: address, size, then the raw bytes copied straight out of
     * our own address space. */
    for (i = 0; i < entry_count; i++) {
        rdump_map_entry *e = &entries[i];
        if (!e->capture) {
            continue;
        }
        uint64_t size = e->end - e->start;
        RDUMP_TRY(rdump_wr_u64(fp, e->start));
        RDUMP_TRY(rdump_wr_u64(fp, size));
        if (mem_fd >= 0) {
            RDUMP_TRY(rdump_write_region_safe(fp, mem_fd, e->start, size,
                                              region_buf, region_bufsz));
        } else {
            RDUMP_TRY(rdump_wr(fp, (const void *)(uintptr_t)e->start, (size_t)size));
        }
    }

#undef RDUMP_TRY
done:
    if (mem_fd >= 0) {
        close(mem_fd);
    }
    free(region_buf);
    /* A deferred write error (NFS, quota, delayed writeback) can surface only
     * at the final flush, so fold a fclose() failure into rc; otherwise a
     * truncated dump looks successful and (on the OOM path) gets a .done. */
    if (fclose(fp) != 0 && rc == 0) {
        rc = -1;
    }
    if (rc != 0) {
        *err = "failed while writing dump file (disk full?)";
    }
    free(entries);
    free(maps_buf);
    return rc;
}

/* ------------------------------------------------------------------ */
/* PHP function: bool rdump_dump(string $path, bool $full = false)    */
/* ------------------------------------------------------------------ */

ZEND_BEGIN_ARG_INFO_EX(arginfo_rdump_dump, 0, 0, 1)
    ZEND_ARG_INFO(0, path)
    ZEND_ARG_INFO(0, full)
ZEND_END_ARG_INFO()

PHP_FUNCTION(rdump_dump)
{
    char *path;
    size_t path_len;
    zend_bool full = 0;

    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_PATH(path, path_len)
        Z_PARAM_OPTIONAL
        Z_PARAM_BOOL(full)
    ZEND_PARSE_PARAMETERS_END();

    char *err = NULL;
    if (rdump_write_file(path, full ? 1 : 0, &err) != 0) {
        php_error_docref(NULL, E_WARNING, "rdump_dump: %s",
                         err ? err : "unknown error");
        RETURN_FALSE;
    }
    RETURN_TRUE;
}

/* ------------------------------------------------------------------ */
/* Auto-dump on memory_limit exhaustion (opt-in via rdump.oom_dump).   */
/*                                                                     */
/* A register_shutdown_function() cannot capture every OOM: when the   */
/* fatal is itself a VM-stack page allocation (or the stack sits right */
/* at a page boundary), pushing the shutdown closure's own call frame  */
/* needs another emalloc that fails too, so the handler never runs.    */
/* Hooking zend_error_cb catches the fatal in C, on the intact stack,  */
/* before any bailout/unwind -- and rdump_dump()'s libc-malloc path    */
/* runs regardless of the exhausted memory_limit. (Same technique as   */
/* php-memory-profiler's dump-on-limit.)                               */
/* ------------------------------------------------------------------ */

#define RDUMP_OOM_PREFIX "Allowed memory size of"

/* zend_error_cb's signature changed across versions; mirror it exactly
 * so the function-pointer assignment is type-compatible, and forward all
 * arguments unchanged to the previous handler. */
#if PHP_VERSION_ID < 80000
# if PHP_VERSION_ID < 70200
#  define RDUMP_ERROR_CB_PARAMS \
    int type, const char *error_filename, const unsigned int error_lineno, \
    const char *format, va_list args
# else
#  define RDUMP_ERROR_CB_PARAMS \
    int type, const char *error_filename, const uint32_t error_lineno, \
    const char *format, va_list args
# endif
# define RDUMP_ERROR_CB_PASSTHRU type, error_filename, error_lineno, format, args
# define RDUMP_ERROR_MSG (format)
#elif PHP_VERSION_ID < 80100
# define RDUMP_ERROR_CB_PARAMS \
    int type, const char *error_filename, const uint32_t error_lineno, \
    zend_string *message
# define RDUMP_ERROR_CB_PASSTHRU type, error_filename, error_lineno, message
# define RDUMP_ERROR_MSG (ZSTR_VAL(message))
#else
# define RDUMP_ERROR_CB_PARAMS \
    int type, zend_string *error_filename, const uint32_t error_lineno, \
    zend_string *message
# define RDUMP_ERROR_CB_PASSTHRU type, error_filename, error_lineno, message
# define RDUMP_ERROR_MSG (ZSTR_VAL(message))
#endif

static void (*rdump_original_error_cb)(RDUMP_ERROR_CB_PARAMS);

/* Expand an OOM-dump path template into buf: %p -> pid, %i -> thread id,
 * %t -> unix time, %% -> a literal '%'. Any other %x is emitted verbatim.
 * This lets a single static rdump.oom_dump INI value give each FPM worker
 * (%p) or ZTS thread (%i) its own file instead of racing on one path.
 * Returns 0 on success, -1 if the result would not fit (caller then falls
 * back to the literal template). */
static int rdump_expand_path(const char *tmpl, char *buf, size_t buf_sz)
{
    size_t o = 0;
    const char *p;
    for (p = tmpl; *p != '\0'; p++) {
        if (*p == '%' && p[1] != '\0') {
            int n;
            p++;
            if (*p == 'p') {
                n = snprintf(buf + o, buf_sz - o, "%lld", (long long)getpid());
            } else if (*p == 'i') {
                /* Thread id: distinguishes ZTS worker threads that share one
                 * PID, so concurrent OOM dumps don't collide on one path. */
                n = snprintf(buf + o, buf_sz - o, "%ld", (long)syscall(SYS_gettid));
            } else if (*p == 't') {
                n = snprintf(buf + o, buf_sz - o, "%lld", (long long)time(NULL));
            } else if (*p == '%') {
                n = snprintf(buf + o, buf_sz - o, "%%");
            } else {
                n = snprintf(buf + o, buf_sz - o, "%%%c", *p);
            }
            if (n < 0 || (size_t)n >= buf_sz - o) {
                return -1;
            }
            o += (size_t)n;
        } else {
            if (o + 1 >= buf_sz) {
                return -1;
            }
            buf[o++] = *p;
        }
    }
    buf[o] = '\0';
    return 0;
}

/* Remove a stale "<dump_path>.done" before a dump is (re)written to the same
 * path. Without this, a fixed-path dump would keep last run's marker present
 * while the new dump is mid-write, telling a watcher the half-written file is
 * already complete. Cleared before truncation, re-created only on success. */
static void rdump_clear_done_marker(const char *dump_path)
{
    char marker[4096];
    if ((size_t)snprintf(marker, sizeof(marker), "%s.done", dump_path)
            >= sizeof(marker)) {
        return;
    }
    unlink(marker);
}

/* Drop a "<dump_path>.done" marker once a dump is fully written and closed.
 * A directory watcher can wait for this instead of the .rdump, so it never
 * ships a half-written file -- the atomic-visibility benefit of a temp+rename
 * without the rename, and safe to run on the OOM death path (libc only).
 * Best-effort: a missing marker just means "no completion signal". */
static void rdump_write_done_marker(const char *dump_path)
{
    char marker[4096];
    if ((size_t)snprintf(marker, sizeof(marker), "%s.done", dump_path)
            >= sizeof(marker)) {
        return;
    }
    /* O_NONBLOCK so a FIFO planted at the path can't hang us; the regular-file
     * check then ignores any non-regular target. */
    int fd = open(marker, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    if (fd < 0) {
        return;
    }
    struct stat st;
    if (fstat(fd, &st) == 0 && !S_ISREG(st.st_mode)) {
        close(fd);
        return;
    }
    close(fd);
}

/* Release any runtime override set via rdump_set_oom_dump(). */
static void rdump_clear_runtime_oom_dump(void)
{
    if (RDUMP_G(oom_dump_runtime) != NULL) {
        free(RDUMP_G(oom_dump_runtime));
        RDUMP_G(oom_dump_runtime) = NULL;
    }
    RDUMP_G(oom_dump_runtime_set) = 0;
    RDUMP_G(oom_dump_runtime_full) = 0;
}

static void rdump_zend_error_cb(RDUMP_ERROR_CB_PARAMS)
{
    /* This runs on every diagnostic that goes through zend_error_cb
     * (notices, warnings, deprecations, ...). The auto-dump is the cold,
     * exceptional path, so keep the common case to a single predicted-
     * not-taken comparison and resolve everything else only inside. */
    if (UNEXPECTED(type == E_ERROR && !RDUMP_G(in_oom_dump))) {
        /* Runtime override (rdump_set_oom_dump) wins over the INI default;
         * an override set to "" force-disables even when the INI has a path. */
        const char *path;
        int full;
        if (RDUMP_G(oom_dump_runtime_set)) {
            path = RDUMP_G(oom_dump_runtime);
            full = RDUMP_G(oom_dump_runtime_full) ? 1 : 0;
        } else {
            path = RDUMP_G(oom_dump);
            full = RDUMP_G(oom_dump_full) ? 1 : 0;
        }

        const char *msg = RDUMP_ERROR_MSG;
        if (path != NULL
            && path[0] != '\0'
            && msg != NULL
            && strncmp(msg, RDUMP_OOM_PREFIX, sizeof(RDUMP_OOM_PREFIX) - 1) == 0
        ) {
            /* Runaway guard: a worker pinned at memory_limit OOMs on nearly
             * every request, so cap the auto-dump over the worker's lifetime
             * by count and/or by minimum spacing. Both gates apply together;
             * the default max of 1 keeps it to a single dump per worker. */
            char expanded[4096];
            const char *out_path = path;
            int path_ok = 1;
            if (strchr(path, '%') != NULL) {
                if (rdump_expand_path(path, expanded, sizeof(expanded)) == 0) {
                    out_path = expanded;
                } else {
                    /* Template too long to expand. Don't fall back to the
                     * literal "%p"/"%i" path: that would point every worker or
                     * thread at the same file and clobber each other. Skip. */
                    path_ok = 0;
                }
            }

            zend_long max = RDUMP_G(oom_dump_max);
            zend_long interval = RDUMP_G(oom_dump_min_interval);
            zend_long budget = rdump_parse_bytes(RDUMP_G(oom_dump_max_total));
            zend_long now = (zend_long)time(NULL);
            int capped = (max > 0 && RDUMP_G(oom_dump_count) >= max);
            int too_soon = (interval > 0
                && RDUMP_G(oom_dump_last_ts) != 0
                && now - RDUMP_G(oom_dump_last_ts) < interval);

            /* Disk budget: if the *.rdump files already in the target's
             * directory (excluding the one we'd overwrite) total at or above
             * the budget, skip -- this is the only gate that bounds the
             * combined footprint of many workers, not just one worker's rate.
             * Best-effort: concurrent workers can each pass and overshoot. */
            int over_budget = 0;
            if (path_ok && !capped && !too_soon && budget > 0) {
                char pathcopy[4096];
                const char *dir = ".";
                const char *base = out_path;
                if (snprintf(pathcopy, sizeof(pathcopy), "%s", out_path)
                        < (int)sizeof(pathcopy)) {
                    char *slash = strrchr(pathcopy, '/');
                    if (slash != NULL) {
                        *slash = '\0';
                        dir = pathcopy[0] != '\0' ? pathcopy : "/";
                        base = slash + 1;
                    }
                }
                /* Also reserve what the new dump is known to contain -- at
                 * least its ZendMM heap (~memory_get_usage(true)). The real
                 * dump is never smaller than that, so adding this lower bound
                 * never skips a dump that would have fit, yet it heads off the
                 * "the dump that crosses the line still gets written" overshoot
                 * in the cases where even the guaranteed minimum won't fit. */
                uint64_t existing = rdump_dir_rdump_total(dir, base);
                uint64_t incoming = (uint64_t)zend_memory_usage(1);
                if (existing + incoming >= (uint64_t)budget) {
                    over_budget = 1;
                }
            }

            if (path_ok && !capped && !too_soon && !over_budget) {
                /* Count the attempt (success or failure) and stamp the time
                 * before writing, so a broken path or full disk cannot retry
                 * without bound either. */
                RDUMP_G(oom_dump_count)++;
                RDUMP_G(oom_dump_last_ts) = now;

                /* Drop any prior completion marker before truncating the dump,
                 * so a watcher never sees a stale .done over a half-rewritten
                 * file when the same path is reused. */
                if (RDUMP_G(oom_dump_marker)) {
                    rdump_clear_done_marker(out_path);
                }

                char *err = NULL;
                RDUMP_G(in_oom_dump) = 1;
                /* Best-effort: this is the death path, so swallow any failure. */
                int wrote = rdump_write_file(out_path, full, &err);
                RDUMP_G(in_oom_dump) = 0;

                /* Signal completion only once the dump is fully on disk. */
                if (wrote == 0 && RDUMP_G(oom_dump_marker)) {
                    rdump_write_done_marker(out_path);
                }
            }
        }
    }
    rdump_original_error_cb(RDUMP_ERROR_CB_PASSTHRU);
}

/* ------------------------------------------------------------------ */
/* PHP function: bool rdump_set_oom_dump(?string $path, bool $full=false)
 *
 * Runtime toggle for the OOM auto-dump, overriding the INI default for
 * the rest of the current request:
 *   - non-empty path -> enable, writing to that path (so callers can use
 *     a per-request/per-pid filename the static INI cannot express);
 *   - ""             -> force-disable even if rdump.oom_dump is set;
 *   - null           -> clear the override and fall back to the INI.
 * Returns true on success, or false (with an E_WARNING) if the path could
 * not be stored, in which case the override is left cleared.
 */
/* ------------------------------------------------------------------ */

ZEND_BEGIN_ARG_INFO_EX(arginfo_rdump_set_oom_dump, 0, 0, 1)
    ZEND_ARG_INFO(0, path)
    ZEND_ARG_INFO(0, full)
ZEND_END_ARG_INFO()

PHP_FUNCTION(rdump_set_oom_dump)
{
    char *path = NULL;
    size_t path_len = 0;
    zend_bool full = 0;

    /* "p!" validates the path (rejecting NUL bytes) and allows null. */
    if (zend_parse_parameters(ZEND_NUM_ARGS(), "p!|b",
            &path, &path_len, &full) == FAILURE) {
        return;
    }

    rdump_clear_runtime_oom_dump();

    if (path != NULL) {
        char *copy = strdup(path);
        if (copy == NULL) {
            /* strdup can fail exactly when callers reach for this API -- near
             * the memory_limit. Leave the override cleared (not half-set) so
             * resolution falls back to the rdump.oom_dump INI default rather
             * than silently disabling the auto-dump, and signal the failure. */
            php_error_docref(NULL, E_WARNING,
                "rdump_set_oom_dump: out of memory copying the path");
            RETURN_FALSE;
        }
        RDUMP_G(oom_dump_runtime) = copy;
        RDUMP_G(oom_dump_runtime_set) = 1;
        RDUMP_G(oom_dump_runtime_full) = full ? 1 : 0;
    }

    RETURN_TRUE;
}

/* ------------------------------------------------------------------ */
/* Module plumbing.                                                   */
/* ------------------------------------------------------------------ */

PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY(
        "rdump.oom_dump", "", PHP_INI_SYSTEM, OnUpdateString,
        oom_dump, zend_rdump_globals, rdump_globals
    )
    STD_PHP_INI_BOOLEAN(
        "rdump.oom_dump_full", "0", PHP_INI_SYSTEM, OnUpdateBool,
        oom_dump_full, zend_rdump_globals, rdump_globals
    )
    STD_PHP_INI_ENTRY(
        "rdump.oom_dump_max", "1", PHP_INI_SYSTEM, OnUpdateLong,
        oom_dump_max, zend_rdump_globals, rdump_globals
    )
    STD_PHP_INI_ENTRY(
        "rdump.oom_dump_min_interval", "0", PHP_INI_SYSTEM, OnUpdateLong,
        oom_dump_min_interval, zend_rdump_globals, rdump_globals
    )
    STD_PHP_INI_ENTRY(
        "rdump.oom_dump_max_total", "0", PHP_INI_SYSTEM, OnUpdateString,
        oom_dump_max_total, zend_rdump_globals, rdump_globals
    )
    STD_PHP_INI_BOOLEAN(
        "rdump.oom_dump_marker", "0", PHP_INI_SYSTEM, OnUpdateBool,
        oom_dump_marker, zend_rdump_globals, rdump_globals
    )
    /* Crash-safe region reads via /proc/self/mem. Off by default (the direct
     * copy is faster, and safe under NTS); recommended under ZTS, where a
     * concurrent thread can unmap a region mid-dump and crash it. */
    STD_PHP_INI_BOOLEAN(
        "rdump.safe_read", "0", PHP_INI_SYSTEM, OnUpdateBool,
        safe_read, zend_rdump_globals, rdump_globals
    )
PHP_INI_END()

static const zend_function_entry rdump_functions[] = {
    PHP_FE(rdump_dump, arginfo_rdump_dump)
    PHP_FE(rdump_set_oom_dump, arginfo_rdump_set_oom_dump)
    PHP_FE_END
};

static PHP_GINIT_FUNCTION(rdump)
{
#if defined(COMPILE_DL_RDUMP) && defined(ZTS)
    ZEND_TSRMLS_CACHE_UPDATE();
#endif
    rdump_globals->oom_dump = NULL;
    rdump_globals->oom_dump_full = 0;
    rdump_globals->oom_dump_max = 1;
    rdump_globals->oom_dump_min_interval = 0;
    rdump_globals->oom_dump_max_total = NULL;
    rdump_globals->oom_dump_count = 0;
    rdump_globals->oom_dump_last_ts = 0;
    rdump_globals->oom_dump_marker = 0;
    rdump_globals->safe_read = 0;
    rdump_globals->oom_dump_runtime = NULL;
    rdump_globals->oom_dump_runtime_full = 0;
    rdump_globals->oom_dump_runtime_set = 0;
    rdump_globals->in_oom_dump = 0;
}

PHP_RSHUTDOWN_FUNCTION(rdump)
{
    /* A runtime override lasts only for the request that set it. */
    rdump_clear_runtime_oom_dump();
    return SUCCESS;
}

PHP_MINIT_FUNCTION(rdump)
{
    REGISTER_INI_ENTRIES();
    rdump_original_error_cb = zend_error_cb;
    zend_error_cb = rdump_zend_error_cb;
    return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(rdump)
{
    /* Only un-hook if nothing else chained on top of us, to avoid
     * cutting another extension's handler out of the chain. */
    if (zend_error_cb == rdump_zend_error_cb) {
        zend_error_cb = rdump_original_error_cb;
    }
    UNREGISTER_INI_ENTRIES();
    return SUCCESS;
}

PHP_MINFO_FUNCTION(rdump)
{
    char php_version[8];
    snprintf(php_version, sizeof(php_version), "v%d%d",
             PHP_MAJOR_VERSION, PHP_MINOR_VERSION);

    php_info_print_table_start();
    php_info_print_table_row(2, "rdump support", "enabled");
    php_info_print_table_row(2, "extension version", PHP_RDUMP_VERSION);
    php_info_print_table_row(2, "RDUMP format version", "3");
    php_info_print_table_row(2, "target php version tag", php_version);
    php_info_print_table_row(2, "memory_limit auto-dump", "available (zend_error_cb)");
    php_info_print_table_end();

    DISPLAY_INI_ENTRIES();
}

zend_module_entry rdump_module_entry = {
    STANDARD_MODULE_HEADER,
    "rdump",
    rdump_functions,
    PHP_MINIT(rdump),
    PHP_MSHUTDOWN(rdump),
    NULL,           /* RINIT */
    PHP_RSHUTDOWN(rdump),
    PHP_MINFO(rdump),
    PHP_RDUMP_VERSION,
    PHP_MODULE_GLOBALS(rdump),
    PHP_GINIT(rdump),
    NULL,           /* GSHUTDOWN */
    NULL,           /* post deactivate */
    STANDARD_MODULE_PROPERTIES_EX
};

#ifdef COMPILE_DL_RDUMP
# ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
# endif
ZEND_GET_MODULE(rdump)
#endif
