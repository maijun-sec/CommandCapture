# PowerShell static analysis: the constructs that used to be missed or
# mis-reported.
#
# Each case here was a real gap:
#   - Invoke-Expression "cmd"   the command inside the string really runs
#   - Invoke-Expression $cmd    the command is only known at runtime
#   - @{ key = value }          a hashtable literal is data, not a command
#   - @params                   a splatting token is an argument
#   - @"..."@                   a here-string is text, not code

# 1. Invoke-Expression with a literal string --- the command really runs
Invoke-Expression "git status"

# 2. The alias form
iex "npm install"

# 3. A single-quoted literal
Invoke-Expression 'where.exe'

# 4. Invoke-Expression with a variable --- the command is NOT knowable
$cmd = "git"
Invoke-Expression $cmd

# 5. A hashtable literal --- the key is not a command
$params = @{ Path = "C:\temp" }

# 6. A splatting token --- not a command
Get-ChildItem @params

# 7. A here-string --- its content is text, not commands
$text = @"
should-not-appear-inside-a-here-string
"@

# 8. A single-quoted here-string
$literal = @'
also-should-not-appear
'@

# 9. The line number after a here-string must still be right
Get-Location
