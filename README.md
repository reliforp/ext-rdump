# ext-rdump

[![build](https://github.com/reliforp/ext-rdump/actions/workflows/build.yml/badge.svg)](https://github.com/reliforp/ext-rdump/actions/workflows/build.yml)

Take a memory dump of the **current PHP process** at any point in time, for later
analysis with [reli](https://github.com/reliforp/reli-prof).

One call writes a snapshot of your process's memory to a file:

```php
rdump_dump('/tmp/app.rdump');
```

[Feed that file to reli](https://github.com/reliforp/reli-prof/blob/HEAD/docs/memory/memory-dump.md)
and you can:

- see the **call stack** at the moment of the dump,
- find out **what objects/strings/arrays exist, how much memory each uses, and
  how they reference each other**,
- help investigate **memory leaks, resource leaks, circular references, and
  memory bottlenecks**.

`reli` can do all this by attaching from outside, but that needs `ptrace`.
`ext-rdump` dumps from *inside* the process instead, without needing ptrace.

## Requirements

- Linux, PHP **7.0 – 8.5** (NTS and ZTS)
- `reli` (only on the machine where you *analyse* the dump)

## Install

From source:

```bash
phpize && ./configure --enable-rdump && make && sudo make install
# then add to php.ini:
# extension=rdump.so
```

Or via [PIE](https://github.com/php/pie) / PECL:

```bash
pie install reliforp/ext-rdump
# or
pecl install package.xml   # from a checkout
```

## Usage

```php
bool rdump_dump(string $path, bool $full = false)
```

Writes the dump to `$path`; returns `false` (with an `E_WARNING`) on failure.
Pass `$full = true` for a self-contained dump (also embeds read-only code
segments) when you will analyse it on a host that lacks the original binaries.

### On demand from a signal handler

```php
// kill -USR2 <pid>
pcntl_async_signals(true);
pcntl_signal(SIGUSR2, fn() => rdump_dump('/tmp/sig.rdump'));
```

### Capturing the moment of `memory_limit` death

To capture the moment of `memory_limit` exhaustion, enable the auto-dump in
php.ini:

```ini
extension=rdump.so
rdump.oom_dump=/var/log/php-oom-%p.rdump   ; empty = disabled
;rdump.oom_dump_full=1                     ; also embed read-only segments
;rdump.oom_dump_max=1                      ; max auto-dumps per worker (0 = unlimited)
;rdump.oom_dump_min_interval=0             ; min seconds between auto-dumps (0 = off)
```

The path may contain `%p` (PID) and `%t` (Unix time), expanded when the dump
fires; `%%` is a literal `%`. Under PHP-FPM and other multi-worker setups give
the template a `%p` so workers write distinct files instead of racing to
overwrite one path. (This expansion applies only to the OOM auto-dump path:
both the `rdump.oom_dump` INI value and a `rdump_set_oom_dump()` override. It
does not apply to the literal `$path` you pass to `rdump_dump()`.)

#### Limiting the auto-dump

When application code or data keeps exhausting `memory_limit`, a worker can OOM
request after request, so an unguarded auto-dump could write a fresh (often
100 MB+) file every time and fill the disk. Three INI guards, all applied together:

```ini
rdump.oom_dump_max=1            ; max dumps per worker process (default 1; 0 = unlimited)
rdump.oom_dump_min_interval=0   ; min seconds between dumps (default 0 = off)
rdump.oom_dump_max_total=0      ; skip once *.rdump in the dump dir would exceed this (K/M/G; 0 = off)
```

`oom_dump_max` is a per-worker count (default 1 = one dump per worker).
`oom_dump_max_total` bounds the *combined* footprint across workers; it counts
every `*.rdump` in the directory plus the incoming dump's minimum size. Both are
best-effort: concurrent workers can still overshoot together.

These guards apply only to the automatic OOM dump; explicit `rdump_dump()` calls
are never throttled. Give the path a `%p` so each worker writes a distinct file.

#### Shipping dumps with a watcher

Set `rdump.oom_dump_marker=1` and the extension drops an empty `<path>.done`
once a dump is fully written. A watcher waits for `oom-1234.rdump.done` before
shipping `oom-1234.rdump`, so it won't start on a half-written file. Give each
dump a unique path (`%p`, plus `%t` for repeats) so a later dump can't truncate
one while it is being shipped.

#### Setting the path at runtime

For a per-request or per-PID filename, or when you cannot edit php.ini, set the
auto-dump from code instead:

```php
rdump_set_oom_dump("/var/log/oom-" . getmypid() . ".rdump");
// rdump_set_oom_dump("/var/log/oom.rdump", true); // full dump
// rdump_set_oom_dump("");   // force off for this request, even if the INI set a path
// rdump_set_oom_dump(null); // clear the override, fall back to rdump.oom_dump
```

The runtime setting applies to the current request and takes precedence over
the `rdump.oom_dump` default. It returns `true` on success, or `false` (with an
`E_WARNING`) if the path could not be stored, leaving the previous default in
effect rather than silently disabling the dump.

## Analyse with reli

Reading the dump is reli's job; its
[memory-dump docs](https://github.com/reliforp/reli-prof/blob/HEAD/docs/memory/memory-dump.md)
have the full story. The easiest way to run it is reli's Docker image: its
[getting-started guide](https://github.com/reliforp/reli-prof/blob/HEAD/docs/getting-started.md)
sets up a `reli` command for you, with no local PHP 8.4 / FFI setup. Then:

```bash
# header / memory map / regions
reli inspector:memory:dump:inspect /tmp/app.rdump

# full analysis: type breakdown, retention, leaks, cycles, findings
reli inspector:memory:analyze /tmp/app.rdump

# human-readable report
reli inspector:memory:analyze /tmp/app.rdump -f report
```

Analysing on a different host? Either use `$full = true` at capture time, or
point reli at a copy of the target's filesystem with `--dependency-root=/path`.

## Security

**A dump is a verbatim copy of your process's memory, so treat the file as highly
sensitive.** It can contain anything that was resident at the moment of capture:
environment variables, database and API credentials, session tokens, decrypted
request and response bodies, private keys, other users' data on a shared
process, and so on.

- The file is created `0600` (owner-only) with `O_NOFOLLOW` and `O_CLOEXEC`, and
  a pre-existing target is `fchmod`-ed back to `0600`. Write it somewhere only
  the intended user can read, not a world-readable temp dir or a webroot.
- Delete it once you have finished analysing, and scrub it from any backups or
  log shipping. Treat it like a credential dump, because it can be one.
- Move dumps to the analysis host over a secure channel; do not email them or
  drop them in shared buckets unencrypted.

## Notes

- Linux only (the dump is built from `/proc/self/maps`).
- The snapshot is taken synchronously in the calling thread, not stop-the-world.
  On a busy ZTS process other threads may change memory while it is written, so
  the dump can be internally inconsistent. In the worst case a region read for
  the maps snapshot is `munmap`/`mprotect`-ed by another thread before its bytes
  are copied, which can **crash** the process mid-dump. In practice this is rare,
  and on the OOM death path it costs you nothing you were not already losing, but
  do not wire `rdump_dump()` into a hot, otherwise-healthy request path expecting
  it to be perfectly safe under heavy multi-threaded churn.
- The output is byte-compatible with reli's RDUMP format (the same file
  `reli inspector:memory:dump` produces).
- **No steady-state overhead.** The extension installs no executor or allocator
  hooks, so normal execution runs at full speed. The only always-on hook is on
  `zend_error_cb` (the error path), where it adds a couple of comparisons and
  one chained call, only when an error is actually emitted. Cost is incurred
  only when you call `rdump_dump()` or an OOM dump fires; safe to keep resident.
- Plays nice with other `zend_error_cb` users (e.g. php-memory-profiler): the
  hook always chains to the previous handler, so every extension still fires and
  writes its own dump on OOM.

## License

MIT. See [LICENSE](LICENSE).
