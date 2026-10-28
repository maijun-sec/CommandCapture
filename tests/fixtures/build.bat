@echo off
REM === builtin commands (no process created - only static analysis sees them) ===
echo hello from echo
cd /d %~dp0
set FOO=bar
dir /b > nul
if exist "%~dp0" echo fixture-dir-exists
for %%i in (a b c) do echo loop %%i

REM === external commands: direct children ===
whoami > nul
where cmd.exe > nul

REM === pipe: cmd.exe starts both sides ===
echo piped | findstr piped

REM === call nested script (grandchild) ===
call "%~dp0nested.bat" arg1 arg2

REM === redirection + short-lived process ===
cmd /c exit 0
exit /b 0