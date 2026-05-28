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
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

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

/* ------------------------------------------------------------------ */
/* Dump writer.                                                       */
/* ------------------------------------------------------------------ */

static const char rdump_magic[8] = {'R', 'D', 'U', 'M', 'P', '\0', '\0', '\0'};

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

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        free(entries);
        free(maps_buf);
        *err = "failed to open output file for writing";
        return -1;
    }

    int rc = 0;
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
        RDUMP_TRY(rdump_wr(fp, (const void *)(uintptr_t)e->start, (size_t)size));
    }

#undef RDUMP_TRY
done:
    if (rc != 0) {
        *err = "failed while writing dump file (disk full?)";
    }
    fclose(fp);
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
/* Module plumbing.                                                   */
/* ------------------------------------------------------------------ */

static const zend_function_entry rdump_functions[] = {
    PHP_FE(rdump_dump, arginfo_rdump_dump)
    PHP_FE_END
};

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
    php_info_print_table_end();
}

zend_module_entry rdump_module_entry = {
    STANDARD_MODULE_HEADER,
    "rdump",
    rdump_functions,
    NULL,           /* MINIT */
    NULL,           /* MSHUTDOWN */
    NULL,           /* RINIT */
    NULL,           /* RSHUTDOWN */
    PHP_MINFO(rdump),
    PHP_RDUMP_VERSION,
    STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_RDUMP
# ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
# endif
ZEND_GET_MODULE(rdump)
#endif
