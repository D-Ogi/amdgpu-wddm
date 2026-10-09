# Typed view of BC250_START_HEALTH_* in driver/kmd/bc250kmd_escape.h.
[System.FlagsAttribute()]
enum Bc250StartHealthFlags {
 Full = 1
 Ready = 2
 Visible = 4
 Confirmed = 8
}
# Pure gate for starting the WSI control within an already-running DWM trial. Self-contained on purpose, so a
# native-trial package can carry it alone; test-identity.ps1 proves the default ABI is identity.ps1's candidate.
function Get-ConfirmedPresentStart {
 param([Parameter(Mandatory)][string]$Health,[Parameter(Mandatory)][double]$ElapsedSeconds,
 [UInt64]$ExpectedGeneration=0,[UInt64]$ExpectedEpoch=0,[UInt64]$ConfirmedEpoch=0,[string]$Abi=$KmdCandidateAbi)
 if($Abi -notmatch '^0x[0-9A-F]{8}$'){throw 'ABI required: pass -Abi or dot-source identity.ps1 first'}
 $pattern='health abi=1 version='+[regex]::Escape($Abi)+' flags=(\d+) generation=(\d+) epoch=(\d+) completed=(\d+) age_ms=(\d+) ready_ms=(\d+)'
 if($Health -notmatch $pattern){throw 'Invalid KMD health witness for the expected ABI'}
 $flags=[Bc250StartHealthFlags][int]$Matches[1];$generation=[UInt64]$Matches[2];$epoch=[UInt64]$Matches[3]
 $completed=[UInt64]$Matches[4];$age=[UInt64]$Matches[5];$ready=[UInt64]$Matches[6]
 if($ElapsedSeconds -lt 0 -or $ElapsedSeconds -ge 90){throw 'Confirmed health not observed within 90 seconds'}
 $required=[Bc250StartHealthFlags]::Full -bor [Bc250StartHealthFlags]::Ready -bor [Bc250StartHealthFlags]::Visible
 $known=$required -bor [Bc250StartHealthFlags]::Confirmed
 $confirmed=($flags -band [Bc250StartHealthFlags]::Confirmed) -ne 0
 if(($flags -band $required) -ne $required -or ([int]$flags -band (-bnot [int]$known)) -ne 0 -or !$generation -or !$epoch -or !$completed -or $age -gt 15000){throw 'Unhealthy or stale presentation witness'}
 if(($ExpectedGeneration -and $generation -ne $ExpectedGeneration) -or ($ExpectedEpoch -and $epoch -ne $ExpectedEpoch)){throw 'Health generation/epoch changed'}
 # The 60 s ready age belongs to the interval the KMD durably confirmed under its own lock. CONFIRMED follows
 # the confirmed start, that is its generation, while every mode set and every visibility change advances the
 # epoch and restarts the ready clock (driver/kmd/start_health.c HealthInvalidate), so a game's exit mode commit
 # leaves a CONFIRMED witness with a short ready age. -ConfirmedEpoch names the interval of this same start that
 # a caller already admitted with the full ready age, and it must be given with the generation it belongs to.
 # A later interval of that start may then show a short ready age; the admitted interval itself may not, and an
 # earlier one is impossible within a generation.
 if($ConfirmedEpoch -and (!$ExpectedGeneration -or $ExpectedEpoch)){throw 'An admitted interval needs its generation and no pinned epoch'}
 if($ConfirmedEpoch -and $epoch -lt $ConfirmedEpoch){throw 'Confirmed interval precedes the admitted start'}
 if($confirmed -and $ready -lt 60000 -and !($ConfirmedEpoch -and $epoch -gt $ConfirmedEpoch)){throw 'Confirmed health has insufficient ready age'}
 return @{launch=$confirmed;flags=[int]$flags;flag_names=$flags.ToString();generation=$generation;epoch=$epoch;completed=$completed;ready_ms=$ready;age_ms=$age;confirmed_since_epoch=$ConfirmedEpoch}
}
