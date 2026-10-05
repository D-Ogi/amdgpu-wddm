# Typed view of BC250_START_HEALTH_* in driver/kmd/bc250kmd_escape.h.
[System.FlagsAttribute()]
enum Bc250StartHealthFlags {
 Full = 1
 Ready = 2
 Visible = 4
 Confirmed = 8
}
# Pure gate for starting the WSI control within an already-running DWM trial.
function Get-ConfirmedPresentStart {
 param([Parameter(Mandatory)][string]$Health,[Parameter(Mandatory)][double]$ElapsedSeconds,
 [UInt64]$ExpectedGeneration=0,[UInt64]$ExpectedEpoch=0)
 $pattern='health abi=1 version=0x000700AA flags=(\d+) generation=(\d+) epoch=(\d+) completed=(\d+) age_ms=(\d+) ready_ms=(\d+)'
 if($Health -notmatch $pattern){throw 'Invalid KMD170 health witness'}
 $flags=[Bc250StartHealthFlags][int]$Matches[1];$generation=[UInt64]$Matches[2];$epoch=[UInt64]$Matches[3]
 $completed=[UInt64]$Matches[4];$age=[UInt64]$Matches[5];$ready=[UInt64]$Matches[6]
 if($ElapsedSeconds -lt 0 -or $ElapsedSeconds -ge 90){throw 'Confirmed health not observed within 90 seconds'}
 $required=[Bc250StartHealthFlags]::Full -bor [Bc250StartHealthFlags]::Ready -bor [Bc250StartHealthFlags]::Visible
 $known=$required -bor [Bc250StartHealthFlags]::Confirmed
 $confirmed=($flags -band [Bc250StartHealthFlags]::Confirmed) -ne 0
 if(($flags -band $required) -ne $required -or ([int]$flags -band (-bnot [int]$known)) -ne 0 -or !$generation -or !$epoch -or !$completed -or $age -gt 15000){throw 'Unhealthy or stale presentation witness'}
 if(($ExpectedGeneration -and $generation -ne $ExpectedGeneration) -or ($ExpectedEpoch -and $epoch -ne $ExpectedEpoch)){throw 'Health generation/epoch changed'}
 if($confirmed -and $ready -lt 60000){throw 'Confirmed health has insufficient ready age'}
 return @{launch=$confirmed;flags=[int]$flags;flag_names=$flags.ToString();generation=$generation;epoch=$epoch;completed=$completed;ready_ms=$ready;age_ms=$age}
}
