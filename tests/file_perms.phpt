--TEST--
rdump_dump() writes 0600 and refuses to follow a symlink
--SKIPIF--
<?php
if (!extension_loaded("rdump")) die("skip rdump not loaded");
if (DIRECTORY_SEPARATOR !== "/") die("skip POSIX-only");
?>
--FILE--
<?php
$dir = sys_get_temp_dir();
$pid = getmypid();

// 1) A freshly created dump is owner-only (0600).
$new = "$dir/rdump_perm_new_$pid.rdump";
@unlink($new);
var_dump(rdump_dump($new));
clearstatcache();
var_dump(decoct(fileperms($new) & 0777));

// 2) A pre-existing group/world-readable file is tightened back to 0600.
$pre = "$dir/rdump_perm_pre_$pid.rdump";
file_put_contents($pre, "stale");
chmod($pre, 0644);
var_dump(rdump_dump($pre));
clearstatcache();
var_dump(decoct(fileperms($pre) & 0777));

// 3) A symlink planted at the path is refused (O_NOFOLLOW), and its target
//    is left untouched -- no clobbering a file the symlink points at.
$target = "$dir/rdump_perm_target_$pid";
$link = "$dir/rdump_perm_link_$pid.rdump";
@unlink($target);
@unlink($link);
file_put_contents($target, "victim");
symlink($target, $link);
var_dump(@rdump_dump($link));
var_dump(file_get_contents($target) === "victim");

// 4) A non-regular target (here /dev/null, a char device) is refused, so a
//    FIFO/device/socket can't be written to (or hang) instead of a file.
var_dump(@rdump_dump("/dev/null"));

@unlink($new);
@unlink($pre);
@unlink($target);
@unlink($link);
?>
--EXPECT--
bool(true)
string(3) "600"
bool(true)
string(3) "600"
bool(false)
bool(true)
bool(false)
