# ext-rdump

[![build](https://github.com/reliforp/ext-rdump/actions/workflows/build.yml/badge.svg)](https://github.com/reliforp/ext-rdump/actions/workflows/build.yml)

Take a memory dump of the **current PHP process** at any point in time, then hand
it to [reli](https://github.com/reliforp/reli-prof) for analysis.

One call writes a snapshot of your process's memory to a file:

```php
rdump_dump('/tmp/app.rdump');
```

Feed that file to reli and you can:

- see the **call stack** at the moment of the dump,
- find out **what objects/strings/arrays exist, how much memory each uses, and
  how they reference each other**,
- automatically surface **memory leaks, resource leaks, circular references, and
  memory bottlenecks**.

`reli` can already do all of this by attaching from the outside, but that needs
`ptrace` granted to the PHP runtime. `ext-rdump` instead dumps the process from
*inside* itself — no ptrace, no external tooling on the target — which is often
an easier thing to ship to production.

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
including the live VM call stack — so trigger it where it matters:

```php
// On memory_limit exhaustion. Free a few KB first so the handler's own
// allocations (error_get_last() etc.) have headroom after the OOM;
// rdump_dump() builds the dump with libc malloc, off the memory_limit heap.
$reserve = str_repeat("\0", 4096);
register_shutdown_function(function () use (&$reserve) {
    $reserve = null;
    $e = error_get_last();
    if ($e !== null && strpos($e['message'], 'Allowed memory size') !== false) {
        rdump_dump('/tmp/oom.rdump');
    }
});

// Or proactively, before the limit is hit:
if (memory_get_usage() > 256 * 1024 * 1024) {
    rdump_dump('/tmp/highwater.rdump');
}

// Or on demand from a signal handler (kill -USR2 <pid>):
pcntl_signal(SIGUSR2, fn() => rdump_dump('/tmp/sig.rdump'));
```

> The few-KB reserve just gives the handler's own post-OOM bookkeeping
> (`error_get_last()` etc. — a few hundred bytes) room to run; rdump_dump()'s
> own buffers are libc `malloc`, off the `memory_limit` heap.

Pass `$full = true` for a self-contained dump (also embeds read-only code
segments) when you will analyse it on a host that lacks the original binaries.

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

## Notes

- Linux only (the dump is built from `/proc/self/maps`).
- The snapshot is taken synchronously in the calling thread — not stop-the-world;
  on a busy ZTS process other threads may change memory while it is written.
- The output is byte-compatible with reli's RDUMP format (the same file
  `reli inspector:memory:dump` produces).

## License

MIT. See [LICENSE](LICENSE).
