# Tests the export check in scripts/check-dll-exports.ps1 against captured
# dumpbin output, so it can run anywhere PowerShell does -- no Windows, no
# Visual Studio, no DLL.
#
#   pwsh test/test-dll-exports.ps1
$ErrorActionPreference = "Stop"

. "$PSScriptRoot/../scripts/check-dll-exports.ps1"

$failures = 0
function Check([bool]$ok, [string]$what) {
	if ($ok) { Write-Host "  ok   $what" }
	else { Write-Host "  FAIL $what"; $script:failures++ }
}

# What dumpbin actually prints for a DLL built with sdk/goradio.def
# applied: six undecorated names, wrapped in a lot of other text.
$goodLines = @(
	'Microsoft (R) COFF/PE Dumper Version 14.38.33133.0'
	'Copyright (C) Microsoft Corporation.  All rights reserved.'
	''
	'Dump of file build\Release\goradio.dll'
	''
	'File Type: DLL'
	''
	'  Section contains the following exports for goradio.dll'
	''
	'           6 number of functions'
	'           6 number of names'
	''
	'    ordinal hint RVA      name'
	''
	'          1    0 00011000 AmxLoad'
	'          2    1 00011010 AmxUnload'
	'          3    2 00011020 Load'
	'          4    3 00011030 ProcessTick'
	'          5    4 00011040 Supports'
	'          6    5 00011050 Unload'
	''
	'  Summary'
	'        1000 .data'
)
$good = $goodLines -join "`n"

Write-Host "a good dump reports nothing missing"
Check ((@(Get-MissingGoradioExports -DumpOutput $good)).Count -eq 0) "all six found"

# The regression test for the bug this shipped with. Handing the function
# the raw *array* -- exactly what dumpbin returns -- must fail loudly at
# the parameter, not slip through to -notmatch where a collection operand
# silently turns the comparison into a filter and every export looks
# missing.
Write-Host "an array operand is rejected, not silently filtered"
$threw = $false
try { Get-MissingGoradioExports -DumpOutput $goodLines | Out-Null }
catch { $threw = $true }
Check $threw "passing raw dumpbin lines throws rather than misreporting"

# And the shape the script itself uses -- joined first -- is correct.
Check ((@(Get-MissingGoradioExports -DumpOutput ($goodLines -join "`n"))).Count -eq 0) `
	"the joined form the script uses reports nothing missing"

Write-Host "decorated names are reported as missing"
$decorated = ($goodLines | ForEach-Object {
	$_ -replace '\b(AmxLoad|AmxUnload|Load|ProcessTick|Supports|Unload)\b', '_$1@0'
}) -join "`n"
Check ((@(Get-MissingGoradioExports -DumpOutput $decorated)).Count -eq 6) `
	"a DLL exporting _Supports@0 fails the check"

Write-Host "a partial export table names only what is absent"
$partial = ($goodLines | Where-Object { $_ -notmatch 'ProcessTick' }) -join "`n"
$missing = @(Get-MissingGoradioExports -DumpOutput $partial)
Check ($missing.Count -eq 1 -and $missing[0] -eq 'ProcessTick') "only ProcessTick reported"

Write-Host "empty output fails rather than passing vacuously"
Check ((@(Get-MissingGoradioExports -DumpOutput "")).Count -eq 6) "empty dump reports all six"

Write-Host ""
if ($failures -gt 0) { Write-Host "$failures failure(s)"; exit 1 }
Write-Host "all export-check tests passed"
