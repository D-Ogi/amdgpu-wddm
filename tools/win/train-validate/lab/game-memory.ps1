# The memory admission of a game arm, read before the session starts. The game harness admits a session only
# when the 'Memory\Available MBytes' counter reads NeedMB or more (game-runtime.ps1: 3500 MB, the owner's
# low-memory rule after session 101 paged at the menu). On this board the carve-out takes its share of the
# 16 GiB before Windows starts: at 12288 MiB Windows sees about 3816 MB and has about 1300 MB available, so no
# pre-step can ever admit a game. The b29 run waited out a whole host backstop on that (native-caps547).
# This script reads the same counter and fails at once, with the reason.
param([int]$NeedMB = 3500, [int]$OsFloorMB = 2048)
$visible = [int]((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1MB)
$available = [int](New-Object Diagnostics.PerformanceCounter('Memory', 'Available MBytes')).NextValue()
"game memory: Windows sees $visible MB, $available MB available, the game harness needs $NeedMB MB available"
if ($visible -lt $NeedMB + $OsFloorMB) {
    "game memory FAILED carve-out too large for games: $visible MB of RAM cannot hold $NeedMB MB available " +
        "next to Windows ($OsFloorMB MB); set a smaller board memory size and restart first"
    exit 1
}
if ($available -lt $NeedMB) {
    "game memory FAILED only $available MB available of the $NeedMB MB the game harness needs: " +
        "end what holds the memory first"
    exit 1
}
'game memory OK'
