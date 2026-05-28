--TEST--
rdump_set_oom_dump() returns true for path / "" / null and accepts $full
--SKIPIF--
<?php if (!extension_loaded("rdump")) die("skip rdump not loaded"); ?>
--FILE--
<?php
// Enable with a (templated) path -> true.
var_dump(rdump_set_oom_dump("/tmp/rdump-oom-%p.rdump"));

// "" force-disables for the request -> true.
var_dump(rdump_set_oom_dump(""));

// null clears the override, falling back to the INI default -> true.
var_dump(rdump_set_oom_dump(null));

// $full is accepted.
var_dump(rdump_set_oom_dump("/tmp/rdump-oom.rdump", true));
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
