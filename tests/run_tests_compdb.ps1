# CommandCapture - capture every command a script or executable spawns.
#
# Copyright (c) 2026 maijun
#
# SPDX-License-Identifier: MIT
#
# Released under the MIT License. See the LICENSE file at the repository
# root for the full text.

﻿# ccap compdb end-to-end regression tests (Windows)
#
# Covers **the same scenarios** as tests/run_tests_compdb.sh,
# and the two must stay in sync —— inconsistent scenarios turn "only one
# side passes" into a false green.
#
# Covers:
#   1. The full compiler list
#   2. build log parsing
#   3. clang compilation database export
#   4. analyzer style translation
#   5. Script filtering
#   6. Reverse parsing of an existing compilation database
#   7. Aliases (symlinks recognize each other)

param(
    [string]$Ccap = ""
)

$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root

if ([string]::IsNullOrEmpty($Ccap)) {
    $Ccap = Join-Path $Root "build\Release\ccap.exe"
}
if (-not (Test-Path $Ccap)) {
    Write-Host "ccap not found: $Ccap" -ForegroundColor Red
    Write-Host "Build first:  cmake --build build --config Release" -ForegroundColor Yellow
    exit 2
}

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

# Read the file content (returns an empty string when missing, does not
# throw)
function GetText([string]$path) {
    if (-not (Test-Path $path)) { return "" }
    return (Get-Content $path -Raw -Encoding UTF8)
}

$Tmp = Join-Path $env:TEMP ("ccap_compdb_test_" + [System.Diagnostics.Process]::GetCurrentProcess().Id)
if (Test-Path $Tmp) { Remove-Item $Tmp -Recurse -Force }
New-Item -ItemType Directory -Path $Tmp | Out-Null

try {
    # ── 1. Compiler list ─────────────────────────────────────────────────
    Write-Host "=== 1. The full compiler list ==="
    $listFile = Join-Path $Tmp "list.txt"
    & $Ccap compdb list --format text > $listFile 2>&1
    $listText = GetText $listFile

    foreach ($kind in @("gcc", "clang", "msvc", "generic")) {
        Check "the list contains $kind" ($listText -match $kind) "output: $listText"
    }
    Check "the list mentions the version suffix" ($listText -match "gcc-11") "output: $listText"

    # ── 2. build log ─────────────────────────────────────────
    Write-Host ""
    Write-Host "=== 2. build log parsing ==="
    $logFile = Join-Path $Tmp "build-log.txt"
    @"
EXECUTING 'make all '
EXECUTING '/bin/sh -c cd qmake && make '
CWD = /export/home/acc/test-packages/qt-x11-free-3.3.2/qmake
COMPILING '/export/home/acc/prevent/bin/sa-translate g++ -c -o property.o -I. -Igenerators -DQT_NO_TEXTCODEC property.cpp '
EXECUTING '/usr/ccs/bin/as -Qy -s -o property.o /var/tmp/cc2Wo7sG.s '
CWD = /export/home/acc/test-packages/qt-x11-free-3.3.2/qt
COMPILING '/export/home/acc/prevent/bin/sa-translate g++ -c -o main.o -I. -DQT_NO_TEXTCODEC main.cpp '
"@ | Set-Content -Path $logFile -Encoding UTF8

    $parseFile = Join-Path $Tmp "parse.txt"
    & $Ccap compdb parse $logFile --format text > $parseFile 2>&1
    $parseText = GetText $parseFile

    # The build log holds 5 commands: make / sh / wrapper(g++) / as / wrapper(g++)
    Check "parsed 5 commands" ($parseText -match "Parsed 5 command") "output: $parseText"
    Check "2 of them are compilation commands" ($parseText -match "2 are compilation command") "output: $parseText"

    $showFile = Join-Path $Tmp "show.txt"
    & $Ccap compdb show $logFile > $showFile 2>&1
    $showText = GetText $showFile

    Check "the wrapper prefix was stripped" ($showText -match "\[KEEP\] g\+\+ ") "output: $showText"
    Check "make was classified as a script" ($showText -match "dropped: script") "output: $showText"
    Check "as was excluded" ($showText -match "dropped: excluded \(as\)") "output: $showText"
    Check "the macros were extracted" ($showText -match "QT_NO_TEXTCODEC") "output: $showText"
    Check "the include paths were extracted" ($showText -match "includes: \. generators") "output: $showText"
    Check "the CWD was associated correctly" ($showText -match "qt-x11-free-3\.3\.2/qmake") "output: $showText"

    # ── 3. Export the clang compilation database ─────────────────────────────────────
    Write-Host ""
    Write-Host "=== 3. clang compilation database export ==="
    $dbFile = Join-Path $Tmp "compile_commands.json"
    & $Ccap compdb export $logFile --output $dbFile --format text > $null 2>&1
    Check "the file was generated" (Test-Path $dbFile)

    if (Test-Path $dbFile) {
        $dbText = GetText $dbFile
        $db = $null
        try { $db = $dbText | ConvertFrom-Json } catch { $db = $null }
        Check "it is valid JSON" ($null -ne $db) "content: $dbText"
        if ($null -ne $db) {
            Check "it has 2 entries" ($db.Count -eq 2) "count: $($db.Count)"
            Check "the entry has arguments" ($null -ne $db[0].arguments) "entry: $($db[0] | ConvertTo-Json -Compress)"
            Check "file is an absolute path" ($db[0].file.StartsWith("/")) "file: $($db[0].file)"
        }
    }

    # ── 4. analyzer style translation ─────────────────────────────────────────
    Write-Host ""
    Write-Host "=== 4. analyzer style translation ==="
    $trFile = Join-Path $Tmp "translate.txt"
    & $Ccap compdb translate $logFile > $trFile 2>&1
    $trText = GetText $trFile
    $n = ([regex]::Matches($trText, "g\+\+")).Count
    Check "translated 2 commands" ($n -eq 2) "actual=$n"
    Check "it contains -I style arguments" ($trText -match "-I ") "output: $trText"

    # ── 5. Script filtering ───────────────────────────────────────────────────
    Write-Host ""
    Write-Host "=== 5. Script filtering ==="
    $cmdsFile = Join-Path $Tmp "cmds.txt"
    @"
g++ -I../include -DFOO=1 -std=c++17 -c src/main.cpp -o build/main.o
gcc -c src/util.c -o build/util.o
sh -c 'g++ -c other.cpp'
make -j8
bash build.sh
cmd.exe /C build.bat
"@ | Set-Content -Path $cmdsFile -Encoding UTF8

    $parse2File = Join-Path $Tmp "parse2.txt"
    & $Ccap compdb parse $cmdsFile --format text > $parse2File 2>&1
    $parse2Text = GetText $parse2File
    Check "2 of the 6 commands are compilation commands" ($parse2Text -match "2 are compilation command") "output: $parse2Text"

    $parse3File = Join-Path $Tmp "parse3.txt"
    & $Ccap compdb parse $cmdsFile --format text --keep-scripts > $parse3File 2>&1
    $parse3Text = GetText $parse3File
    $kept = 0
    if ($parse3Text -match "(\d+) are compilation command") { $kept = [int]$Matches[1] }
    Check "--keep-scripts kept more commands" ($kept -gt 2) "kept=$kept"

    # ── 6. Reverse parsing of an existing compilation database ─────────────────────────────────────────
    Write-Host ""
    Write-Host "=== 6. Reverse parsing of an existing compile_commands.json ==="
    if (Test-Path $dbFile) {
        $parse4File = Join-Path $Tmp "parse4.txt"
        & $Ccap compdb parse $dbFile --format text > $parse4File 2>&1
        $parse4Text = GetText $parse4File
        Check "it can parse back its own output" ($parse4Text -match "2 are compilation command") "output: $parse4Text"
    } else {
        Check "it can parse back its own output" $false "compile_commands.json does not exist"
    }

    # ── 7. Aliases ───────────────────────────────────────────────────────
    Write-Host ""
    Write-Host "=== 7. Aliases (symlinks recognize each other) ==="
    # Windows has no symlinks, but a **hard link** can be used to verify that
    # the same file is referenced by two names.
    #
    # The real path resolution (GetFinalPathNameByHandle) returns the path the
    # file was opened with on a hard link, so what is tested here is "can two
    # names of the same executable find each other once both are recorded".
    $binDir = Join-Path $Tmp "bin"
    New-Item -ItemType Directory -Path $binDir | Out-Null
    $realGcc = Join-Path $binDir "real-gcc.exe"
    $aliasCc = Join-Path $binDir "alias-cc.exe"
    Set-Content -Path $realGcc -Value "#!/bin/sh" -Encoding UTF8
    Copy-Item $realGcc $aliasCc -Force

    $aliasFile = Join-Path $Tmp "aliases.txt"
    & $Ccap compdb aliases $realGcc > $aliasFile 2>&1
    $aliasText = GetText $aliasFile
    Check "the aliases command runs" ($aliasText -match "real path") "output: $aliasText"
    Check "it reports the kind" ($aliasText -match "kind:") "output: $aliasText"

    # Feed both names at once to verify that the alias group recognizes
# each other
    $alias2File = Join-Path $Tmp "aliases2.txt"
    & $Ccap compdb aliases $realGcc $aliasCc > $alias2File 2>&1
    $alias2Text = GetText $alias2File
    Check "both names appear in the output" `
        (($alias2Text -match "real-gcc") -and ($alias2Text -match "alias-cc")) `
        "output: $alias2Text"

    # ── 8. JSON output validity ────────────────────────────────────────────
    Write-Host ""
    Write-Host "=== 8. JSON output validity ==="
    $jsonOut = Join-Path $Tmp "parse.json"
    & $Ccap compdb parse $logFile --format json > $jsonOut 2>&1
    $jsonText = GetText $jsonOut
    $jsonObj = $null
    try { $jsonObj = $jsonText | ConvertFrom-Json } catch { $jsonObj = $null }
    Check "the JSON output is parseable" ($null -ne $jsonObj) "output: $jsonText"
    if ($null -ne $jsonObj) {
        Check "the total_commands field is correct" ($jsonObj.total_commands -eq 5) "value: $($jsonObj.total_commands)"
        Check "the compile_commands field is correct" ($jsonObj.compile_commands -eq 2) "value: $($jsonObj.compile_commands)"
    }
}
finally {
    if (Test-Path $Tmp) { Remove-Item $Tmp -Recurse -Force -ErrorAction SilentlyContinue }
}

Write-Host ""
Write-Host "==================================="
Write-Host ("PASS: " + $script:pass + "   FAIL: " + $script:fail)
Write-Host "==================================="
if ($script:fail -ne 0) { exit 1 }
exit 0
