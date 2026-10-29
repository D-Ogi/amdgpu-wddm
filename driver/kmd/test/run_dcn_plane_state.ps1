# BD-223 stage S2: dcn.c's plane state code (approvals, admission, the locked plane steps and the way back), extracted
# from the driver's source by generate_dcn_plane_state_test.py and driven by dcn_plane_state_test.c. Each -Mutation
# breaks one rule of that code, and the test must fail on it.
#
#   pwsh driver\kmd\test\run_dcn_plane_state.ps1 -Root $env:BC250_ROOT
param([string]$Root=$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),
      [string]$Out="$Root\scratch\m9\dcn-plane-state",[string]$Source='',
      [ValidateSet('','SaveEveryFlip','StampIgnored','SyncNotSaved','OnPipeIgnoresPitch','StagedKept','ModesetNoRestore')][string]$Mutation='')
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
if(-not $Source){$Source="$repo\driver\kmd\dcn.c"}
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$cl=Join-Path $Root 'toolchain\llvm-23.1.4-win64\bin\clang-cl.exe'
& $cl --version
if ($LASTEXITCODE) { throw 'Explicit clang-cl unavailable' }
$sdk="$Root\toolchain\nuget\microsoft.windows.sdk.cpp\c"
$libs="$Root\toolchain\nuget\microsoft.windows.sdk.cpp.x64\c"
New-Item -ItemType Directory -Force $Out | Out-Null
$env:TEMP="$Root\scratch\tmp"; $env:TMP=$env:TEMP
& python "$PSScriptRoot\generate_dcn_plane_state_test.py" --source $Source --out "$Out\dcn_plane_state_actual.c"
if($LASTEXITCODE -ne 0){throw 'Source extraction failed'}
if($Mutation) {
    $path="$Out\dcn_plane_state_actual.c"
    # The extraction is written with the host's line ends; the anchors have LF.
    $text=[IO.File]::ReadAllText($path).Replace("`r`n","`n")
    $m=@{
        # The mode's requestor is captured at every plane flip, so a flip between planes saves the first plane's.
        SaveEveryFlip=@('status = Bc250Dcn201RequestorLocked(&io, &s->Staged, s->StagedRefKhz, &s->Save);',
                        's->Save.Count = 0; status = Bc250Dcn201RequestorLocked(&io, &s->Staged, s->StagedRefKhz, &s->Save);')
        # An approval of another mode is taken.
        StampIgnored=@('if (Copy->ModeStamp == stamp && Copy->Pitch == Pitch','if ((void)stamp, Copy->Pitch == Pitch')
        # The mode's global sync is not saved, so the full-size flip does not write it back.
        SyncNotSaved=@('if (status >= 0) s->SyncSaved = TRUE;','(void)0;')
        # The same shape at another pitch counts as on the pipe: its requestor is not written.
        OnPipeIgnoresPitch=@('s->Requestor &&
           s->Pitch == Pitch;','s->Requestor && (s->Pitch != 0 || Pitch != 0);')
        # The staged plan outlives its transaction.
        StagedKept=@('    s->StagedValid = FALSE;
    DcnPlaneIo(Device, &io);','    DcnPlaneIo(Device, &io);')
        # modeset.c's program forgets the plane's registers without writing the mode's back.
        ModesetNoRestore=@('if (!PipeProgrammed && (s->Requestor || s->SyncSaved)) {','if (!PipeProgrammed && s->Pitch == 0xFFFFFFFFu) {')
    }[$Mutation]
    $find=$m[0].Replace("`r`n","`n")
    if(([regex]::Matches($text,[regex]::Escape($find))).Count -ne 1){throw "$Mutation mutation anchor changed"}
    [IO.File]::WriteAllText($path,$text.Replace($find,$m[1].Replace("`r`n","`n")))
}
$kmd="$repo\driver\kmd"
& $cl /nologo /TC /W4 /WX /O2 /MT -Wno-multichar -fuse-ld=lld "/I$kmd" "/I$repo\driver\kmd\test" "/I$repo\driver\shim\dml201" "/I$repo\third_party\linux-amdgpu" "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/Fo$Out\" "/Fe$Out\dcn_plane_state_test.exe" "$Out\dcn_plane_state_actual.c" "$kmd\dcn_plane.c" "$kmd\dcn_scale.c" "$repo\driver\shim\bc250_scaler_buffer.c" "$repo\driver\shim\bc250_scaler_filter.c" "$kmd\display_io.c" "$kmd\edid.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'Host build failed'}
& "$Out\dcn_plane_state_test.exe"
exit $LASTEXITCODE
