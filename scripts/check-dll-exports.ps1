# Verifies a built goradio.dll exports the six SA-MP plugin entry points,
# undecorated. The server resolves them by plain name, so a DLL that
# exports "_Supports@0" instead of "Supports" loads as "Failed." with no
# further explanation -- worth catching in CI rather than in someone's
# server log.
#
#   pwsh scripts/check-dll-exports.ps1 build/Release/goradio.dll
param(
	[Parameter(Mandatory = $true)][string]$Dll
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $Dll)) { throw "no such file: $Dll" }

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found; cannot locate dumpbin" }
$dumpbin = & $vswhere -latest -products * -find '**\dumpbin.exe' | Select-Object -First 1
if (-not $dumpbin) { throw "could not locate dumpbin.exe" }

# -join is load-bearing. dumpbin returns an *array* of lines, and
# PowerShell's -match/-notmatch on a collection filters the collection
# rather than returning a boolean -- so `$array -notmatch "x"` is truthy
# whenever any single line fails to match, which is always. Written that
# way the check reports every export as missing regardless of what the
# DLL actually contains.
$out = (& $dumpbin /exports $Dll) -join "`n"
Write-Host $out

$expected = @("Supports", "Load", "Unload", "AmxLoad", "AmxUnload", "ProcessTick")
$missing = @()
foreach ($fn in $expected) {
	if ($out -notmatch "\b$fn\b") { $missing += $fn }
}

if ($missing.Count -gt 0) {
	throw ("missing exports: {0}`n" -f ($missing -join ", ")) +
		"If the dump above shows decorated names like _Supports@0, then " +
		"sdk/goradio.def is not being applied by the linker."
}

Write-Host "all six entry points exported, undecorated"
