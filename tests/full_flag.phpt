--TEST--
rdump_dump($path, true) captures more regions than the default
--SKIPIF--
<?php if (!extension_loaded("rdump")) die("skip rdump not loaded"); ?>
--FILE--
<?php
// Read the region_count field out of an RDUMP header. Layout (all LE):
//   magic[8], u32 format_version, str php_version,
//   u64 pid, u64 eg, u64 cg, u64 rss,
//   u32 module_globals_count, { str name, u64 addr } * count,
//   u32 entry_count, u32 region_count
function rdump_region_count(string $path): int
{
    $fp = fopen($path, "rb");
    fread($fp, 8);                                   // magic
    fread($fp, 4);                                   // format_version
    $len = unpack("Vv", fread($fp, 4))["v"];
    fread($fp, $len);                                // php_version
    fread($fp, 8 * 4);                               // pid, eg, cg, rss
    $modules = unpack("Vv", fread($fp, 4))["v"];     // module-globals count
    for ($i = 0; $i < $modules; $i++) {
        $l = unpack("Vv", fread($fp, 4))["v"];
        fread($fp, $l);                              // name
        fread($fp, 8);                               // addr
    }
    fread($fp, 4);                                   // entry_count
    $region_count = unpack("Vv", fread($fp, 4))["v"];
    fclose($fp);
    return $region_count;
}

$lean = tempnam(sys_get_temp_dir(), "rdump_lean_");
$full = tempnam(sys_get_temp_dir(), "rdump_full_");

var_dump(rdump_dump($lean, false));
var_dump(rdump_dump($full, true));

$lean_regions = rdump_region_count($lean);
$full_regions = rdump_region_count($full);

// The lean dump captures at least the volatile (writable/anon) regions...
var_dump($lean_regions > 0);
// ...and $full additionally embeds the read-only file-backed segments
// (binary/.so .text, .rodata, ...), so it must hold strictly more.
var_dump($full_regions > $lean_regions);

unlink($lean);
unlink($full);
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
