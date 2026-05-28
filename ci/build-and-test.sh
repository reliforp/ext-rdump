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
