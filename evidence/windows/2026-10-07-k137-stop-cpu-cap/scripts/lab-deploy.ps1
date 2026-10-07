# LAB: install KMD 0.7.216.15 (kmd/k137-cpu-baseline) over tester.20 and restart Windows (deploy-candidate.ps1).
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\BC250\tmp\k137\deploy-candidate.ps1 -Inf C:\BC250\tmp\kmd216-15\bc250kmd.inf -Defaults C:\BC250\tmp\k137\registry-defaults.json 2>&1
