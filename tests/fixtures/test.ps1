# PowerShell static analysis test
# Covers: aliases, pipelines, script blocks, call operators, $() sub-expressions,
# comments, param

Write-Output "=== 1. basic cmdlets and aliases ==="
Get-Date
ls
Get-ChildItem -Path C:\temp

Write-Output ""
Write-Output "=== 2. pipeline ==="
Get-Process | Where-Object { $_.Name -eq 'explorer' } | Select-Object Id

Write-Output ""
Write-Output "=== 3. call operators ==="
& "C:\Windows\System32\whoami.exe" /all
. "$PSScriptRoot\helper.ps1"

Write-Output ""
Write-Output "=== 4. sub-expressions ==="
Write-Output "$(Get-Date -Format o)"
$x = (Get-Location)
Write-Output "now is $(Get-Date -Format HH:mm:ss)"

Write-Output ""
Write-Output "=== 5. variables and assignments (must not be commands) ==="
$env:PATH
$myvar = 42
$obj.Property

Write-Output ""
Write-Output "=== 6. comments (must not appear) ==="
# this is a comment
<#
block comment
Get-NothingHere
#>

Write-Output ""
Write-Output "=== 7. param (must not appear) ==="
param($x)

Write-Output ""
Write-Output "=== 8. nested sub-expressions ==="
Write-Output "$((Get-Date).Year)"

Write-Output ""
Write-Output "=== 9. external programs ==="
git status
npm install
dotnet build
