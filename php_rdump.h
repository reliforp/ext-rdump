/*
 * ext-rdump: dump the calling PHP process's own memory into reli's
 * RDUMP dump-file format, from inside the process, without ptrace.
 *
 * The produced file is byte-compatible with the format written by
 * reli's Reli\Inspector\MemoryDump\MemoryDumpWriter (magic "RDUMP",
 * format version 3) and can be analysed with:
 *
 *     php reli inspector:memory:dump:inspect <file>
 *     php reli inspector:memory:analyze     <file>
 */

#ifndef PHP_RDUMP_H
#define PHP_RDUMP_H

extern zend_module_entry rdump_module_entry;
#define phpext_rdump_ptr &rdump_module_entry

#define PHP_RDUMP_VERSION "0.1.0"

/* RDUMP dump-file format version we emit (must match reli's reader range). */
#define RDUMP_FORMAT_VERSION 3

/* Per-(thread-)module state, holding the INI-configured auto-dump settings
 * and a re-entrancy guard for the zend_error_cb OOM hook. */
ZEND_BEGIN_MODULE_GLOBALS(rdump)
    char *oom_dump;          /* rdump.oom_dump (INI): path, or "" to disable */
    zend_bool oom_dump_full; /* rdump.oom_dump_full (INI) */
    /* Runaway guard for the OOM auto-dump. A worker that keeps hitting
     * memory_limit would otherwise dump on every request and bury the disk;
     * these cap the dumps over the worker's whole lifetime, not per request.
     * count/last_ts are process- (per-thread-) scoped and survive RSHUTDOWN. */
    zend_long oom_dump_max;          /* rdump.oom_dump_max (INI): 0 = unlimited */
    zend_long oom_dump_min_interval; /* rdump.oom_dump_min_interval (INI), seconds; 0 = off */
    char *oom_dump_max_total;        /* rdump.oom_dump_max_total (INI): byte budget
                                      * across all *.rdump in the dump dir (K/M/G
                                      * suffixes; "0" = off), parsed on use. */
    zend_long oom_dump_count;        /* OOM dumps attempted so far this process */
    zend_long oom_dump_last_ts;      /* time() of the last OOM dump attempt, 0 = none */
    zend_bool oom_dump_marker;       /* rdump.oom_dump_marker (INI): write <path>.done */
    /* Runtime override set via rdump_set_oom_dump(). Owned by us (libc
     * strdup/free), reset each request. Takes precedence over the INI
     * default when oom_dump_runtime_set is true ("" => force-disabled). */
    char *oom_dump_runtime;
    zend_bool oom_dump_runtime_full;
    zend_bool oom_dump_runtime_set;
    zend_bool in_oom_dump;   /* guard against re-entering the dump */
ZEND_END_MODULE_GLOBALS(rdump)

#define RDUMP_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(rdump, v)

#if defined(ZTS) && defined(COMPILE_DL_RDUMP)
ZEND_TSRMLS_CACHE_EXTERN()
#endif

#endif /* PHP_RDUMP_H */
