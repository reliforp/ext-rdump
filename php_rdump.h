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
    char *oom_dump;          /* rdump.oom_dump: path, or "" to disable */
    zend_bool oom_dump_full; /* rdump.oom_dump_full */
    zend_bool in_oom_dump;   /* guard against re-entering the dump */
ZEND_END_MODULE_GLOBALS(rdump)

#define RDUMP_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(rdump, v)

#if defined(ZTS) && defined(COMPILE_DL_RDUMP)
ZEND_TSRMLS_CACHE_EXTERN()
#endif

#endif /* PHP_RDUMP_H */
