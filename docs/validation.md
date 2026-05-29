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
reconstructs the exact call stack at the moment of death.

## Environment

| | |
|---|---|
| PHP | 8.4.19 (NTS), x86-64 Linux |
| ext-rdump | this branch, `phpize && ./configure --enable-rdump && make` |
| reli | `reliforp/reli-prof` 0.13.x-dev |
| Pipeline | `rdump_dump()` / OOM auto-dump → `inspector:memory:analyze -f rmem` → `inspector:memory:report` |

`ext-rdump`'s own test suite (now 7 tests, see [below](#test-added)) is green.

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
