# CommandCapture - capture every command a script or executable spawns.
#
# Copyright (c) 2026 maijun
#
# SPDX-License-Identifier: MIT
#
# Released under the MIT License. See the LICENSE file at the repository
# root for the full text.

﻿# CommandCapture regression tests (Windows / PowerShell)
#
# Scenarios covered:
#   1. cmd builtin commands (only visible via static analysis)
#   2. Direct child processes
#   3. Pipes (cmd.exe starts both sides)
#   4. Nested scripts -> third level
#   5. Redirection / short-lived processes
#   6. cmake / ninja / gcc toolchain
#   7. msbuild
#   8. Output formats
#
# Usage: powershell -File tests/run_tests.ps1 [-Ccap <path>]

param(
    [string]$Ccap = ""
)

$ErrorActionPreference = 'Stop'

if (-not $Ccap) {
    $here = Split-Path -Parent $MyInvocation.MyCommand.Path
    $root = Split-Path -Parent $here
    foreach ($cand in @("$root\build\Release\ccap.exe", "$root\build\ccap.exe", "$root\build\Debug\ccap.exe")) {
        if (Test-Path $cand) { $Ccap = $cand; break }
    }
}
if (-not $Ccap -or -not (Test-Path $Ccap)) {
    Write-Error "ccap executable not found, please specify it with -Ccap"
    exit 2
}

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$fixtures = Join-Path $here 'fixtures'
$tmp = Join-Path $env:TEMP ("ccap_test_" + [Guid]::NewGuid().ToString('N').Substring(0,8))
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

$script:pass = 0
$script:fail = 0

function Check([string]$name, [bool]$ok, [string]$detail = "") {
    if ($ok) {
        Write-Host ("ok   " + $name) -ForegroundColor Green
        $script:pass++
    } else {
        Write-Host ("FAIL " + $name + "  " + $detail) -ForegroundColor Red
        $script:fail++
    }
}

function CountRuntime([string]$file) {
    if (-not (Test-Path $file)) { return 0 }
    return (Select-String -Path $file -Pattern '"source":"runtime"' -AllMatches).Count
}
function CountStatic([string]$file) {
    if (-not (Test-Path $file)) { return 0 }
    return (Select-String -Path $file -Pattern '"source":"static"' -AllMatches).Count
}
function CountExe([string]$file, [string]$exe) {
    if (-not (Test-Path $file)) { return 0 }
    return (Select-String -Path $file -Pattern ([regex]::Escape('"executable":"' + $exe + '"')) -AllMatches).Count
}
function HasPattern([string]$file, [string]$pat) {
    if (-not (Test-Path $file)) { return $false }
    $m = @(Select-String -Path $file -Pattern $pat -Quiet)
    if ($m.Count -eq 0) { return $false }
    return [bool]($m[0])
}

Write-Host "=== CommandCapture regression tests ==="
Write-Host "ccap: $Ccap"
Write-Host ""

# ---------------------------------------------------------------- 1. cmd script
Write-Host "--- 1. cmd script (builtin + child process + pipe + nesting) ---"
$f = Join-Path $tmp 'build.ndjson'
& $Ccap run (Join-Path $fixtures 'build.bat') --format ndjson --out $f *> $null

$rt = CountRuntime $f
$st = CountStatic $f
Check "runtime event count >= 8" ($rt -ge 8) "actual=$rt"
Check "static event count >= 10" ($st -ge 10) "actual=$st"
Check "captured whoami" ((CountExe $f 'whoami') -ge 1)
Check "captured findstr (right side of the pipe)" ((CountExe $f 'findstr') -ge 1)
Check "captured hostname (nested script, >=2)" ((CountExe $f 'hostname') -ge 2)
Check "static layer captured the echo builtin" ((CountExe $f 'echo') -ge 1)
Check "an event with depth=2 exists" (HasPattern $f '"depth":2')
Check "a root event with depth=0 exists" (HasPattern $f '"depth":0')
# Key regression: backslash paths used to be parsed as C:WINDOWSsystem32cmd.exe
Check "the executable name is not truncated at the backslashes" (-not (HasPattern $f '"executable":"[A-Z]:[A-Z]+'))

# ---------------------------------------------------------------- 2. Executable file
Write-Host ""
Write-Host "--- 2. Running an executable directly ---"
$f2 = Join-Path $tmp 'exe.ndjson'
$sys = Join-Path $env:SystemRoot 'System32\cmd.exe'
& $Ccap run $sys '/c' 'exit 0' --format ndjson --out $f2 *> $null
Check "captured cmd.exe" ((CountExe $f2 'cmd.exe') -ge 1)
Check "runtime event count >= 1" ((CountRuntime $f2) -ge 1)

# ---------------------------------------------------------------- 3. Static analysis
Write-Host ""
Write-Host "--- 3. Static analysis only (scan) ---"
$f3 = Join-Path $tmp 'scan.ndjson'
& $Ccap scan (Join-Path $fixtures 'build.bat') --format ndjson --out $f3 *> $null
Check "scan produces static events only" (-not (HasPattern $f3 '"source":"runtime"'))
Check "scan event count >= 10" ((CountStatic $f3) -ge 10)
Check "scan marks builtins" (HasPattern $f3 '"is_builtin":true')
Check "scan records the script line number" (HasPattern $f3 '"script_line":[1-9]')

# ---------------------------------------------------------------- 4. JSON format
Write-Host ""
Write-Host "--- 4. Output format ---"
$f4 = Join-Path $tmp 'out.json'
& $Ccap run (Join-Path $fixtures 'build.bat') --format json --out $f4 *> $null
$jsonOk = $false
try {
    $null = Get-Content $f4 -Raw -Encoding UTF8 | ConvertFrom-Json
    $jsonOk = $true
} catch { $jsonOk = $false }
Check "the JSON format is valid" $jsonOk
Check "the JSON contains an events array" (HasPattern $f4 '"events"')

# ---------------------------------------------------------------- 5. cmake toolchain
Write-Host ""
Write-Host "--- 5. cmake / ninja / gcc toolchain ---"
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if ($cmake) {
    $f5 = Join-Path $tmp 'cmake.ndjson'
    $cmbuild = Join-Path $tmp 'cmbuild'
    & $Ccap run $cmake '-S' (Join-Path $fixtures 'cmake_proj') '-B' $cmbuild --format ndjson --out $f5 *> $null
    $rt5 = CountRuntime $f5
    Check "cmake captured child processes" ($rt5 -ge 3) "actual=$rt5"
    Check "cmake tree depth >= 3" (HasPattern $f5 '"depth":3')
    Check "captured cc1plus (the real compiler)" ((CountExe $f5 'cc1plus.exe') -ge 1)
} else {
    Write-Host "skip cmake (not installed)"
}

# ---------------------------------------------------------------- 6. msbuild
Write-Host ""
Write-Host "--- 6. msbuild (32-bit WOW64 target) ---"
$msbuild = 'D:\development\MicrosoftVisualStudio\2022\Community\MSBuild\Current\Bin\MSBuild.exe'
if (Test-Path $msbuild) {
    $f6 = Join-Path $tmp 'msbuild.ndjson'
    & $Ccap run $msbuild (Join-Path $fixtures 'msbuild_proj\demo.proj') '/t:Build' '/nologo' '/v:minimal' --format ndjson --out $f6 *> $null
    $rt6 = CountRuntime $f6
    Check "msbuild captured child processes" ($rt6 -ge 2) "actual=$rt6"
    # The command line of a 32-bit process must be readable from the PEB
    # (it used to be read as garbage)
    Check "the 32-bit process command line is readable" (HasPattern $f6 'MSBuild\.exe')
} else {
    Write-Host "skip msbuild (not installed)"
}

# ========================================================================
# 6b. The Windows build tools, one by one
#
# The point of this section is coverage of the *build drivers* that actually
# spawn compilers on Windows, not of any single compiler. Each entry drives
# a different way of getting from "a project directory" to "a cl.exe / g++
# invocation", and each one has its own process shape.
# ========================================================================
Write-Host ""
Write-Host "--- 6b. Windows build drivers ---"

# ── cmake + Ninja generator ───────────────────────────────────────────
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
$ninja = (Get-Command ninja -ErrorAction SilentlyContinue).Source
if ($cmake -and $ninja) {
    $f6b1 = Join-Path $tmp 'cmake_ninja.ndjson'
    $nb = Join-Path $tmp 'nb'
    & $Ccap run $cmake '-G' 'Ninja' '-S' (Join-Path $fixtures 'cmake_ninja_proj') '-B' $nb --format ndjson --out $f6b1 *> $null
    $rt = CountRuntime $f6b1
    Check "cmake + Ninja captured child processes" ($rt -ge 3) "actual=$rt"
    Check "cmake + Ninja reached the compiler" ((CountExe $f6b1 'cc1plus.exe') -ge 1)
    Check "cmake + Ninja build tree is deep enough" (HasPattern $f6b1 '"depth":[2-9]')
} else {
    Write-Host "skip cmake + Ninja (cmake or ninja not installed)"
}

# ── Ninja invoked directly (no cmake in the loop) ─────────────────────
if ($ninja) {
    $f6b2 = Join-Path $tmp 'ninja_direct.ndjson'
    $nb2 = Join-Path $tmp 'nb2'
    # Generate the build.ninja with cmake first, then run ninja directly, so
    # the captured tree starts at ninja rather than at cmake.
    & $cmake '-G' 'Ninja' '-S' (Join-Path $fixtures 'cmake_ninja_proj') '-B' $nb2 *> $null
    if (Test-Path (Join-Path $nb2 'build.ninja')) {
        & $Ccap run $ninja '-C' $nb2 --format ndjson --out $f6b2 *> $null
        $rt = CountRuntime $f6b2
        Check "ninja invoked directly captured child processes" ($rt -ge 2) "actual=$rt"
        Check "ninja reached the compiler" ((CountExe $f6b2 'cc1plus.exe') -ge 1)
        # ninja -> cmd.exe /C -> c++.exe -> c++ -> cc1plus: five levels.
        Check "ninja build tree is deep enough" (HasPattern $f6b2 '"depth":[3-9]')
    } else {
        Write-Host "skip ninja direct (build.ninja not generated)"
    }
} else {
    Write-Host "skip ninja direct (ninja not installed)"
}

# ── GNU make (the sh-based toolchain) ─────────────────────────────────
$make = (Get-Command make -ErrorAction SilentlyContinue).Source
$gpp = (Get-Command g++ -ErrorAction SilentlyContinue).Source
if ($make -and $gpp) {
    $f6b3 = Join-Path $tmp 'make.ndjson'
    & $Ccap run $make '-C' (Join-Path $fixtures 'make_proj') --format ndjson --out $f6b3 *> $null
    $rt = CountRuntime $f6b3
    Check "make captured child processes" ($rt -ge 2) "actual=$rt"
    Check "make reached the compiler" ((CountExe $f6b3 'cc1plus.exe') -ge 1)
    # make -> sh -c -> g++ -> cc1plus
    Check "make build tree is deep enough" (HasPattern $f6b3 '"depth":[2-9]')
} else {
    Write-Host "skip make (make or g++ not installed)"
}

# ── MSVC: cl.exe driven directly ──────────────────────────────────────
#
# This is the case that matters most for the PEB reading: cl.exe is a
# 64-bit process here, and the command line has to come back intact.
$clExe = $null
$vcvars = 'D:\development\MicrosoftVisualStudio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
if (Test-Path $vcvars) {
    $found = cmd /c "call `"$vcvars`" >nul 2>&1 && where cl"
    if ($found -match '([A-Za-z]:\\[^\r\n]*cl\.exe)') { $clExe = $Matches[1] }
}
if ($clExe) {
    $f6b4 = Join-Path $tmp 'cl.ndjson'
    $clSrc = Join-Path $tmp 'clsrc'
    New-Item -ItemType Directory -Force -Path $clSrc | Out-Null
    Set-Content -Path (Join-Path $clSrc 'main.cpp') -Encoding UTF8 -Value '#include <cstdio>' -NoNewline
    Add-Content -Path (Join-Path $clSrc 'main.cpp') -Encoding UTF8 -Value 'int main(){return 0;}'
    # Run cl.exe under the MSVC environment.
    $clCmd = "call `"$vcvars`" >nul 2>&1 && cl /nologo /c /EHsc /std:c++17 main.cpp"
    & cmd /c "`"$clCmd`"" 2>&1 | Out-Null
    $clArgs = "/c `"$clCmd`""
    & $Ccap run 'cmd.exe' '/c' $clCmd --format ndjson --out $f6b4 *> $null
    $rt = CountRuntime $f6b4
    Check "cl.exe captured child processes" ($rt -ge 2) "actual=$rt"
    Check "cl.exe command line is readable" (HasPattern $f6b4 'cl\.exe')
    Check "the compile command carries the MSVC switches" (HasPattern $f6b4 '/EHsc|/std:c\+\+17')
} else {
    Write-Host "skip cl.exe direct (vcvars64 not found)"
}

# ── MSBuild: find every installed version, not just one hardcoded path ─
Write-Host ""
Write-Host "--- 6c. Every installed MSBuild ---"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$msbuilds = @()
if (Test-Path $vswhere) {
    $msbuilds = & $vswhere -products * -requires Microsoft.Component.MSBuild `
        -find 'MSBuild\**\Bin\MSBuild.exe' 2>$null
}
if (-not $msbuilds) {
    # Fall back to the well-known layout.
    $msbuilds = Get-ChildItem 'D:\development\MicrosoftVisualStudio' -Recurse -Filter 'MSBuild.exe' `
        -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match 'MSBuild\\Current\\Bin' } |
        Select-Object -ExpandProperty FullName
}
if ($msbuilds -and $msbuilds.Count -gt 0) {
    $i = 0
    foreach ($mb in $msbuilds) {
        $i++
        $f6c = Join-Path $tmp ("msbuild_$i.ndjson")
        & $Ccap run $mb (Join-Path $fixtures 'msbuild_proj\demo.proj') '/t:Build' '/nologo' '/v:minimal' --format ndjson --out $f6c *> $null
        $rt = CountRuntime $f6c
        $ver = (& $mb -version -nologo 2>$null | Select-Object -First 1)
        Check "MSBuild [$ver] captured child processes" ($rt -ge 2) "actual=$rt"
        Check "MSBuild [$ver] command line is readable" (HasPattern $f6c 'MSBuild\.exe')
    }
} else {
    Write-Host "skip MSBuild versions (none found)"
}

# ---------------------------------------------------------------- 7. Timeout
Write-Host ""
Write-Host "--- 7. Timeout handling ---"
$f7 = Join-Path $tmp 'timeout.ndjson'
& $Ccap run $sys '/c' 'ping -n 20 127.0.0.1' --timeout 2000 --format ndjson --out $f7 *> $null
Check "a result is still produced after the timeout" (Test-Path $f7)

# The timeout argument must be respected —— early versions still waited an
# unconditional 5 seconds after the timeout, so --timeout 2000 actually
# took 7 seconds.
$sw7 = [System.Diagnostics.Stopwatch]::StartNew()
& $Ccap run $sys '/c' 'ping -n 30 127.0.0.1' --timeout 2000 --format json --out (Join-Path $tmp 'to2.json') *> $null
$sw7.Stop()
Check "the timeout argument is respected (2000ms -> < 4000ms)" ($sw7.ElapsedMilliseconds -lt 4000) "actual=$($sw7.ElapsedMilliseconds)ms"

$toJson = Get-Content (Join-Path $tmp 'to2.json') -Raw -Encoding UTF8 | ConvertFrom-Json
Check "the timeout is flagged" ($toJson.timed_out -eq $true)
Check "the notes say PARTIAL after the timeout" (($toJson.notes -join ' ') -match 'PARTIAL')

# ---------------------------------------------------------------- 8. Deduplication
Write-Host ""
Write-Host "--- 8. Deduplication must not drop events ---"
$dupBat = Join-Path $tmp 'dup.bat'
[System.IO.File]::WriteAllText($dupBat, "@echo off`necho same`necho same`necho same`n",
    (New-Object System.Text.UTF8Encoding($false)))
$f8 = Join-Path $tmp 'dup.ndjson'
& $Ccap scan $dupBat --format ndjson --out $f8 *> $null
# Only count "echo same" (`echo off` is also executable=echo and would
# skew the count)
$dupCount = (Select-String -Path $f8 -Pattern '"executable":"echo","argv":\["echo","same"\]' -AllMatches).Count
Check "3 identical echo lines are not merged" ($dupCount -eq 3) "actual=$dupCount"

# The platform field of a static event
Check "the static event platform is correct" (HasPattern $f8 '"source":"static","pid":0,"ppid":0,"depth":0,"kind":"[^"]*","executable":"echo"')
$stJson = Join-Path $tmp 'st.json'
& $Ccap scan $dupBat --format json --out $stJson *> $null
$json = Get-Content $stJson -Raw -Encoding UTF8 | ConvertFrom-Json
Check "the static event platform = windows" ($json.events[0].platform -eq 'windows') "actual=$($json.events[0].platform)"

# A builtin command must not report a resolved path.
# There may really be an echo.exe in PATH (w64devkit / MSYS both ship one),
# resolving it would make people think echo is an external program —— but it
# never creates a process.
$builtinJson = Join-Path $tmp 'builtin.json'
& $Ccap scan (Join-Path $fixtures 'level3.bat') --format json --out $builtinJson *> $null
$bjson = Get-Content $builtinJson -Raw -Encoding UTF8 | ConvertFrom-Json
$echoEv = @($bjson.events | Where-Object { $_.executable -eq 'echo' })
Check "there is an echo builtin event" ($echoEv.Count -ge 1)
if ($echoEv.Count -ge 1) {
    Check "the builtin command has resolved = false" ($echoEv[0].resolved -eq $false) "actual=$($echoEv[0].resolved)"
    Check "the builtin command has no resolved= note" (-not (@($echoEv[0].notes) -match '^resolved='))
}
$hostEv = @($bjson.events | Where-Object { $_.executable -eq 'hostname' -and $_.source -eq 'static' })
if ($hostEv.Count -ge 1) {
    Check "an external command still has resolved = true" ($hostEv[0].resolved -eq $true)
}

# ---------------------------------------------------------------- 9. PowerShell
Write-Host ""
Write-Host "--- 9. PowerShell static analysis ---"
$psFixture = Join-Path $here 'fixtures\test.ps1'
if (Test-Path $psFixture) {
    $f9 = Join-Path $tmp 'ps.ndjson'
    & $Ccap scan $psFixture --format ndjson --out $f9 *> $null
    Check "captured the alias (ls)" ((CountExe $f9 'ls') -ge 1)
    Check "captured the cmdlet (Get-ChildItem)" ((CountExe $f9 'Get-ChildItem') -ge 1)
    Check "captured the right side of the pipe (Where-Object)" ((CountExe $f9 'Where-Object') -ge 1)
    Check "captured the external program (git)" ((CountExe $f9 'git') -ge 1)
    Check "captured the external program (npm)" ((CountExe $f9 'npm') -ge 1)
    # A quoted path must not be lost
    Check "captured the full quoted path" (HasPattern $f9 'whoami\.exe')
    # A variable reference is not a command
    Check "a variable reference is not a command" (-not (HasPattern $f9 '"executable":"\$env:PATH"'))
    Check "a variable assignment is not a command" (-not (HasPattern $f9 '"executable":"\$tool'))
    Check "param() is not a command" (-not (HasPattern $f9 '"executable":"param'))
    # Commands inside a block comment must not show up
    Check "the block comment content is ignored" (-not (HasPattern $f9 'should-not-appear'))
    # Commands inside a script block must show up
    Check "commands inside a script block are captured" ((CountExe $f9 'Get-Date') -ge 1)
} else {
    Write-Host "skip PowerShell (fixture missing)"
}

# ---------------------------------------------------------------- 9b. PowerShell gaps
#
# The constructs that used to be missed or mis-reported.
Write-Host ""
Write-Host "--- 9b. PowerShell: dynamic constructs ---"
$psGap = Join-Path $here 'fixtures\test_gaps.ps1'
if (Test-Path $psGap) {
    $f9b = Join-Path $tmp 'ps_gaps.ndjson'
    & $Ccap scan $psGap --format ndjson --out $f9b *> $null

    # Invoke-Expression with a literal: the command inside really runs
    Check "the command inside Invoke-Expression is captured" ((CountExe $f9b 'git') -ge 1)
    Check "the command inside the iex alias is captured" ((CountExe $f9b 'npm') -ge 1)
    Check "the command inside a single-quoted Invoke-Expression is captured" ((CountExe $f9b 'where.exe') -ge 1)

    # Invoke-Expression with a variable: the command is not knowable, and
    # guessing would be a false positive
    Check "Invoke-Expression with a variable reports no command" (-not (HasPattern $f9b '"executable":"\$cmd"'))

    # A hashtable literal is data
    Check "a hashtable key is not a command" (-not (HasPattern $f9b '"executable":"Path'))

    # A splatting token is an argument
    Check "a splatting token is not a command" (-not (HasPattern $f9b '"executable":"@params"'))

    # A here-string is text
    Check "the content of a here-string is not a command" (-not (HasPattern $f9b 'should-not-appear'))
    Check "the content of a single-quoted here-string is not a command" (-not (HasPattern $f9b 'also-should-not-appear'))

    # The line numbers after a multi-line here-string must still be right.
    # Get-Location is the last line of the fixture (line 41).
    Check "the line numbers after a here-string are still correct" (HasPattern $f9b '"script_line":41')
} else {
    Write-Host "skip PowerShell gaps (fixture missing)"
}

# ---------------------------------------------------------------- 10. BeingDebugged
Write-Host ""
Write-Host "--- 10. --clear-being-debugged ---"
$flagTarget = Join-Path $here 'probe\DbgFlagTarget.exe'
if (-not (Test-Path $flagTarget)) {
    # CMake builds it into the build directory
    $root2 = Split-Path -Parent $here
    foreach ($p in @("$root2\build\Release\DbgFlagTarget.exe", "$root2\build\DbgFlagTarget.exe")) {
        if (Test-Path $p) { $flagTarget = $p; break }
    }
}
if (Test-Path $flagTarget) {
    $outPlain = & $Ccap run $flagTarget --format text 2>&1 | Out-String
    $outPatched = & $Ccap run $flagTarget '--clear-being-debugged' --format text 2>&1 | Out-String
    Check "by default BeingDebugged = 1" ($outPlain -match 'PEB\.BeingDebugged = 1')
    Check "after the patch BeingDebugged = 0" ($outPatched -match 'PEB\.BeingDebugged = 0')
    Check "after the patch IsDebuggerPresent = 0" ($outPatched -match 'IsDebuggerPresent = 0')
} else {
    Write-Host "skip BeingDebugged (probe not compiled)"
}

# ---------------------------------------------------------------- 11. 32-bit target
Write-Host ""
Write-Host "--- 11. 32-bit (WOW64) target ---"
$t32 = Join-Path $here 'probe\Target32.exe'
if (-not (Test-Path $t32)) {
    $root3 = Split-Path -Parent $here
    foreach ($p in @("$root3\build\Release\Target32.exe", "$root3\build\Target32.exe")) {
        if (Test-Path $p) { $t32 = $p; break }
    }
}
if (Test-Path $t32) {
    $f11 = Join-Path $tmp 't32.ndjson'
    & $Ccap run $t32 'arg1' 'arg2' --format ndjson --out $f11 *> $null
    Check "the 32-bit target is captured" ((CountExe $f11 'Target32.exe') -ge 1)
    Check "the 32-bit target arguments are complete" (HasPattern $f11 'arg1')
    # WOW64 has two PEBs, BeingDebugged must be patched in the PEB32
    $out32 = & $Ccap run $t32 '--clear-being-debugged' --format text 2>&1 | Out-String
    Check "after the 32-bit patch BeingDebugged = 0" ($out32 -match 'BeingDebugged = 0')
    Check "after the 32-bit patch IsDebuggerPresent = 0" ($out32 -match 'IsDebuggerPresent = 0')
} else {
    Write-Host "skip 32-bit (Target32.exe not compiled; vcvars32 required)"
}

# ---------------------------------------------------------------- 12. Clean stdout
Write-Host ""
Write-Host "--- 12. --stdout-to-stderr makes the JSON directly consumable ---"
# Use Start-Process to separate stdout / stderr explicitly.
# (With a plain & call, PowerShell wraps stderr in an ErrorRecord and mixes
# it into the output stream.)
$outFile = Join-Path $tmp 'clean_stdout.txt'
$errFile = Join-Path $tmp 'clean_stderr.txt'
$args12 = @('run', $sys, '/c', 'echo TARGET_OUTPUT', '--format', 'json', '--stdout-to-stderr')
$p12 = Start-Process -FilePath $Ccap -ArgumentList $args12 -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput $outFile -RedirectStandardError $errFile

$stdoutOnly = Get-Content $outFile -Raw -ErrorAction SilentlyContinue
$jsonOk = $false
try { $null = $stdoutOnly | ConvertFrom-Json; $jsonOk = $true } catch { $jsonOk = $false }
Check "the JSON in stdout can be parsed directly" $jsonOk
# Note: TARGET_OUTPUT **should** appear in the JSON (it is the captured argv),
# so a plain substring search would not do. What we check is that "the target
# output did not leak into stdout as its own line".
$stdoutLines = @(Get-Content $outFile -ErrorAction SilentlyContinue)
$polluted = $false
foreach ($l in $stdoutLines) {
    if ($l.Trim() -eq 'TARGET_OUTPUT') { $polluted = $true; break }
}
Check "stdout has no target output line mixed in" (-not $polluted)
Check "the target output went to stderr" ((Get-Content $errFile -Raw -ErrorAction SilentlyContinue) -match 'TARGET_OUTPUT')

# ---------------------------------------------------------------- 13. --tree
Write-Host ""
Write-Host "--- 13. --tree process tree view ---"
$treeOut = & $Ccap run (Join-Path $fixtures 'build.bat') --tree 2>&1 | Out-String
Check "the tree has a root node" ($treeOut -match '\+- cmd\.exe')
Check "the tree has indented child nodes" ($treeOut -match '(?m)^\s{2}\+- ')
Check "the tree contains the pid information" ($treeOut -match '\[pid=\d+')
Check "the tree lists the static events separately" ($treeOut -match 'static \(script builtins')

# ---------------------------------------------------------------- 14. Filtering
Write-Host ""
Write-Host "--- 14. --exclude / --include filtering ---"
$baseNd = Join-Path $tmp 'filter_base.ndjson'
& $Ccap run (Join-Path $fixtures 'build.bat') --format ndjson --out $baseNd *> $null
$baseCount = CountRuntime $baseNd

$exNd = Join-Path $tmp 'filter_ex.ndjson'
& $Ccap run (Join-Path $fixtures 'build.bat') --format ndjson --out $exNd '--exclude' 'whoami' *> $null
$exCount = CountRuntime $exNd
Check "--exclude reduced the event count" ($exCount -lt $baseCount) "base=$baseCount after=$exCount"
Check "there is no whoami after --exclude" (-not (HasPattern $exNd '"executable":"whoami"'))

$inNd = Join-Path $tmp 'filter_in.ndjson'
& $Ccap run (Join-Path $fixtures 'build.bat') --format ndjson --out $inNd '--include' 'whoami' *> $null
$inCount = CountRuntime $inNd
Check "--include leaves only the allowlisted events" ($inCount -eq 1) "actual=$inCount"

# The filtered out events must be counted and reported —— silently dropping
# events is the worst possible failure mode
$filterJson = Join-Path $tmp 'filter.json'
& $Ccap run (Join-Path $fixtures 'build.bat') --format json --out $filterJson '--exclude' 'whoami' *> $null
$fjson = Get-Content $filterJson -Raw -Encoding UTF8 | ConvertFrom-Json
Check "the filtered count is in the notes" (($fjson.notes -join ' ') -match 'filtered out')

# No filtering by default
$noFilterCount = CountRuntime $baseNd
Check "by default no event is discarded" ($noFilterCount -ge 8) "actual=$noFilterCount"

# ---------------------------------------------------------------- 15. --summary
Write-Host ""
Write-Host "--- 15. --summary aggregate statistics ---"
$sumOut = & $Ccap run (Join-Path $fixtures 'build.bat') --format text --summary 2>&1 | Out-String
Check "the summary has a runtime count" ($sumOut -match 'runtime events:\s+\d+')
Check "the summary has a static count" ($sumOut -match 'static events:\s+\d+')
Check "the summary has the maximum depth" ($sumOut -match 'max depth:\s+\d+')
Check "the summary has the unique executable count" ($sumOut -match 'unique executables:\s+\d+')

# Key regression: --summary must not break the validity of the JSON.
# Early versions appended the summary as plain text after the JSON -> the whole
# document was not valid JSON.
$sumJson = Join-Path $tmp 'summary.json'
& $Ccap run (Join-Path $fixtures 'build.bat') --format json --summary --out $sumJson *> $null
$sumOk = $false
$sumObj = $null
try {
    $sumObj = Get-Content $sumJson -Raw -Encoding UTF8 | ConvertFrom-Json
    $sumOk = $true
} catch { $sumOk = $false }
Check "JSON + --summary is still valid JSON" $sumOk
if ($sumOk) {
    Check "the JSON has a summary field" ($null -ne $sumObj.summary)
    Check "summary.runtime_events is a number" ($sumObj.summary.runtime_events -is [int] -or $sumObj.summary.runtime_events -is [long])
    Check "the runtime count in the summary matches the events" `
        ($sumObj.summary.runtime_events -eq (@($sumObj.events | Where-Object { $_.source -eq 'runtime' })).Count)
    Check "the summary has the executables aggregation" ($null -ne $sumObj.summary.executables)
}

# The NDJSON convention is "one event object per line" —— the summary must
# not be appended to it
$sumNd = Join-Path $tmp 'summary.ndjson'
& $Ccap run (Join-Path $fixtures 'build.bat') --format ndjson --summary --out $sumNd *> $null
$ndBad = 0
foreach ($l in (Get-Content $sumNd -ErrorAction SilentlyContinue)) {
    if (-not [string]::IsNullOrWhiteSpace($l)) {
        try { $null = $l | ConvertFrom-Json } catch { $ndBad++ }
    }
}
Check "every line of NDJSON + --summary is still valid JSON" ($ndBad -eq 0) "bad=$ndBad"

# ---------------------------------------------------------------- 16. Capture completeness conclusion
Write-Host ""
Write-Host "--- 16. Capture completeness conclusion (notes) ---"
$compJson = Join-Path $tmp 'complete.json'
& $Ccap run (Join-Path $fixtures 'build.bat') --format json --out $compJson *> $null
$cjson = Get-Content $compJson -Raw -Encoding UTF8 | ConvertFrom-Json
Check "the notes contain the completeness conclusion" (($cjson.notes -join ' ') -match 'capture completeness:')

# ---------------------------------------------------------------- 17. PowerShell subexpression
Write-Host ""
Write-Host "--- 17. PowerShell $() subexpression ---"
if (Test-Path $psFixture) {
    $subNd = Join-Path $tmp 'ps_sub.ndjson'
    & $Ccap scan $psFixture --format ndjson --out $subNd *> $null
    # The Get-Date inside $(Get-Date -Format o) must be reported on its own
    Check "the command inside the subexpression is captured" (HasPattern $subNd '"argv":\["Get-Date","-Format","o"\]')
    # The arguments of the outer command must be preserved completely (not a
# chunk short because the subexpression was pulled out)
    Check "the outer command arguments are preserved completely" (HasPattern $subNd '\$\(Get-Date -Format o\)')
    # The verb "Where" of Where-Object must be in the verb table -> flagged as builtin
    Check "Where-Object is recognized as a cmdlet" (HasPattern $subNd '"executable":"Where-Object","argv":\["Where-Object"\],"command_line":"[^"]*","cwd":"[^"]*","is_builtin":true')
}

# ---------------------------------------------------------------- 18. REM comments
Write-Host ""
Write-Host "--- 18. cmd REM comments (case insensitive) ---"
$remBat = Join-Path $tmp 'rem.bat'
[System.IO.File]::WriteAllText($remBat, "@echo off`nREM this is a comment`nRem another`necho real`n",
    (New-Object System.Text.UTF8Encoding($false)))
$remNd = Join-Path $tmp 'rem.ndjson'
& $Ccap scan $remBat --format ndjson --out $remNd *> $null
Check "uppercase REM is not treated as a command" (-not (HasPattern $remNd 'this is a comment'))
Check "mixed case Rem is not treated as a command" (-not (HasPattern $remNd 'another'))
Check "the real command is still there" ((CountExe $remNd 'echo') -ge 1)

Write-Host "--- 19. cmd 'for' loop: the body is the command, the list is not ---"
# The parentheses of a "for" loop hold the **iteration list**, not commands.
# Treating them as commands reported "a b" as a command and lost the real
# one ("echo loop").
$forBat = Join-Path $tmp 'forloop.bat'
[System.IO.File]::WriteAllText($forBat,
    "@echo off`nfor %i in (a b) do echo loop`nfor %%i in (x y) do echo batchloop`nfor %i in (do) do echo tricky`n",
    (New-Object System.Text.UTF8Encoding($false)))
$forNd = Join-Path $tmp 'forloop.ndjson'
& $Ccap scan $forBat --format ndjson --out $forNd *> $null
Check "the iteration list is not reported as a command" (-not (HasPattern $forNd '"a b"'))
Check "the for body is reported (command line form)" (HasPattern $forNd 'loop')
Check "the for body is reported (batch form)" (HasPattern $forNd 'batchloop')
Check "a 'do' inside the list does not confuse the parser" (HasPattern $forNd 'tricky')

# ---------------------------------------------------------------- Summary
Write-Host ""
Write-Host "==================================="
Write-Host ("PASS: {0}   FAIL: {1}" -f $script:pass, $script:fail)
Write-Host "==================================="

Get-ChildItem -Path $tmp -Recurse -ErrorAction SilentlyContinue | ForEach-Object {
    if ($_.PSIsContainer) { Move-Item $_.FullName (Join-Path $env:TEMP ("ccap_trash_" + [Guid]::NewGuid().ToString('N'))) -ErrorAction SilentlyContinue }
}

if ($script:fail -eq 0) { exit 0 } else { exit 1 }
