--TEST--
rdump_dump() writes a well-formed RDUMP dump file
--SKIPIF--
<?php if (!extension_loaded("rdump")) die("skip rdump not loaded"); ?>
--FILE--
<?php
$path = tempnam(sys_get_temp_dir(), "rdump_test_");

// Keep some live data on the heap so there is something to capture.
$payload = str_repeat("reli-rdump-marker", 1000);

var_dump(rdump_dump($path));

$fp = fopen($path, "rb");
$magic = fread($fp, 8);
var_dump($magic === "RDUMP\0\0\0");

// format_version (uint32 LE) == 3
$format_version = unpack("Vv", fread($fp, 4))["v"];
var_dump($format_version);

// php_version string (uint32 len + bytes), e.g. "v84"
$len = unpack("Vv", fread($fp, 4))["v"];
$php_version = fread($fp, $len);
var_dump($php_version === "v" . PHP_MAJOR_VERSION . PHP_MINOR_VERSION);

// pid (int64 LE) == our pid
$pid = unpack("Pv", fread($fp, 8))["v"];
var_dump($pid === getmypid());

fclose($fp);
unlink($path);

// A non-writable path must fail gracefully (warning + false), not fatal.
var_dump(@rdump_dump("/this/path/does/not/exist/dump.rdump"));
?>
--EXPECTF--
bool(true)
bool(true)
int(3)
bool(true)
bool(true)
bool(false)
