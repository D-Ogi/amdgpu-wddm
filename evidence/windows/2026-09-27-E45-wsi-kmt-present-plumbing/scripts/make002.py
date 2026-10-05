import pathlib, subprocess

d = pathlib.Path(r'P:\bc-250\scratch\wsi-kmt-2026-09-27')


def rep(s, pairs):
    for old, new in pairs:
        assert s.count(old) == 1, old[:60]
        s = s.replace(old, new)
    return s


run = (d / 'run-kmt-001.ps1').read_text(encoding='utf-8')
run = run.replace('wsi-kmt-001', 'wsi-kmt-002').replace('kmt-001', 'kmt-002')
run = rep(run, [
    ("""# wsi-kmt-002: plumbing control (H1) of the Vulkan WSI KMT present path on candidate ICD B9F4C95B
# (fork amdgpu-wddm/radv-wddm2-wsi-kmt 1ae7b6af = present-log 0f9811a5 + KMT present). The candidate is
# selected per process through VK_DRIVER_FILES; the registered ICD is not touched.""",
     """# wsi-kmt-002: plumbing control (H1) of the Vulkan WSI KMT present path on candidate ICD B9F4C95B
# (fork amdgpu-wddm/radv-wddm2-wsi-kmt 1ae7b6af = present-log 0f9811a5 + KMT present). Run 001 showed that
# the loader ignores VK_DRIVER_FILES in the elevated task (baseline 93B1D1FD loaded), so this run swaps the
# candidate in place of the registered ICD for its duration (E43 method) and restores it in finally."""),
    ("""New-Item -ItemType Directory -Path $out, "$out\\icd", "$out\\mesa-cache" | Out-Null
""", """New-Item -ItemType Directory -Path $out, "$out\\mesa-cache" | Out-Null
"""),
    ("""# Per-process ICD selection: a manifest next to a copy of the candidate, VK_DRIVER_FILES in the child only.
Copy-Item -LiteralPath $candidate -Destination "$out\\icd\\vulkan_radeon.dll"
if ((Get-FileHash -LiteralPath "$out\\icd\\vulkan_radeon.dll").Hash -ne $candidateHash) { throw 'Candidate copy hash' }
@{ file_format_version = '1.0.0'; ICD = @{ library_path = "$out\\icd\\vulkan_radeon.dll"; api_version = '1.4.363' } } |
  ConvertTo-Json -Depth 3 | Set-Content -LiteralPath "$out\\icd\\radeon_icd.json" -Encoding ASCII

$code = 0
$pmProc = $null
$child = $null
try {
""", """# Registered ICD: keep the baseline copy in the run directory, then swap the candidate in (restored in finally).
Copy-Item -LiteralPath $registered -Destination "$out\\baseline-93B1D1FD.dll"
if ((Get-FileHash -LiteralPath "$out\\baseline-93B1D1FD.dll").Hash -ne $baselineHash) { throw 'Baseline copy hash' }
$swapped = $false
$code = 0
$pmProc = $null
$child = $null
try {
  Copy-Item -LiteralPath $candidate -Destination $registered -Force
  $swapped = $true
  "icd_swapped=" + (Get-FileHash -LiteralPath $registered).Hash.Substring(0, 8)
"""),
    ("""    $env:VK_DRIVER_FILES = "$out\\icd\\radeon_icd.json"
""", ""),
    ("""  foreach ($n in 'VK_DRIVER_FILES', 'BC250_WSI_PRESENT_LOG', 'BC250_TRACE_SUBMITS', 'BC250_WSI_CPU_PRESENT', 'MESA_SHADER_CACHE_DIR') { Remove-Item "Env:$n" -ErrorAction SilentlyContinue }
""", """  foreach ($n in 'BC250_WSI_PRESENT_LOG', 'BC250_TRACE_SUBMITS', 'BC250_WSI_CPU_PRESENT', 'MESA_SHADER_CACHE_DIR') { Remove-Item "Env:$n" -ErrorAction SilentlyContinue }
  if ($swapped) {
    $restored = $false
    for ($i = 0; $i -lt 5 -and -not $restored; $i++) {
      try { Copy-Item -LiteralPath "$out\\baseline-93B1D1FD.dll" -Destination $registered -Force; $restored = $true } catch { "icd restore attempt $i failed: $($_.Exception.Message)"; Start-Sleep -Seconds 2 }
    }
    $h = (Get-FileHash -LiteralPath $registered).Hash
    if ($h -ne $baselineHash) { "ICD RESTORE MISMATCH $h"; $code = 3 } else { 'registered ICD restored to 93B1D1FD' }
  }
"""),
])
(d / 'run-kmt-002.ps1').write_text(run, encoding='utf-8', newline='\r\n')
for n in ('worker', 'launch', 'status', 'cleanup'):
    s = (d / f'{n}-kmt-001.ps1').read_text(encoding='utf-8').replace('wsi-kmt-001', 'wsi-kmt-002').replace('kmt-001', 'kmt-002')
    (d / f'{n}-kmt-002.ps1').write_text(s, encoding='utf-8', newline='\r\n')
p = (d / 'pull-001.py').read_text(encoding='utf-8').replace('wsi-kmt-001', 'wsi-kmt-002').replace('kmt-001', 'kmt-002').replace('run001', 'run002')
p = p.replace("'icd/radeon_icd.json', ", "")
(d / 'pull-002.py').write_text(p, encoding='utf-8', newline='\n')
print('written')
