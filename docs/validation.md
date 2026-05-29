# Pre-release validation: ext-rdump + reli on real-world memory issues

This is a record of an end-to-end shakedown of `ext-rdump` against `reli`
before release. The goal was not a synthetic benchmark but to answer a
practical question: **if you take a real PHP program that people actually
report running out of memory on, can `ext-rdump` capture a dump cleanly, and
does `reli`'s report tell you something useful about why?**

Real cases were sourced by searching GitHub issues for `"allowed memory size
of"` and `"php memory usage"` (934 issues match the first query alone), then
reproducing representative ones locally.

**Verdict: yes, end to end.** Every workload captured a valid RDUMP, `reli`
analysed all of them, and in each case the top findings pointed straight at the
real cause — the parser AST, the accumulated array, the `Cell` objects, the
Doctrine UnitOfWork. The headline feature — the automatic dump on
`memory_limit` exhaustion — fired correctly and produced a report that
reconstructs the exact call stack at the moment of death. This holds across the
engine boundary too: dumps captured on **PHP 7.4** (Zend 3.4) analyse cleanly
with reli running on 8.4 (see [§ PHP 7.4](#php-74-zend-34-cross-version-validation)).

## Environment

| | |
|---|---|
| PHP | 8.4.19 (NTS) for cases 1–5; **7.4.33 (NTS)** for the cross-version run, x86-64 Linux |
| ext-rdump | this branch, `phpize && ./configure --enable-rdump && make` |
| reli | `reliforp/reli-prof` 0.13.x-dev (always runs on 8.4; reads v74 and v84 dumps) |
| Pipeline | `rdump_dump()` / OOM auto-dump → `inspector:memory:analyze -f rmem` → `inspector:memory:report` |

`ext-rdump`'s own test suite (now 7 tests, see [below](#test-added)) is green on
PHP 8.4.19 (**NTS and ZTS**) and 7.4.33.

## Results at a glance

| Case | Real issue | Workload | PHP heap | Dump | Graph | reli's top finding |
|---|---|---|---|---:|---:|---|
| 1. less.php | [wikimedia/less.php#104](https://github.com/wikimedia/less.php/issues/104) | compile 1.4 MB LESS | 212 MB | 328 MB | 6.5M nodes | `$parser->rules` holds 165 MB via 4,004 children; AST node classes named |
| 2. HTMLPurifier | [ezyang/htmlpurifier#270](https://github.com/ezyang/htmlpurifier/issues/270) | purify 6,000 docs | 9.8 MB | 22 MB | 60K nodes | `$clean` array 7.4 MB via 6,000 children; `ZendString` 69.7% of heap |
| 3. PhpSpreadsheet | [PHPOffice/PhpSpreadsheet#1094](https://github.com/PHPOffice/PhpSpreadsheet/issues/1094) | read a 2 MB / 360k-cell xlsx | 214 MB | 743 MB | 7.6M nodes | `Cell` × 360,000 (58% of objects); worksheet↔cell cycle 188 MB |
| 4. Doctrine ORM | canonical batch OOM (e.g. [sentry-laravel#178](https://github.com/getsentry/sentry-laravel/issues/178)) | 40k-order batch, no `clear()` | 198 MB | 225 MB | 4.3M nodes | `$em->unitOfWork->{entityIdentifiers,originalEntityData,identityMap}`; Order↔OrderItem cycle 188 MB |
| 5. **OOM auto-dump** | reproduces #1094's *exact* 128 MB fatal | read xlsx under `memory_limit=128M` | 125 MB (97.7% of limit) | 971 MB | 4.3M nodes | `near_memory_limit` HIGH + **exact call stack at the fatal** |

In every case the analysed-heap coverage was 94–98.5%.

## Per-case detail

### 1. less.php — cumulative parser growth (#104)

Compiling a 1.4 MB generated stylesheet left the parser holding **212 MB**
(peak 321 MB) — the cumulative-growth shape the issue describes. `reli` reported:

- `bottleneck_path`: `$parser->rules[...]->rules->referenced` (190 MB)
- `choke_point` / `large_array`: `$parser->rules` — 165 MB via **4,004**
  children (exactly the 4,000 components compiled), plus
  `$parser->cachedEvaldRules` (21 MB)
- `companion_cluster` / `expensive_property`: the AST node classes by name —
  `Less_Tree_Ruleset` ×28,000, `Less_Tree_Declaration::$value`, etc.

That is a precise map of where less.php's memory goes, down to the named
properties on its tree nodes.

### 2. HTMLPurifier — definition cache (#270)

`#270` is a **PHP 7.4-specific `unserialize` engine regression**, so it does
**not** reproduce on 8.4 — an honest reproducibility caveat. Memory stayed flat
at ~10 MB. The case still validated `reli` on a real library's live heap: it
correctly isolated the accumulated `$clean` array (7.4 MB via 6,000 children),
flagged `ZendString` as 69.7% of the heap, and resolved deep HTMLPurifier
internals by PHP-syntax path
(`$config->definitions['HTML']->info`,
`$purifier->strategy->strategies[1]->zipper->referenced->front->...`).

### 3. PhpSpreadsheet — large-file read OOM (#1094)

A **2 MB** xlsx (360k cells) expanded to **214 MB** of heap on load — the
classic "small file, huge memory" report. `reli`:

- named the cause: `PhpOffice\PhpSpreadsheet\Cell\Cell` — **360,000 instances**,
  58% of object memory
- traced the bottleneck path to `…->cellCollection` (188 MB) and detected the
  worksheet↔cell reference **cycle** (15 classes, 188 MB retained)
- surfaced a genuine inefficiency: every `Cell` carries an `IgnoredErrors`
  object — 360,000 of them for **41 MB**, almost all empty
  (`structural_duplicate` flags 41 MB of identical shapes)

### 4. Doctrine ORM — the framework batch OOM

The single most common real-world "allowed memory size of" in Symfony/Laravel
batch commands: a long loop that `persist()`s and `flush()`es but never
`clear()`s, so the `EntityManager` retains every entity. 40,000 orders /
**160,000 managed entities** → 198 MB. `reli` pointed straight at the
`UnitOfWork`:

- `$em->unitOfWork->entityIdentifiers`, `originalEntityData` (67 MB **each** —
  the change-tracking snapshot, the non-obvious cost of skipping `clear()`),
  `identityMap['App\Order']` (40,000), `identityMap['App\OrderItem']` (120,000),
  `entityStates` (160,000)
- the bidirectional `Order`↔`OrderItem` reference **cycle** (188 MB retained)

Any Doctrine developer reading this report would immediately recognise the
missing `$em->clear()`.

### 5. OOM auto-dump — the headline feature

This is the capability that matters most in production: capture the moment of
`memory_limit` death **with no code change**, driven only by INI. Re-running the
PhpSpreadsheet read under `memory_limit=128M` reproduced #1094's exact fatal
("Allowed memory size of 134217728 bytes exhausted") and the extension wrote
`oom-<pid>.rdump` automatically. From that dump `reli` produced a full
post-mortem:

- `near_memory_limit` **HIGH** — "Peak usage is 97.7% of memory_limit — only
  2.91 MB headroom" (125 MB / 128 MB), confirming the snapshot is at the
  exhaustion point
- the **exact call stack at the fatal**:
  `preg_match → Coordinate::coordinateFromString:37 →
  Xlsx::loadSpreadsheetFromFile:913 → BaseReader::load:290 → <main>:18`,
  matching the engine's "Coordinate.php on line 37"
- the cause at that instant: 167,134 `Cell` objects (it died roughly half-way
  through the 360k), the cell cache holding 73.85 MB, plus the in-flight reader
  arrays

Auto-dump was validated on three independent paths:

- `rdump.oom_dump` via INI (with `%p` PID expansion) — fires
- `rdump_set_oom_dump()` at runtime, no INI — fires
- `rdump.oom_dump_marker=1` — the `<path>.rdump.done` marker is written
- `rdump.oom_dump_max_total` — with the budget set below the directory's
  existing `*.rdump` footprint, the next OOM dump is **skipped** (the process
  still dies, but no file is written), as designed

## Notable real-world insight: OOM dumps can be ~1 GB even at a 128 MB limit

The case-5 dump was **971 MB** despite the 128 MB PHP limit, because the dump
includes the glibc `[heap]` and anonymous mmaps, and PhpSpreadsheet's
libxml2/libzip allocate the XLSX parse tree in **C memory that `memory_limit`
never counts**. `reli` shows it plainly: `RSS: 985 MB` vs `Heap: 119 MB`. This
is exactly the confusing real-world situation where `memory_get_usage()` looks
fine but the process is huge — and `ext-rdump` captures it faithfully.

The practical consequence: on a worker that OOMs repeatedly, an unguarded
auto-dump could fill the disk fast. The existing guards
(`oom_dump_max`, `oom_dump_min_interval`, `oom_dump_max_total`) are the right
mitigation, and `oom_dump_max_total` is verified above. Worth keeping this
prominent in the README (it already is).

## PHP 7.4 (Zend 3.4) cross-version validation

`ext-rdump` advertises PHP 7.0–8.5, and 7.x is a genuinely different engine
(Zend 3.4, different ZendMM and struct layouts), so the OOM path and the dump
format were re-checked there. PHP 7.4.33 was built from the `php-src` tag
(the ondrej PPA and php.net are blocked by this environment's network policy;
`ext/openssl` was left out because PHP 7.4's openssl does not compile against
OpenSSL 3.0 — not needed at runtime here). reli always runs on 8.4 and reads the
7.4-produced **v74** dump.

- **Build + suite**: ext-rdump compiles cleanly against 7.4 and **all 7 phpt
  tests pass** under 7.4.33, including `dump.phpt` (which asserts the dump's
  `php_version` is `v74`) and the new `oom_dump.phpt`.
- **less.php v4 OOM (real #104 shape, captured by auto-dump)**: compiling a
  1.05 MB stylesheet exhausts 7.4's default 128 MB limit
  ("Allowed memory size of 134217728 bytes exhausted" in `Parser.php`). With
  `rdump.oom_dump` set, the extension auto-wrote a 137 MB v74 dump at the fatal,
  and the host reli (8.4) analysed it end to end (2.9M nodes, 90% analysed):
  - `near_memory_limit` HIGH — peak **99.2%** of `memory_limit` (126.97/128 MB)
  - the **exact 7.4 call stack**:
    `Less_Parser::parseMixinArgs:1665 → parseMixinCall:1505 → parsePrimary →
    … → parse:463 → <main>:23`, matching the fatal line
  - cause: the AST under construction (`$root`, 110 MB via 2,379 children) and
    the **v4** node classes by name (`Less_Tree_Rule` ×21,389,
    `Less_Tree_Ruleset` ×14,256, `Less_Tree_Expression` ×47,530) — note these
    differ from the v5 names in case 1, confirming reli resolves the v74 heap,
    not the host's 8.4 layout
- **HTMLPurifier on 7.4.33**: #270's `unserialize` leak was reported on 7.4.0 /
  7.4.10 and is **fixed by 7.4.33** (the last 7.4 release), so it stays flat at
  ~2 MB here too — not reproducible on any PHP we can install. reli still reads
  the small v74 dump and resolves HTMLPurifier's static config singleton
  (`HTMLPurifier_ConfigSchema->static_properties->singleton->defaultPlist->…`).

**Takeaway:** capture on 7.4, analyse on 8.4 — the format and the OOM auto-dump
work across the engine boundary, and reli applies the correct per-version
(v74 vs v84) layout automatically.

## Concurrency, ZTS, and `safe_read` (the crash path)

The one place ext-rdump can *crash the host process* is the documented ZTS race:
another thread `munmap`/`mprotect`s a region while the dumping thread is copying
it. `rdump.safe_read` exists to neutralise it (read regions through
`/proc/self/mem` so a vanished page returns an error instead of faulting). This
is the scariest surface, so it was reproduced and the mitigation verified
directly. PHP 8.4 ZTS was built from source for this.

First, the ZTS basics: ext-rdump **builds and loads on 8.4 ZTS**, the **phpt
suite is 7/7 green** there, and `rdump.safe_read` **defaults to on under ZTS**
(off under NTS) — i.e. the safe default is in place exactly where the race
exists.

Then the race itself, two ways — a raw pthread doing `mmap`/`munmap` churn (NTS),
and **real PHP interpreter threads** via `ext-parallel` churning their ZendMM
heaps so freed 2 MB chunks are returned to the OS = `munmap` (ZTS, the
FrankenPHP worker-threads shape):

| Build | `safe_read` | Result under concurrent `munmap` |
|---|---|---|
| NTS 8.4 | `0` | **SIGSEGV during the dump** |
| NTS 8.4 | `1` | completes every dump, no crash |
| ZTS 8.4 | `0` | **SIGSEGV during the dump** |
| ZTS 8.4 | `1` (the default) | completes every dump, no crash |

Both crashes have the **same backtrace**, which pins the hazard precisely:

```
#0 __memcpy_evex_unaligned_erms
#3 __GI__IO_fwrite (count=4194304)
#4 rdump_wr                rdump.c:64
#5 rdump_write_file        rdump.c:634   <- the safe_read=0 direct-copy branch
#6 zif_rdump_dump          rdump.c:687
```

i.e. `fwrite` `memcpy`-ing straight from a region pointer (`(const void *)e->start`)
that a concurrent thread had just unmapped. With `safe_read=1` that branch is
replaced by `rdump_write_region_safe()` (pread from `/proc/self/mem`,
EFAULT→zero-fill), and the dump loop runs to completion every time.

**Conclusion:** the hazard is real and is exactly the one documented; the
mitigation works; and because `safe_read` ships **on under ZTS**, the default
configuration is crash-safe. The only way to hit the crash is to *explicitly*
turn `safe_read` off under concurrent load — which the README already warns
against. No code change needed; the default is load-bearing and correct.

## Overhead and interop

- **No steady-state overhead.** A CPU + allocation-churn benchmark (best of 5)
  is within noise loaded vs not (cpu 0.061 s vs 0.062 s; alloc ~0.85 s either
  way), and unchanged with `rdump.oom_dump` set — consistent with the "only an
  error-path `zend_error_cb` hook, safe to keep resident" claim.
- **`zend_error_cb` chaining.** Loaded alongside a second extension that also
  hooks `zend_error_cb`, an OOM fires **both** handlers (ext-rdump writes its
  dump *and* the other extension runs) regardless of load order — confirming the
  "plays nice with php-memory-profiler etc." claim.

## Rough edges / recommendations

1. **Analysis is heavy on large dumps.** A 200 MB heap becomes a 4–8M-node
   graph; `inspector:memory:analyze` took ~1.5 min and ~2 GB RSS on the 1 GB
   OOM dump, more on less.php. This is reli's side, not ext-rdump's, but it is
   the practical ceiling for "capture in prod, analyse later." For first-pass
   triage on big dumps, reach for `--no-full-analysis` and `--memory-limit`.
2. **Capture worked on every shape tried** — AST-heavy (less.php), object-heavy
   (PhpSpreadsheet, Doctrine), string-heavy (HTMLPurifier), and mid-syscall at
   OOM — with no crashes and 94–98.5% analysed coverage. No correctness issues
   found in `ext-rdump` itself.
3. **Test gap closed.** Before this run, the phpt suite exercised
   `rdump_dump()` and the *return value* of `rdump_set_oom_dump()`, but nothing
   actually triggered the `memory_limit` path — the headline feature was
   untested. Added `tests/oom_dump.phpt` (below).

## Test added

`tests/oom_dump.phpt` spawns a child that exhausts a 16 MB `memory_limit` with
`rdump.oom_dump` set, then asserts from the parent that the extension
auto-wrote a well-formed RDUMP file plus its `.done` marker — i.e. that the OOM
hook fires end to end, not just that the setter returns `true`.
