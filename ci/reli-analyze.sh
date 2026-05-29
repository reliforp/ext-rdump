#!/bin/sh
# Install reli (needs PHP 8.4+) once, then analyse every dump in $IN_DIR. The
# dumps may have been produced by ext-rdump on older PHP versions; this proves
# current reli can inspect/analyze cross-version RDUMP files. Setting reli up
# (FFI build + composer) is the fixed cost, so it's paid once and amortised over
# all dumps rather than per dump.
#
#   docker run --rm -e IN_DIR=/in -v "$PWD/reli":/reli -v "$PWD/out":/in \
#     -w /reli php:8.4-cli sh /ext/ci/reli-analyze.sh
set -eu

: "${IN_DIR:?set IN_DIR to the directory of dumps to analyse}"

apt-get update
apt-get install -y --no-install-recommends git unzip libffi-dev
docker-php-ext-install -j"$(nproc)" ffi pcntl

php -r "copy('https://getcomposer.org/installer', '/tmp/composer-setup.php');"
php /tmp/composer-setup.php --install-dir=/usr/local/bin --filename=composer
rm -f /tmp/composer-setup.php

composer install --no-interaction --no-progress

analyze_one() {
    dump="$1"
    # Derive the expected origin tag from the filename (php7.0-cli.rdump -> v70).
    tag="v$(basename "$dump" | sed -E 's/^php([0-9])\.([0-9]).*/\1\2/')"
    echo "::group::analyze $(basename "$dump") (origin $tag)"
    test -s "$dump" || { echo "::error::missing/empty dump $dump"; return 1; }

    # The dump records its origin PHP version in the header, so no
    # --php-version override is needed for a file dump.
    inspect="$(php -d ffi.enable=1 reli inspector:memory:dump:inspect "$dump")"
    echo "$inspect" | head -20
    echo "$inspect" | grep -q 'Magic:.*RDUMP'      || { echo "::error::$dump: no RDUMP magic"; return 1; }
    echo "$inspect" | grep -q 'Format Version:.*3'  || { echo "::error::$dump: unexpected format version"; return 1; }
    echo "$inspect" | grep -qi "PHP Version:.*$tag" || { echo "::error::$dump: origin not $tag"; return 1; }

    if php -d ffi.enable=1 reli inspector:memory:analyze "$dump" -f report \
            >/tmp/reli-analyze.log 2>&1; then
        head -20 /tmp/reli-analyze.log
    else
        echo "::error::$dump: reli analyze exited non-zero; full output:"
        cat /tmp/reli-analyze.log
        return 1
    fi
    grep -q 'Memory Analysis Report' /tmp/reli-analyze.log || {
        echo "::error::$dump: no report produced; full output:"
        cat /tmp/reli-analyze.log
        return 1
    }
    echo "OK: $(basename "$dump")"
    echo "::endgroup::"
}

rc=0
n=0
for d in "$IN_DIR"/*.rdump; do
    n=$((n + 1))
    analyze_one "$d" || rc=1
done

if [ "$n" -eq 0 ]; then
    echo "::error::no dumps found in $IN_DIR"
    exit 1
fi
if [ "$rc" -ne 0 ]; then
    echo "::error::one or more cross-version dumps failed to analyse"
    exit 1
fi
echo "reli cross-version analyze OK ($n dumps)"
