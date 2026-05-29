# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/), and the project aims to follow
[Semantic Versioning](https://semver.org/).

## 0.1.0

Initial release: in-process memory dumps in reli's RDUMP format, without
`ptrace`.

### Added
- `rdump_dump(string $path, bool $full = false): bool` — write a dump of the
  current process's memory. `$full` also embeds read-only file-backed segments
  for host-independent analysis.
- `memory_limit` auto-dump via `zend_error_cb` (opt-in `rdump.oom_dump`),
  catching even the OOMs a `register_shutdown_function` cannot.
  - Runaway guards: `rdump.oom_dump_max` (per-worker count),
    `rdump.oom_dump_min_interval` (spacing), `rdump.oom_dump_max_total` (byte
    budget across `*.rdump` in the directory).
  - Path templates `%p` (PID), `%i` (thread id), `%t` (time), `%%`.
  - `rdump.oom_dump_marker` writes a `<path>.done` completion marker for
    directory watchers.
  - `rdump_set_oom_dump(?string $path, bool $full = false): bool` runtime
    override.
- `rdump.safe_read` reads regions via `/proc/self/mem` so a concurrent unmap
  yields zero-filled bytes instead of a crash. Default on under ZTS, off under
  NTS.

### Security
- Dumps are created `0600` with `O_EXCL` / `O_NOFOLLOW` / `O_CLOEXEC`; an
  existing regular file is replaced by a fresh inode, and non-regular targets
  are refused.

### Compatibility
- Linux, 64-bit little-endian, PHP 7.0 – 8.5 (NTS and ZTS).
