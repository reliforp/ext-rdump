#!/bin/sh
# Integration test: a dump produced by ext-rdump must be readable by reli,
# the very tool it targets. Builds the extension, writes a dump, then has
# reli parse and analyse it.
#
# Run inside an official php:8.4-cli image with both trees mounted:
#   docker run --rm -v "$PWD/ext":/ext -v "$PWD/reli":/reli -w /ext \
#     php:8.4-cli sh ci/reli-integration.sh
#
# /ext  = this extension's source tree
# /reli = a checkout of reliforp/reli-prof
set -eux

# --- toolchain + the extensions reli itself needs --------------------
apt-get update
apt-get install -y --no-install-recommends $PHPIZE_DEPS git unzip libffi-dev
docker-php-ext-install -j"$(nproc)" ffi pcntl

# --- composer --------------------------------------------------------
php -r "copy('https://getcomposer.org/installer', '/tmp/composer-setup.php');"
php /tmp/composer-setup.php --install-dir=/usr/local/bin --filename=composer
rm -f /tmp/composer-setup.php

# --- build ext-rdump -------------------------------------------------
cd /ext
phpize
./configure --enable-rdump
make -j"$(nproc)"
SO="/ext/modules/rdump.so"
php -d extension="$SO" --ri rdump

# --- install reli ----------------------------------------------------
cd /reli
composer install --no-interaction --no-progress

# --- produce a dump with the extension -------------------------------
DUMP=/tmp/reli-integration.rdump
export DUMP
rm -f "$DUMP"
php -d extension="$SO" -r '
    // Keep distinctive live data on the heap so the analysis has substance.
    $marker = array_fill(0, 1000, str_repeat("reli-rdump-integration", 32));
    if (!rdump_dump(getenv("DUMP"), true)) {
        fwrite(STDERR, "rdump_dump() returned false\n");
        exit(1);
    }
'
test -s "$DUMP"

# --- reli must read it ----------------------------------------------
# FFI is enabled explicitly; reli wires FFI bindings at bootstrap even when
# reading a file rather than attaching to a live process.
INSPECT="$(php -d ffi.enable=1 reli inspector:memory:dump:inspect "$DUMP")"
echo "$INSPECT" | head -20
echo "$INSPECT" | grep -q 'Magic:.*RDUMP'      || { echo '::error::reli did not recognise the RDUMP magic'; exit 1; }
echo "$INSPECT" | grep -q 'Format Version:.*3'  || { echo '::error::reli read an unexpected format version'; exit 1; }

# Full analysis must also complete and emit a report. Capture stdout+stderr so
# a fatal/exception is visible in the CI log instead of a bare "no report".
if php -d ffi.enable=1 reli inspector:memory:analyze "$DUMP" -f report \
        >/tmp/reli-analyze.log 2>&1; then
    head -30 /tmp/reli-analyze.log
else
    echo '::error::reli analyze exited non-zero; full output:'
    cat /tmp/reli-analyze.log
    exit 1
fi
grep -q 'Memory Analysis Report' /tmp/reli-analyze.log || {
    echo '::error::reli analyze produced no report; full output:'
    cat /tmp/reli-analyze.log
    exit 1
}

echo "reli-integration OK"
