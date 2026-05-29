#!/bin/sh
# Leak check for ext-rdump under valgrind.
#
# AddressSanitizer is unsuitable here: ASan instruments the same process
# whose entire address space this extension dumps, so reading ASan's own
# shadow-memory regions (present in /proc/self/maps under ASan) aborts in
# the fwrite interceptor before leak detection even runs. valgrind, by
# contrast, completes the full dump path.
#
# valgrind also prints "Invalid read" / "uninitialised write" notes for the
# region copy -- those are inherent to dumping raw process memory (page
# tails mapped past the brk, never-written bytes) and are NOT leaks, so we
# gate purely on the leak summary's lost-block counts.
set -eux

apt-get update
apt-get install -y --no-install-recommends $PHPIZE_DEPS valgrind

phpize
./configure --enable-rdump
make -j"$(nproc)"
SO="$(pwd)/modules/rdump.so"

cat > /tmp/leak.php <<'PHP'
<?php
// Exercise the alloc/free paths repeatedly so any per-call leak accumulates.
$f = tempnam(sys_get_temp_dir(), "rdump_leak_");
for ($i = 0; $i < 3; $i++) {
    rdump_dump($f);
}
@unlink($f);
echo "done\n";
PHP

# USE_ZEND_ALLOC=0 routes the Zend MM through plain malloc so every
# allocation (PHP's and ours) is visible to valgrind.
USE_ZEND_ALLOC=0 valgrind \
    --leak-check=full \
    --show-leak-kinds=definite,indirect,possible \
    --log-file=/tmp/valgrind.log \
    php -n -d extension="$SO" /tmp/leak.php

echo '=== valgrind leak summary ==='
grep -E '(definitely|indirectly|possibly) lost:' /tmp/valgrind.log || true

if grep -E '(definitely|indirectly|possibly) lost:' /tmp/valgrind.log \
        | grep -vq '0 bytes in 0 blocks'; then
    echo '::error::valgrind reported leaked blocks'
    cat /tmp/valgrind.log
    exit 1
fi
echo 'no leaks'
