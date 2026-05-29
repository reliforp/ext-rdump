--TEST--
rdump_dump() with rdump.safe_read=1 reads regions via /proc/self/mem
--SKIPIF--
<?php
if (!extension_loaded("rdump")) die("skip rdump not loaded");
if (!is_readable("/proc/self/mem")) die("skip /proc/self/mem not readable");
?>
--INI--
rdump.safe_read=1
--FILE--
<?php
// With safe_read on, regions are copied through /proc/self/mem instead of a
// direct dereference; the result must still be a well-formed RDUMP dump.
$path = tempnam(sys_get_temp_dir(), "rdump_safe_");
$payload = str_repeat("reli-safe-marker", 1000);

var_dump(rdump_dump($path));

$fp = fopen($path, "rb");
var_dump(fread($fp, 8) === "RDUMP\0\0\0");
var_dump(unpack("Vv", fread($fp, 4))["v"]);   // format version
fclose($fp);

var_dump(filesize($path) > 1024);
unlink($path);
?>
--EXPECT--
bool(true)
bool(true)
int(3)
bool(true)
