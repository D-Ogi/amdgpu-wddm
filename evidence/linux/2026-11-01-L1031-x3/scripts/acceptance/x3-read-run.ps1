# Run the installed read-only X3 audit in place and show the acknowledgement log tail.
& 'C:\BC250\x3\x3-read.ps1'
'--- log'
Get-Content 'C:\BC250\x3\x3-clear.log' -Tail 4
