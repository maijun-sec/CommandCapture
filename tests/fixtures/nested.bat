@echo off
REM `call` runs in the same cmd.exe, so use explicit cmd /c to get a real 3rd level.
echo nested running with %1 %2
hostname > nul
cmd /c "%~dp0level3.bat"
exit /b 0