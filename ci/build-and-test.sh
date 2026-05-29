#!/bin/sh
# Build and test ext-rdump inside an official php:<ver>-{cli,zts} image.
#
# Run from the extension root with the source tree mounted, e.g.:
#   docker run --rm -v "$PWD":/ext -w /ext php:8.4-cli sh ci/build-and-test.sh
#
# Kept POSIX-sh so it runs unchanged on the oldest images (php:7.0) too.
set -eux

# --- build toolchain -------------------------------------------------
# Old Debian releases (jessie/stretch/buster) used by the 7.0-7.4 images
# have moved to archive.debian.org, so a plain `apt-get update` 404s.
# Fall back to the archive in that case. PHPIZE_DEPS (gcc, make, autoconf,
# ...) is set by the official php images.
APT_INSTALL_FLAGS=""
if ! apt-get update >/dev/null 2>&1; then
    cn=""
    if [ -r /etc/os-release ]; then
        # shellcheck disable=SC1091
        . /etc/os-release
        cn="${VERSION_CODENAME:-}"
        if [ -z "$cn" ]; then
            case "${VERSION_ID:-}" in
                8) cn=jessie ;;
                9) cn=stretch ;;
                10) cn=buster ;;
                11) cn=bullseye ;;
                12) cn=bookworm ;;
            esac
        fi
    fi
    echo "deb http://archive.debian.org/debian ${cn} main" > /etc/apt/sources.list
    # The archive's signing keys have long expired (EXPKEYSIG), so the
    # repo reads as unsigned. Permit the insecure archive explicitly --
    # acceptable for a throwaway CI build container.
    apt-get \
        -o Acquire::Check-Valid-Until=false \
        -o Acquire::AllowInsecureRepositories=true \
        update
    APT_INSTALL_FLAGS="--allow-unauthenticated"
fi
apt-get install -y --no-install-recommends $APT_INSTALL_FLAGS $PHPIZE_DEPS

php -v

# --- build -----------------------------------------------------------
phpize
./configure --enable-rdump
make -j"$(nproc)"

SO="$(pwd)/modules/rdump.so"
php -d extension="$SO" --ri rdump

# --- phpt suite (when run-tests.php is available) --------------------
if [ -f run-tests.php ]; then
    # Older run-tests.php (PHP 7.0-7.x) does not auto-detect the PHP
    # binary and aborts unless TEST_PHP_EXECUTABLE is set explicitly.
    NO_INTERACTION=1 \
    TEST_PHP_EXECUTABLE="$(command -v php)" \
    TEST_PHP_ARGS="-d extension=$SO" \
        php run-tests.php -q tests/ | tee /tmp/rdump-run-tests.log
    # Fail on reported test failures and on run-tests' own setup errors
    # (e.g. a missing executable) so the suite can't silently no-op.
    if grep -Eq 'FAIL|^ERROR:' /tmp/rdump-run-tests.log; then
        echo '::error::phpt suite reported failures'
        cat tests/*.diff 2>/dev/null || true
        exit 1
    fi
fi

# --- authoritative smoke gate (works identically on every version) ---
cat > /tmp/rdump-smoke.php <<'PHP'
<?php
$f = tempnam(sys_get_temp_dir(), "rdump_ci_");
if (!rdump_dump($f)) {
    fwrite(STDERR, "rdump_dump() returned false\n");
    exit(1);
}
$magic = file_get_contents($f, false, null, 0, 8);
if ($magic !== "RDUMP\x00\x00\x00") {
    fwrite(STDERR, "bad magic in dump file\n");
    exit(2);
}
if (filesize($f) < 1024) {
    fwrite(STDERR, "dump file unexpectedly small\n");
    exit(3);
}
unlink($f);
echo "smoke OK (zts=" . (PHP_ZTS ? "yes" : "no") . ")\n";
PHP
php -d extension="$SO" /tmp/rdump-smoke.php

# --- memory_limit auto-dump hook -------------------------------------
# Exercises the version-specific zend_error_cb signature at runtime and
# proves the C-level OOM hook fires where a shutdown handler cannot:
# pure recursion exhausts memory_limit via VM-stack growth, so even the
# shutdown closure's own frame can't be pushed.
OOM_DUMP=/tmp/rdump-oom.rdump
rm -f "$OOM_DUMP"
php -d extension="$SO" -d memory_limit=2M -d rdump.oom_dump="$OOM_DUMP" \
    -r 'function r(){ r(); } r();' >/dev/null 2>&1 || true
if [ -s "$OOM_DUMP" ] && [ "$(head -c 5 "$OOM_DUMP")" = "RDUMP" ]; then
    echo "oom-hook OK"
else
    echo "::error::rdump.oom_dump hook did not produce a dump"
    exit 1
fi

# Same, but enabled at runtime via rdump_set_oom_dump() (exercises the
# function path on every version).
OOM_DUMP_RT=/tmp/rdump-oom-rt.rdump
rm -f "$OOM_DUMP_RT"
php -d extension="$SO" -d memory_limit=2M \
    -r 'rdump_set_oom_dump("'"$OOM_DUMP_RT"'"); function r(){ r(); } r();' \
    >/dev/null 2>&1 || true
if [ -s "$OOM_DUMP_RT" ] && [ "$(head -c 5 "$OOM_DUMP_RT")" = "RDUMP" ]; then
    echo "oom-hook (runtime toggle) OK"
else
    echo "::error::rdump_set_oom_dump() did not produce a dump"
    exit 1
fi

# --- OOM dump path %p expansion --------------------------------------
# The auto-dump path expands %p to the PID. Run the OOMing child in the
# background so we know its PID, then require the dump named with that exact
# PID to exist.
EXPAND_DIR=/tmp/rdump-expand
rm -rf "$EXPAND_DIR"; mkdir -p "$EXPAND_DIR"
php -d extension="$SO" -d memory_limit=8M \
    -d rdump.oom_dump="$EXPAND_DIR/oom-%p.rdump" \
    -r 'function r(){ r(); } r();' >/dev/null 2>&1 &
EXPAND_PID=$!
wait "$EXPAND_PID" 2>/dev/null || true
if [ -s "$EXPAND_DIR/oom-$EXPAND_PID.rdump" ]; then
    echo "oom-expand (%p) OK"
else
    echo "::error::%p did not expand to the worker PID"
    ls "$EXPAND_DIR" || true
    exit 1
fi
rm -rf "$EXPAND_DIR"

# --- OOM auto-dump runaway guard (built-in server, multi-request) ----
# A one-shot CLI run can only OOM once (one process = one request), so it
# cannot show that rdump.oom_dump_max caps dumps over a worker's lifetime.
# The built-in server handles many requests in one process and survives each
# per-request fatal -- the same shape as an FPM worker -- so it can. We probe
# "did another dump get written?" by deleting the file between requests.
cat > /tmp/rdump-oom-app.php <<'PHP'
<?php $a = []; while (true) { $a[] = str_repeat("x", 100000); }
PHP

# One request, ignoring the 500 the OOM produces.
rdump_req() {
    php -r '$c = stream_context_create(["http" => ["ignore_errors" => true, "timeout" => 5]]);
            @file_get_contents("http://127.0.0.1:" . $argv[1] . "/", false, $c);' "$1"
    sleep 1   # let the synchronous in-request dump settle on disk
}
# Block until the server accepts connections (or give up).
rdump_wait_up() {
    i=0
    while [ "$i" -lt 50 ]; do
        if php -r '$f = @fsockopen("127.0.0.1", (int)$argv[1], $e, $s, 0.2);
                   exit($f ? (fclose($f) ? 0 : 0) : 1);' "$1"; then
            return 0
        fi
        i=$((i + 1)); sleep 0.1
    done
    echo "::error::built-in server did not come up"; return 1
}

CAP_DUMP=/tmp/rdump-cap.rdump

# Default cap of 1: a second OOM request must NOT re-create a deleted dump.
rm -f "$CAP_DUMP"
php -d extension="$SO" -d memory_limit=8M \
    -d rdump.oom_dump="$CAP_DUMP" -d rdump.oom_dump_max=1 \
    -S 127.0.0.1:8077 /tmp/rdump-oom-app.php >/tmp/rdump-srv.log 2>&1 &
SRV=$!
rdump_wait_up 8077 || { kill "$SRV" 2>/dev/null || true; exit 1; }
rdump_req 8077
if [ ! -s "$CAP_DUMP" ]; then
    echo "::error::first OOM request produced no dump"
    kill "$SRV" 2>/dev/null || true; exit 1
fi
rm -f "$CAP_DUMP"
rdump_req 8077
kill "$SRV" 2>/dev/null || true; wait "$SRV" 2>/dev/null || true
if [ -e "$CAP_DUMP" ]; then
    echo "::error::rdump.oom_dump_max=1 did not cap: a second dump was written"
    exit 1
fi
echo "oom-cap (max=1) OK"

# Control: unlimited (max=0) SHOULD re-create the deleted dump on request 2.
rm -f "$CAP_DUMP"
php -d extension="$SO" -d memory_limit=8M \
    -d rdump.oom_dump="$CAP_DUMP" -d rdump.oom_dump_max=0 \
    -S 127.0.0.1:8078 /tmp/rdump-oom-app.php >/tmp/rdump-srv.log 2>&1 &
SRV=$!
rdump_wait_up 8078 || { kill "$SRV" 2>/dev/null || true; exit 1; }
rdump_req 8078
rm -f "$CAP_DUMP"
rdump_req 8078
kill "$SRV" 2>/dev/null || true; wait "$SRV" 2>/dev/null || true
if [ ! -s "$CAP_DUMP" ]; then
    echo "::error::rdump.oom_dump_max=0 unexpectedly suppressed the second dump"
    exit 1
fi
echo "oom-cap (unlimited) OK"
rm -f "$CAP_DUMP"

# --- OOM auto-dump total-size budget --------------------------------
# Unlike the per-worker count cap, the byte budget bounds the *combined*
# footprint of every *.rdump in the dump directory, so a fleet of workers
# cannot fill the disk one dump each. The check also reserves the incoming
# dump's own minimum size (its ZendMM heap), so it skips before crossing the
# line, not only after. The directory is read at dump time, so a single CLI
# OOM tests it; sparse fillers (truncate) stand in for big dumps for free.
BUDGET_DIR=/tmp/rdump-budget
rm -rf "$BUDGET_DIR"; mkdir -p "$BUDGET_DIR"

rdump_run_oom() {
    php -d extension="$SO" -d memory_limit=8M \
        -d rdump.oom_dump="$BUDGET_DIR/oom-%p.rdump" \
        -d rdump.oom_dump_max_total="$1" \
        -r 'function r(){ r(); } r();' >/dev/null 2>&1 || true
}
rdump_have_dump() { ls "$BUDGET_DIR"/oom-*.rdump >/dev/null 2>&1; }

# Existing files already at/over the budget -> skip.
truncate -s 600M "$BUDGET_DIR/filler.rdump"
rdump_run_oom 512M
if rdump_have_dump; then
    echo "::error::rdump.oom_dump_max_total did not cap when existing files exceed it"
    exit 1
fi
echo "oom-budget (existing over) OK"
rm -f "$BUDGET_DIR"/oom-*.rdump

# Existing under the budget, but existing + the incoming dump's minimum tips
# over it -> skip. Proves the incoming-size reservation: 511M alone is < 512M.
truncate -s 511M "$BUDGET_DIR/filler.rdump"
rdump_run_oom 512M
if rdump_have_dump; then
    echo "::error::rdump.oom_dump_max_total ignored the incoming dump's size"
    exit 1
fi
echo "oom-budget (incoming tips over) OK"
rm -f "$BUDGET_DIR"/oom-*.rdump

# Comfortably under the budget -> written.
rm -f "$BUDGET_DIR/filler.rdump"
truncate -s 100M "$BUDGET_DIR/filler.rdump"
rdump_run_oom 512M
if ! rdump_have_dump; then
    echo "::error::rdump.oom_dump_max_total suppressed a dump well under the limit"
    exit 1
fi
echo "oom-budget (under) OK"
rm -rf "$BUDGET_DIR"

# --- OOM dump completion marker -------------------------------------
# With rdump.oom_dump_marker=1 a "<path>.done" must appear once the dump is
# fully written, so a directory watcher can wait for it instead of racing the
# half-written .rdump. A stale marker from a previous dump to the same path
# must be cleared first: pre-plant a non-empty .done and require it to come
# back empty (cleared, then re-created on completion), never left stale.
MARKER_DIR=/tmp/rdump-marker
rm -rf "$MARKER_DIR"; mkdir -p "$MARKER_DIR"
MARKER_DUMP="$MARKER_DIR/fixed.rdump"
printf STALE > "$MARKER_DUMP.done"
php -d extension="$SO" -d memory_limit=8M \
    -d rdump.oom_dump="$MARKER_DUMP" -d rdump.oom_dump_marker=1 \
    -r 'function r(){ r(); } r();' >/dev/null 2>&1 || true
if [ ! -f "$MARKER_DUMP.done" ]; then
    echo "::error::rdump.oom_dump_marker did not write a .done marker"
    exit 1
fi
if [ -s "$MARKER_DUMP.done" ]; then
    echo "::error::a stale .done marker survived a dump to the same path"
    exit 1
fi
echo "oom-marker OK"
rm -rf "$MARKER_DIR"

# --- OOM marker only on a successful dump ----------------------------
# A failed dump must not leave a completion marker. Point the dump at a
# directory so open() fails; the .done (creatable in the parent) must not
# appear.
FAILMARK_DIR=/tmp/rdump-failmarker
rm -rf "$FAILMARK_DIR"; mkdir -p "$FAILMARK_DIR/adir"
php -d extension="$SO" -d memory_limit=8M \
    -d rdump.oom_dump="$FAILMARK_DIR/adir" -d rdump.oom_dump_marker=1 \
    -r 'function r(){ r(); } r();' >/dev/null 2>&1 || true
if [ -e "$FAILMARK_DIR/adir.done" ]; then
    echo "::error::a .done marker was written for a failed dump"
    exit 1
fi
echo "oom-marker (none on failure) OK"
rm -rf "$FAILMARK_DIR"
