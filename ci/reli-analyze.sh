#!/bin/sh
# Install reli (needs PHP 8.4+) in this image and analyse a pre-made dump at
# $IN_DUMP -- which may have been produced by ext-rdump on an older PHP. Proves
# current reli can inspect/analyze a cross-version RDUMP file.
#
#   docker run --rm -e IN_DUMP=/in/d.rdump -e TAG=v74 -v "$PWD/reli":/reli \
#     -v "$PWD/out":/in -w /reli php:8.4-cli sh ci/reli-analyze.sh
set -eux

: "${IN_DUMP:?set IN_DUMP to the dump file to analyse}"
test -s "$IN_DUMP"

apt-get update
apt-get install -y --no-install-recommends git unzip libffi-dev
docker-php-ext-install -j"$(nproc)" ffi pcntl

php -r "copy('https://getcomposer.org/installer', '/tmp/composer-setup.php');"
php /tmp/composer-setup.php --install-dir=/usr/local/bin --filename=composer
rm -f /tmp/composer-setup.php

composer install --no-interaction --no-progress

# The dump records its origin PHP version (e.g. v70/v74); reli reads that from
# the header, so no --php-version override is needed for a file dump.
INSPECT="$(php -d ffi.enable=1 reli inspector:memory:dump:inspect "$IN_DUMP")"
echo "$INSPECT" | head -20
echo "$INSPECT" | grep -q 'Magic:.*RDUMP'     || { echo '::error::reli did not recognise the RDUMP magic'; exit 1; }
echo "$INSPECT" | grep -q 'Format Version:.*3' || { echo '::error::unexpected format version'; exit 1; }
if [ -n "${TAG:-}" ]; then
    echo "$INSPECT" | grep -qi "PHP Version:.*$TAG" \
        || { echo "::error::dump did not report origin PHP $TAG"; exit 1; }
fi

if php -d ffi.enable=1 reli inspector:memory:analyze "$IN_DUMP" -f report \
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

echo "reli cross-version analyze OK (dump=$IN_DUMP tag=${TAG:-n/a})"
