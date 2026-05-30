--TEST--
rdump.oom_dump writes a well-formed RDUMP dump when memory_limit is exhausted
--SKIPIF--
<?php
if (!extension_loaded("rdump")) die("skip rdump not loaded");
if (PHP_OS_FAMILY !== "Linux") die("skip Linux only");
if (!is_file(dirname(__DIR__) . "/modules/rdump.so")) die("skip built modules/rdump.so not found");
if (!function_exists("shell_exec") || !shell_exec("echo ok")) die("skip shell_exec disabled");
?>
--FILE--
<?php
// A memory_limit error is fatal, so it can only be observed from a separate
// process: spawn a child that exhausts memory with rdump.oom_dump set, then
// verify from here that the extension auto-dumped at the moment of death.
$so   = dirname(__DIR__) . "/modules/rdump.so";
$php  = getenv("TEST_PHP_EXECUTABLE");
if ($php === false || $php === "") {
    $php = PHP_BINARY;
}

$dump  = sys_get_temp_dir() . "/rdump_oom_" . getmypid() . ".rdump";
$child = sys_get_temp_dir() . "/rdump_oom_child_" . getmypid() . ".php";
@unlink($dump);
@unlink($dump . ".done");
file_put_contents($child, '<?php $a = []; while (true) { $a[] = str_repeat("x", 4096); }');

$cmd = escapeshellarg($php)
     . " -n"
     . " -d extension=" . escapeshellarg($so)
     . " -d memory_limit=16M"
     . " -d rdump.oom_dump=" . escapeshellarg($dump)
     . " -d rdump.oom_dump_marker=1"
     . " " . escapeshellarg($child)
     . " 2>&1";
$output = (string) shell_exec($cmd);

// The child died on the memory_limit error...
var_dump(strpos($output, "Allowed memory size") !== false);

// ...and the OOM hook wrote the dump without any rdump_dump() call.
var_dump(is_file($dump));

// The dump is a well-formed RDUMP file.
var_dump(file_get_contents($dump, false, null, 0, 8) === "RDUMP\0\0\0");

// The completion marker was written (rdump.oom_dump_marker=1).
var_dump(is_file($dump . ".done"));

@unlink($dump);
@unlink($dump . ".done");
@unlink($child);
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
