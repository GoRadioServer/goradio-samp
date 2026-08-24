# Verifies a built goradio.dll exports the six SA-MP plugin entry points,
# undecorated. The server resolves them by plain name, so a DLL that
# exports "_Supports@0" instead of "Supports" loads as a bare "Failed."
# with nothing else said -- worth catching in CI rather than in someone's
# server log.
#
#   pwsh scripts/check-dll-exports.ps1 -Dll build/Release/goradio.dll
#
# -DumpText substitutes captured dumpbin output for the real thing, which
# is how test/test-dll-exports.ps1 exercises this on a machine with
# neither Windows nor a DLL.
param(
	[string]$Dll,
	[string]$DumpText
)

$ErrorActionPreference = "Stop"

# The parameter is typed [string] deliberately, and that is the defence
# against the bug this check shipped with. dumpbin returns an *array* of
# lines, and PowerShell's -match/-notmatch on a collection filters the
# collection rather than returning a boolean -- so
# `$lines -notmatch "Supports"` yields every line lacking the word, which
# is a non-empty (truthy) array, and the check reports a missing export
# against a perfectly good DLL.
#
# PowerShell will not convert Object[] to String for a typed parameter,
# so handing this the raw dumpbin output fails immediately and loudly
# ("Cannot convert value to type System.String") instead of quietly
# producing a wrong answer. Callers must join the lines themselves.
function Get-MissingGoradioExports {
	param(
		[Parameter(Mandatory = $true)]
		[AllowEmptyString()]
		[string]$DumpOutput
	)

	$expected = @("Supports", "Load", "Unload", "AmxLoad", "AmxUnload", "ProcessTick")
	$missing = @()
	foreach ($fn in $expected) {
		if ($DumpOutput -notmatch "\b$fn\b") { $missing += $fn }
	}
	# Returned bare, not as `,$missing`: the caller wraps the result in
	# @(), and the unary comma would nest an array inside that instead of
	# preserving it.
	return $missing
}

# Dot-sourced by the tests, which want the function but not the run.
if (-not $Dll -and -not $DumpText) { return }

if (-not $DumpText) {
	if (-not (Test-Path $Dll)) { throw "no such file: $Dll" }

	$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
	if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found; cannot locate dumpbin" }
	$dumpbin = & $vswhere -latest -products * -find '**\dumpbin.exe' | Select-Object -First 1
	if (-not $dumpbin) { throw "could not locate dumpbin.exe" }

	$DumpText = (& $dumpbin /exports $Dll) -join "`n"
	Write-Host $DumpText
}

$missing = @(Get-MissingGoradioExports -DumpOutput $DumpText)

if ($missing.Count -gt 0) {
	throw ("missing exports: {0}`n" -f ($missing -join ", ")) +
		"If the dump above shows decorated names like _Supports@0, then " +
		"sdk/goradio.def is not being applied by the linker."
}

Write-Host "all six entry points exported, undecorated"
