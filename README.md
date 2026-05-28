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
- `reli` — only on the machine where you *analyse* the dump

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

The dump reflects the process exactly as it is at the moment of the call —
including the live VM call stack — so trigger it where it matters, for example
on demand from a signal handler:

```php
// Dump on demand: kill -USR2 <pid>
pcntl_signal(SIGUSR2, fn() => rdump_dump('/tmp/sig.rdump'));
```

To capture the moment of `memory_limit` exhaustion, don't wire this into a
shutdown function — let the extension hook the fatal in C instead (see
[below](#capturing-the-moment-of-memory_limit-death)); it catches even the OOMs
a shutdown handler cannot.

Pass `$full = true` for a self-contained dump (also embeds read-only code
segments) when you will analyse it on a host that lacks the original binaries.

### Capturing the moment of `memory_limit` death

A `register_shutdown_function` misses the worst OOMs: when the fatal is itself
a VM-stack allocation (or the stack sits at a page boundary), pushing the
handler's own call frame needs another allocation that also fails, so the
handler never runs. To catch those, let the extension hook the fatal in C, via
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
overwrite one path. (This expansion applies to the OOM auto-dump path only —
both the `rdump.oom_dump` INI value and a `rdump_set_oom_dump()` override — not
to the literal `$path` you pass to `rdump_dump()`.)

On an `Allowed memory size ... exhausted` fatal, the dump is written straight
from the engine's error callback (`zend_error_cb`) — on the intact stack,
before any unwind, and off the `memory_limit` heap (libc `malloc`) — so it
captures even the OOMs a shutdown handler cannot.

#### Don't let it run away

A worker pinned at `memory_limit` tends to OOM on *every* request, so an
unguarded auto-dump would write a fresh (often 100 MB+) file each time and bury
the disk. By default the auto-dump therefore fires **at most once per worker
process** (`rdump.oom_dump_max=1`); the counter is per worker and spans requests,
so restarting the worker re-arms it. Raise the cap if you want more, and/or set a
minimum spacing — both gates apply together:

```ini
rdump.oom_dump_max=5            ; up to 5 dumps over the worker's lifetime (0 = unlimited)
rdump.oom_dump_min_interval=60  ; ...and at least 60s apart
```

The per-worker count still leaves the fleet total unbounded — 200 workers writing
one 130 MB dump each is 26 GB. To cap the *combined* footprint, set a byte budget
(accepts `K`/`M`/`G` suffixes); before each auto-dump the extension sums the
`*.rdump` files already in the dump's directory and skips if they meet the budget:

```ini
rdump.oom_dump_max_total=2G     ; skip the auto-dump once *.rdump there would exceed 2G (0 = off)
```

The check adds the incoming dump's own minimum size (its ZendMM heap,
`~memory_get_usage(true)`) to the existing total, so it skips *before* crossing
the line rather than one dump after. A corollary: set the budget to at least a
few times a single dump's size — a budget smaller than one dump means no dump is
ever written.

Two caveats remain: it is still a *soft* limit — concurrent workers can each pass
the check and overshoot together, and the reservation is a lower bound, so the
written dump can be larger than reserved. And it counts **every** `*.rdump` in
that directory, so give OOM dumps a directory of their own if other `.rdump`
files live alongside.

A dump attempt counts toward the cap whether it succeeds or not, so a bad path or
a full disk can't retry forever either. These guards apply only to the automatic
OOM dump — explicit `rdump_dump()` calls are never throttled. (Combine them with a
`%p` path so each worker still writes a distinct file.)

#### Shipping dumps with a watcher

If a separate process tails the dump directory and ships dumps elsewhere, point
it at a **completion marker** rather than the `.rdump` itself, so it never grabs a
half-written file:

```ini
rdump.oom_dump_marker=1   ; after a complete dump, also create "<path>.done"
```

The `<path>.done` file is created (empty, `0600`) only once the dump is fully
written and closed — wait for `oom-1234.rdump.done`, then ship
`oom-1234.rdump`. When a dump reuses a path, the old marker is removed *before*
the new dump is truncated and re-created only when it completes, so a `.done` is
never stale over a half-rewritten file. This gives the atomic-visibility guarantee a temp-file +
`rename` would, without the rename's downsides, and works on the OOM death path
(it's a plain `open`/`close`). There is deliberately **no PHP completion
callback**: the OOM dump runs inside the engine's error handler on a process that
has just exhausted `memory_limit`, where re-entering PHP userland is unsafe. To
react in-process instead, register a shutdown function that checks
`error_get_last()` — it runs in a safe phase, after the dump is already on disk.

You can also toggle it at runtime — handy for a per-request/per-pid filename,
or when you cannot edit php.ini:

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

```bash
# header / memory map / regions
php reli inspector:memory:dump:inspect /tmp/app.rdump

# full analysis: type breakdown, retention, leaks, cycles, findings
php reli inspector:memory:analyze /tmp/app.rdump

# human-readable report
php reli inspector:memory:analyze /tmp/app.rdump -f report
```

Analysing on a different host? Either use `$full = true` at capture time, or
point reli at a copy of the target's filesystem with `--dependency-root=/path`.

## Security

**A dump is a verbatim copy of your process's memory — treat the file as highly
sensitive.** It can contain anything that was resident at the moment of capture:
environment variables, database and API credentials, session tokens, decrypted
request and response bodies, private keys, other users' data on a shared
process, and so on.

- The file is created `0600` (owner-only) with `O_NOFOLLOW` and `O_CLOEXEC`, and
  a pre-existing target is `fchmod`-ed back to `0600`. Write it somewhere only
  the intended user can read — not a world-readable temp dir or a webroot.
- Delete it once you have finished analysing, and scrub it from any backups or
  log shipping. Treat it like a credential dump, because it can be one.
- Move dumps to the analysis host over a secure channel; do not email them or
  drop them in shared buckets unencrypted.

## Notes

- Linux only (the dump is built from `/proc/self/maps`).
- The snapshot is taken synchronously in the calling thread — not stop-the-world.
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
  one chained call — only when an error is actually emitted. Cost is incurred
  only when you call `rdump_dump()` or an OOM dump fires; safe to keep resident.
- Plays nice with other `zend_error_cb` users (e.g. php-memory-profiler): the
  hook always chains to the previous handler, so every extension still fires and
  writes its own dump on OOM.

## License

MIT. See [LICENSE](LICENSE).
