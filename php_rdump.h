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

#if defined(ZTS) && defined(COMPILE_DL_RDUMP)
ZEND_TSRMLS_CACHE_EXTERN()
#endif

#endif /* PHP_RDUMP_H */
