--TEST--
rdump_dump() replaces an existing file with a fresh inode (no read-fd leak)
--SKIPIF--
<?php
if (!extension_loaded("rdump")) die("skip rdump not loaded");
if (DIRECTORY_SEPARATOR !== "/") die("skip POSIX-only");
?>
--FILE--
<?php
$p = sys_get_temp_dir() . "/rdump_inode_" . getmypid() . ".rdump";
@unlink($p);

// First dump, then hold a read fd on that inode.
var_dump(rdump_dump($p));
$fd = fopen($p, "rb");
$ino1 = fileinode($p);

// A second dump to the same path must land on a NEW inode -- the old one is
// pinned by $fd, so a process still holding a read fd never sees the new
// (secret) dump; it keeps reading the old content.
var_dump(rdump_dump($p));
clearstatcache();
$ino2 = fileinode($p);
var_dump($ino1 !== $ino2);
var_dump(fread($fd, 5) === "RDUMP");

fclose($fd);
@unlink($p);
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
