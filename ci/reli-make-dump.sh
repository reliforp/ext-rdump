#!/bin/sh
# Build ext-rdump in the current (possibly old, 7.x) PHP image and write a
# self-contained dump to $OUT_DUMP. Paired with ci/reli-analyze.sh, which runs
# in a PHP 8.4 image (reli needs 8.4+) and reads the dump: this proves a dump
# produced on an old PHP is still analysable by current reli.
#
#   docker run --rm -e OUT_DUMP=/out/d.rdump -v "$PWD":/ext -v "$PWD/out":/out \
#     -w /ext php:7.4-cli sh ci/reli-make-dump.sh
set -eux

: "${OUT_DUMP:?set OUT_DUMP to the output path}"

# Old Debian releases behind the 7.x images have moved to archive.debian.org,
# so a plain apt-get update can 404; fall back to the archive in that case.
if ! apt-get update >/dev/null 2>&1; then
    cn=""
    if [ -r /etc/os-release ]; then
        # shellcheck disable=SC1091
        . /etc/os-release
        cn="${VERSION_CODENAME:-}"
        # stretch's os-release has no VERSION_CODENAME, so map from VERSION_ID;
        # an empty codename produces a malformed sources.list line.
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
    apt-get -o Acquire::Check-Valid-Until=false \
            -o Acquire::AllowInsecureRepositories=true update
    apt-get install -y --no-install-recommends --allow-unauthenticated $PHPIZE_DEPS
else
    apt-get install -y --no-install-recommends $PHPIZE_DEPS
fi

php -v

phpize
./configure --enable-rdump
make -j"$(nproc)"
SO="$(pwd)/modules/rdump.so"
php -d extension="$SO" --ri rdump | grep -i 'target php version'

# full=true so the dump embeds the read-only segments of *this* old PHP build,
# letting reli on another host/version analyse it without the original binaries.
rm -f "$OUT_DUMP"
php -d extension="$SO" -r '
    $marker = array_fill(0, 1000, str_repeat("reli-rdump-xver", 32));
    if (!rdump_dump(getenv("OUT_DUMP"), true)) {
        fwrite(STDERR, "rdump_dump() returned false\n");
        exit(1);
    }
'
test -s "$OUT_DUMP"
echo "wrote $(wc -c < "$OUT_DUMP") bytes to $OUT_DUMP"
