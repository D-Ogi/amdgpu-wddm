param([Parameter(Mandatory=$true)][string]$Root, [Parameter(Mandatory=$true)][string]$Out,
      [ValidateSet('', 'layer-order', 'chroma-bytes', 'skip-layout', 'skip-color', 'skip-dml', 'describe-ready', 'describe-nv12-storage', 'describe-rgb-span', 'caps-nv12', 'caps-nv12-scale', 'caps-pipes', 'caps-src-width', 'filter-swapped', 'plan-overlap', 'timing-overlap', 'all')][string]$Mutation='')
$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot\..\..\..").Path
$cc=Join-Path $Root 'toolchain\llvm-23.1.4-win64\bin\clang-cl.exe'
$sdk=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp\c\Include\10.0.26100.0'
$libs=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp.x64\c'
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$crt=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$env:TEMP=$Out;$env:TMP=$Out;$env:INCLUDE='';$env:LIB=''
& $cc --version
if($LASTEXITCODE){throw 'Explicit clang-cl unavailable'}
$mutations=@{
 'layer-order'=@('mpc.Plane[pipe].ZOrder = 1u - p->LayerIndex;', 'mpc.Plane[pipe].ZOrder = p->LayerIndex;')
 'chroma-bytes'=@('pipe ? p->ChromaPitchBytes / 2u : 0u', 'pipe ? p->ChromaPitchBytes : 0u')
 'skip-layout'=@('if (!Bc250Nv12LayoutPrepare(&layout,&Plan->Nv12)) goto refused;', '(void)Bc250Nv12LayoutPrepare(&layout,&Plan->Nv12);')
 'skip-color'=@('if (!Bc250CnvcPrepare(&color,&Plan->Color[pipe])) goto refused;', '(void)Bc250CnvcPrepare(&color,&Plan->Color[pipe]);')
 'describe-ready'=@('Plan->Ready = Resident ? 1u : 0u;', 'Plan->Ready = 1u;')
 'describe-nv12-storage'=@('if (!Bc250Nv12StorageScanout(&storage)) goto refused;', '(void)Bc250Nv12StorageScanout(&storage);')
 'describe-rgb-span'=@('span>p->AllocationBytes-p->SurfaceOffset) goto refused;', 'span>~0ull) goto refused;')
 'caps-nv12'=@('if (pipe && (caps->Nv12 == BC250_DCN_NV12_NONE || !caps->MaxSlaveYuvPlanes)) goto refused;', '')
 'caps-nv12-scale'=@('unsigned up = Nv12 ? Caps->MaxUpscaleNv12 : Caps->MaxUpscaleArgb;', 'unsigned up = Caps->MaxUpscaleArgb; (void)Nv12;')
 'caps-pipes'=@('if (caps->Pipes < 2u || caps->MaxSlavePlanes < 1u) goto refused;', '')
 'caps-src-width'=@('> (long long)caps->MaxDownscaleSourceWidth &&', '> 3840 &&')
 'filter-swapped'=@('if (Filter == BC250_OVERLAY_FILTER_AMD) *Kind = BC250_SCALER_FILTER_AMD;', 'if (Filter == BC250_OVERLAY_FILTER_AMD) *Kind = BC250_SCALER_FILTER_LINEAR;')
 'plan-overlap'=@('        Overlaps(Plan,sizeof(*Plan),in.Plane,2u * sizeof(*in.Plane)) ||', '')
 'timing-overlap'=@('        Overlaps(Plan,sizeof(*Plan),in.Timing,sizeof(*in.Timing)) ||', '')
 'skip-dml'=@('if (Bc250Dml201PlanGroup(in.Timing,dml,2,Workspace,WorkspaceBytes,&Plan->Dml) != BC250_DML201_OK) goto refused;', '(void)Bc250Dml201PlanGroup(in.Timing,dml,2,Workspace,WorkspaceBytes,&Plan->Dml);')
}
$names=if($Mutation -eq 'all'){@($mutations.Keys)}elseif($Mutation){@($Mutation)}else{@('tree')}
foreach($name in $names){
 $dir=Join-Path $Out $name
 if(Test-Path -LiteralPath $dir){throw "Retained test directory exists: $dir"}
 New-Item -ItemType Directory -Path $dir | Out-Null
 $source=Join-Path $repo 'driver\kmd\dcn_overlay.c'
 if($name -ne 'tree'){
  $s=[IO.File]::ReadAllText($source);$m=$mutations[$name]
  if(([regex]::Matches($s,[regex]::Escape($m[0]))).Count -ne 1){throw "Mutation anchor changed: $name"}
  $source=Join-Path $dir 'dcn_overlay.c';[IO.File]::WriteAllText($source,$s.Replace($m[0],$m[1]))
 }
 $sources=@($source,"$PSScriptRoot\dcn_overlay_test.c")
 foreach($s in @('dcn_geometry','dcn_surface_layout','dcn_translate','dcn_cnvc_color','dcn_mpc','display_io')){$sources+="$repo\driver\kmd\$s.c"}
 foreach($s in @('bc250_dml201','bc250_scaler_plan','bc250_scaler_geometry','bc250_scaler_buffer','bc250_scaler_filter')){$sources+="$repo\driver\shim\$s.c"}
 & $cc @('/nologo','/TC','/W4','/WX','/O2','/MT','/fp:precise','-fuse-ld=lld',
  "/I$($crt.FullName)\include","/I$sdk\ucrt","/I$repo\driver\kmd","/I$repo\third_party\linux-amdgpu",
  "/Fo$dir\","/Fe$dir\dcn_overlay_test.exe") @sources /link "/LIBPATH:$($crt.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
 if($LASTEXITCODE){throw "Overlay compilation failed: $name"}
 $result=& "$dir\dcn_overlay_test.exe"; $code=$LASTEXITCODE; $result | Write-Output
 if($name -eq 'tree'){if($code){throw 'Overlay assertions failed'}}
 elseif(!$code -or !($result -match '^FAIL line')){throw "Mutation did not fail assertions: $name"}
 else{"mutation $name caught"}
}
