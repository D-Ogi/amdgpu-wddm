param([switch]$DryRun,[switch]$Offline,[string]$ProfilePath='',[string]$Api='',[ValidateSet('','steam','direct')][string]$Launch='')
$ErrorActionPreference='Stop';$d=$PSScriptRoot
if(!$DryRun){. "$d\durable.ps1"}
# Game profile: one start of the game in the console session (the executable, or the running Steam client since 254),
# inside the outer Job. No driver trace, no redirected streams. Bounded here; the outer deadline still governs.
# Game runner (2026-10-01): everything game-specific comes from game-profile.json, staged by run.py from
# scratch\m15\game-runner\profiles (BC250_TRIAL_GAME_PROFILE; default witcher3, the values this script hardcoded).
# -DryRun prints the launch plan and, without -Offline, the read-only gates of this machine; it starts and writes nothing.
$stageConfig=if($DryRun -and !(Test-Path -LiteralPath "$d\config.json")){[pscustomobject]@{}}else{Get-Content "$d\config.json" -Raw|ConvertFrom-Json}
$gp=Get-Content -LiteralPath $(if($ProfilePath){$ProfilePath}else{Join-Path $d 'game-profile.json'}) -Raw|ConvertFrom-Json
$gameApi=if($Api){$Api}elseif($stageConfig.game_profile){[string]$stageConfig.game_profile.api}else{[string]$gp.api}
if(!$gp.apis.$gameApi){throw ('Game API '+$gameApi+' is not in profile '+$gp.id)}
$witness=if($stageConfig.game_profile){$stageConfig.game_profile.witness}else{$gp.apis.$gameApi.witness}
$witcherMenu=$gp.readiness.mode -eq 'witcher3-menu'
$steamClient=[string]$gp.steam.client
$library=[string]$gp.steam.library
$install=[IO.Path]::Combine($library,'common',[string]$gp.steam.install_dir)
$game=if($gp.executable.directory){[IO.Path]::Combine($install,[string]$gp.executable.directory)}else{$install}
$exe=[IO.Path]::Combine($game,[string]$gp.executable.image)
$launchArguments=(@([string]$gp.steam.arguments,[string]$gp.apis.$gameApi.arguments)|Where-Object{$_}) -join ' '
$directArguments=(@([string]$gp.executable.direct_arguments,[string]$gp.apis.$gameApi.arguments)|Where-Object{$_}) -join ' '
# The game's own processes (main and renderers): the running gate, the foreground set, the modules, the exit code.
$gameNames=@(@([string]$gp.processes.main)+@($gp.processes.renderers)|Select-Object -Unique)
$appImage=if($gp.app_profile.image){[string]$gp.app_profile.image}else{[string]$gp.executable.image}
# Images whose M14.1 router decision the profile's rule checks (absent for Witcher 3).
$routerImages=@($gp.router_images|Where-Object{$_})
# The application GPU UMD an allowed router image must reach: the profile's d3d11 witness, the D3D10/11 UMD the router
# serves. A D3D12 session with an allow rule needs it as well: RotTR in DirectX 12 mode opens a D3D11 probe device at
# startup, which on the CPU UMD crashed the game within 1 s (rottr5, 2026-10-01; on the GPU UMD rottr6 ran).
$routerUmd=$gp.apis.d3d11.witness
# The game's own settings the session's API needs (profile apis.<api>.registry), read before the launch.
$registryNeeds=@($gp.apis.$gameApi.registry|Where-Object{$_})
# 100 showed the menu about 115 s after launch; 240 s (118, was 210) leaves the RT world load inside the 300 s trial.
$bound=240
# config.game_seconds (since 161, owner-consented longer trial) adds its excess over 300 s here as in watch.ps1.
if($stageConfig.game_seconds){$bound+=[int]$stageConfig.game_seconds-300}
$watch=@($gp.modules)
$out=Join-Path $d 'game'
# Every note reaches the disk before the next step: a machine-wide stop keeps what was seen (067 kept nothing).
# A dry run creates nothing.
$noteFile=$null
if(!$DryRun){
 if(Test-Path -LiteralPath $out){throw 'Game capture already exists; never overwrite an attempt'}
 $null=New-Item -ItemType Directory -Path $out
 $noteFile=New-Object IO.FileStream((Join-Path $out 'game-log.txt'),[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read,4096,[IO.FileOptions]::WriteThrough)
}
$noteEncoding=New-Object Text.UTF8Encoding($false)
function Note([string]$text){$b=$noteEncoding.GetBytes(('{0:o} {1}' -f [DateTime]::UtcNow,$text)+"`n");$noteFile.Write($b,0,$b.Length);$noteFile.Flush($true)}
function Stop-Requested{try{[bool](Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 2).stop}catch{$false}}
# The KMD's own temperature; -1 means unreadable, never a temperature (066 and 067 read -1 from the other tool).
# The release client (tester.10 on): HKLM\SOFTWARE\amdgpu-wddm\Release InstallRoot\tools\bc250kmd_cli.exe; "clock read"
# goes to the client Resolve-KmdClient names, resolved at the first reading.
# No Release key (the offline dry run on the development PC): no release client; Resolve-KmdClient falls back.
$releaseRoot=[string](Get-ItemProperty 'HKLM:\SOFTWARE\amdgpu-wddm\Release' -Name InstallRoot -ErrorAction SilentlyContinue).InstallRoot
$script:ReleaseCli=if($releaseRoot){Join-Path $releaseRoot 'tools\bc250kmd_cli.exe'}else{''}
# tester.10's release bc250kmd_cli.exe (built from the KMD branch) has no "health" or "clock" command. A query
# form its usage does not list goes to the client that answered that form before the release layout (07147: health,
# 07136: clock), until a release client carries every form (release/tester11-cli).
function Resolve-KmdClient([string]$Release,[string]$Form){
 $ErrorActionPreference='Continue'
 $usage=try{& $Release 2>&1|Out-String}catch{''}
 if($usage.Contains($Form)){return $Release}
 $legacy=if($Form -like 'clock *'){'C:\BC250\m9\candidate07136\client\bc250kmd_cli.exe'}else{'C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe'}
 if(Test-Path -LiteralPath $legacy){return $legacy}
 throw "No KMD client answers '$Form': $Release does not list it and $legacy is absent"
}
$script:ClockCli=''
# The release KMD (tester.11, 0.7.199.1) refuses "clock read" to a non-elevated caller (STATUS_ACCESS_DENIED), and the
# runtime runs as the console user: then the overlay's telemetry (GET /telemetry, the KMD temperature it samples
# elevated) answers, if it is at most 10 s old.
function Temp-Overlay{try{$t=Invoke-RestMethod http://127.0.0.1:2250/telemetry -TimeoutSec 2;if($t.available -and $null -ne $t.temperatureC -and $null -ne $t.ageSeconds -and [double]$t.ageSeconds -le 10){return [double]$t.temperatureC}}catch{};return -1}
function Temp-Now{try{if(!$script:ClockCli){$script:ClockCli=Resolve-KmdClient $script:ReleaseCli 'clock read'};$raw=& $script:ClockCli clock read 2>&1|Out-String;if($raw -match 'temperature_mc=(\d+)'){return [double]$Matches[1]/1000}}catch{};return Temp-Overlay}
$result=[ordered]@{launched=$false;pid=0;session=0;exe_sha256='';exe_version='';elapsed_seconds=0;has_exited=$false;exit_code=$null;bounded_stop=$false;stop_reason='';last_title='';responding_last=$null;peak_ws_mb=0;peak_threads=0;screenshots=0;modules=@{};shadow_dlls=@();settings=@{};saves=@{};errors=@()}
$child=$null;$debugger=$null;$settingsBefore=@{};$gamePids=@()
$result.launch='direct';$result.launch_chain=@();$result.launch_seconds=0;$result.app_profile=''
# The profile's settings policy. backup-restore (Witcher 3: the DX12 build reads and rewrites dx12user.settings, 101
# at its menu; user.settings is the DX11 build's): each file is restored to its before content if the game changed
# it (keeps the owner's 720p). record: hashes and copies before and after, never written. untouched: not read.
$settingsPolicy=[string]$gp.settings.policy
$settingsNames=@($gp.settings.files|Where-Object{$_})
$settingsDir=if($gp.settings.directory){[Environment]::ExpandEnvironmentVariables([string]$gp.settings.directory)}else{''}
# A verified before copy of each file, or its recorded absence; an unreadable file stops the launch (844).
function Capture-Settings{
 foreach($n in $settingsNames){
  $p=Join-Path $settingsDir $n
  if(Test-Path -LiteralPath $p){
   $h=(Get-FileHash -LiteralPath $p).Hash
   Copy-Item -LiteralPath $p -Destination (Join-Path $out ($n+'.before'))
   if((Get-FileHash -LiteralPath (Join-Path $out ($n+'.before'))).Hash -ne $h){throw "Settings before copy mismatch: $n"}
   $settingsBefore[$n]=$h
  }else{$settingsBefore[$n]=''}
  # Unverified until Restore-Settings checks it with the writers gone; a skipped cleanup stays pending (846).
  $result.settings[$n]='pending'
 }
 $result.settings_before=$settingsBefore
}
# Writers gone only when the query itself succeeded: only NoProcessFoundForGivenName errors mean absence (836, 846).
# The session passes the profile's writers; the default is the Witcher 3 pair.
function Writers-Gone([string[]]$Names=@('witcher3','cdb')){
 $queryErrors=$null
 $found=@(Get-Process -Name $Names -ErrorAction SilentlyContinue -ErrorVariable queryErrors)
 if(@($queryErrors|Where-Object {$_.FullyQualifiedErrorId -notlike 'NoProcessFoundForGivenName,*'}).Count){return $false}
 return !$found.Count
}
# Each file back to its before state (present with its hash, or absent), each on its own; changed or new content is
# kept first as <name>.after. With a writer possibly alive no file gets a final status, equal or not: every
# captured file stays restore-pending and is left as found (846).
function Restore-Settings([bool]$writersGone){
 foreach($n in @($settingsBefore.Keys)){
  try{
   $p=Join-Path $settingsDir $n;$b=$settingsBefore[$n]
   $now=if(Test-Path -LiteralPath $p){(Get-FileHash -LiteralPath $p).Hash}else{''}
   if($now -and $now -ne $b){Copy-Item -LiteralPath $p -Destination (Join-Path $out ($n+'.after'))}
   if(!$writersGone){$result.settings[$n]='restore-pending';Note ('settings '+$n+': game not seen gone, restore pending');continue}
   if($now -eq $b){$result.settings[$n]='unchanged';continue}
   if($b){Copy-Item -LiteralPath (Join-Path $out ($n+'.before')) -Destination $p -Force}else{Remove-Item -LiteralPath $p -Force}
   $final=if(Test-Path -LiteralPath $p){(Get-FileHash -LiteralPath $p).Hash}else{''}
   $result.settings[$n]=if($final -eq $b){'restored'}else{'restore-failed'}
   Note ('settings '+$n+' '+$result.settings[$n])
  }catch{$result.settings[$n]='restore-failed';$result.errors+=,('settings '+$n+': '+$_.Exception.Message)}
 }
}
# record: each named file's hash (or its absence) and a copy <name>.<when>, before the launch and after the game
# ended, in $result.settings_record. Nothing is written back, so $result.settings stays empty. Not part of the
# verdict: a file that cannot be read is recorded as unreadable, never fatal.
function Record-Settings([string]$when){
 if(!$result.settings_record){$result.settings_record=[ordered]@{}}
 $r=[ordered]@{}
 foreach($n in $settingsNames){
  $p=Join-Path $settingsDir $n
  if(Test-Path -LiteralPath $p){
   try{$r[$n]=(Get-FileHash -LiteralPath $p).Hash;Copy-Item -LiteralPath $p -Destination (Join-Path $out ($n+'.'+$when))}catch{$r[$n]='unreadable: '+$_.Exception.Message}
  }else{$r[$n]=''}
 }
 $result.settings_record[$when]=$r
 Note ('settings '+$when+': '+(@($r.Keys|ForEach-Object{$_+' '+$(if(!$r[$_]){'absent'}elseif($r[$_] -like 'unreadable*'){$r[$_]}else{$r[$_].Substring(0,16)})}) -join ', '))
}
# The game's save directory, counted before and after (Witcher 3); a profile without one counts nothing.
$saves=if($gp.saves){[Environment]::ExpandEnvironmentVariables([string]$gp.saves)}else{''}
function Save-State{if(!(Test-Path -LiteralPath $saves)){return @{count=0;newest=''}};$f=@(Get-ChildItem -LiteralPath $saves -File);@{count=$f.Count;newest=$(if($f.Count){($f|Sort-Object LastWriteTimeUtc|Select-Object -Last 1).LastWriteTimeUtc.ToString('o')}else{''})}}
# Menu pass (435, 838, 439): the main menu highlights CONTINUE and reads "E Select". The menu signal is the game's
# own settings write: 101 rewrote dx12user/input/profile.settings 2 s after its menu UI appeared (on a black
# background, mean 1.6, which a brightness gate missed). After the first window title a 0.25-scale screenshot is
# taken at most every 8 s; two samples tied to the game's foreground after the signal attempt E once, by scan code.
# If 8 s later the 8x6 grid still matches the pre-attempt one (menu to menu 0.9-1.5), attempt again, at most three
# attempts in all. A changed grid is only a transition candidate; the screenshots of loading and the world are the
# evidence.
# (A dry run sends no input and compiles nothing.)
if(!$DryRun){Add-Type -Namespace Bc -Name Input -ReferencedAssemblies System.Drawing -MemberDefinition @'
[StructLayout(LayoutKind.Explicit, Size = 40)] public struct KeyInput { [FieldOffset(0)] public uint type; [FieldOffset(8)] public ushort vk; [FieldOffset(10)] public ushort scan; [FieldOffset(12)] public uint flags; [FieldOffset(16)] public uint time; [FieldOffset(24)] public IntPtr extra; }
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
[DllImport("user32.dll", SetLastError = true)] static extern uint SendInput(uint count, KeyInput[] inputs, int size);
static uint Send(KeyInput[] inputs) { return SendInput(1, inputs, Marshal.SizeOf(typeof(KeyInput))); }
public static uint[] Key(ushort scan) { return KeyWith(Send, scan); }
// 256+ (menus in the running game): an extended key (E0 prefix: arrows 48/50/4B/4D, Home 47, End 4F, PgUp 49, PgDn
// 51, Delete 53), KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY, released like Key.
public static uint[] KeyExtended(ushort scan) {
    KeyInput[] down = { new KeyInput { type = 1, scan = scan, flags = 0x9 } };
    KeyInput[] up = { new KeyInput { type = 1, scan = scan, flags = 0xB } };
    uint d = Send(down), u = 0, retries = 0;
    if (d != 1) return new uint[] { d, 0, 0 };
    System.Threading.Thread.Sleep(80);
    u = Send(up);
    while (u != 1 && retries < 3) { System.Threading.Thread.Sleep(50); retries++; u = Send(up); }
    return new uint[] { d, u, retries };
}
// {down, up, release retries}. An accepted key-down is released: up to three more key-up submissions (839).
public static uint[] KeyWith(Func<KeyInput[], uint> send, ushort scan) {
    KeyInput[] down = { new KeyInput { type = 1, scan = scan, flags = 0x8 } };
    KeyInput[] up = { new KeyInput { type = 1, scan = scan, flags = 0xA } };
    uint d = send(down), u = 0, retries = 0;
    if (d != 1) return new uint[] { d, 0, 0 };
    System.Threading.Thread.Sleep(80);
    u = send(up);
    while (u != 1 && retries < 3) { System.Threading.Thread.Sleep(50); retries++; u = send(up); }
    return new uint[] { d, u, retries };
}
// World phase (143+, owner: Geralt walks and looks around): a key held for ms then released like Key, and
// relative mouse motion (INPUT_MOUSE, MOUSEEVENTF_MOVE) in n steps of (dx, dy) every ms; Look returns the
// number of accepted steps.
[StructLayout(LayoutKind.Explicit, Size = 40)] public struct MouseInput { [FieldOffset(0)] public uint type; [FieldOffset(8)] public int dx; [FieldOffset(12)] public int dy; [FieldOffset(16)] public uint data; [FieldOffset(20)] public uint flags; [FieldOffset(24)] public uint time; [FieldOffset(32)] public IntPtr extra; }
[DllImport("user32.dll", SetLastError = true, EntryPoint = "SendInput")] static extern uint SendMouseInput(uint count, MouseInput[] inputs, int size);
public static uint[] Hold(ushort scan, int ms) {
    KeyInput[] down = { new KeyInput { type = 1, scan = scan, flags = 0x8 } };
    KeyInput[] up = { new KeyInput { type = 1, scan = scan, flags = 0xA } };
    uint d = Send(down), u = 0, retries = 0;
    if (d != 1) return new uint[] { d, 0, 0 };
    System.Threading.Thread.Sleep(ms);
    u = Send(up);
    while (u != 1 && retries < 3) { System.Threading.Thread.Sleep(50); retries++; u = Send(up); }
    return new uint[] { d, u, retries };
}
// Interactive session (166+): a mouse button press, MOUSEEVENTF_*DOWN then *UP after ms.
public static uint Click(uint downFlag, uint upFlag, int ms) {
    MouseInput[] down = { new MouseInput { type = 0, flags = downFlag } };
    MouseInput[] up = { new MouseInput { type = 0, flags = upFlag } };
    uint ok = SendMouseInput(1, down, Marshal.SizeOf(typeof(MouseInput)));
    System.Threading.Thread.Sleep(ms);
    ok += SendMouseInput(1, up, Marshal.SizeOf(typeof(MouseInput)));
    return ok;
}
// 256+ (general pilot): the cursor to a point given as fractions of the primary screen (MOUSEEVENTF_MOVE |
// MOUSEEVENTF_ABSOLUTE, 0..65535), so a click can land on a text box the lab-side OCR found.
public static uint Point(double fx, double fy) {
    MouseInput[] m = { new MouseInput { type = 0, dx = (int)Math.Round(Math.Max(0, Math.Min(1, fx)) * 65535), dy = (int)Math.Round(Math.Max(0, Math.Min(1, fy)) * 65535), flags = 0x8001 } };
    return SendMouseInput(1, m, Marshal.SizeOf(typeof(MouseInput)));
}
public static uint Look(int dx, int dy, int n, int ms) {
    uint ok = 0;
    for (int i = 0; i < n; i++) {
        MouseInput[] m = { new MouseInput { type = 0, dx = dx, dy = dy, flags = 0x0001 } };
        ok += SendMouseInput(1, m, Marshal.SizeOf(typeof(MouseInput)));
        System.Threading.Thread.Sleep(ms);
    }
    return ok;
}
public static double[] Stats(string path) {
    using (var file = new System.IO.FileStream(path, System.IO.FileMode.Open, System.IO.FileAccess.Read))
    using (var source = new System.Drawing.Bitmap(file))
    using (var bmp = source.Clone(new System.Drawing.Rectangle(0, 0, source.Width, source.Height), System.Drawing.Imaging.PixelFormat.Format24bppRgb)) {
        var data = bmp.LockBits(new System.Drawing.Rectangle(0, 0, bmp.Width, bmp.Height), System.Drawing.Imaging.ImageLockMode.ReadOnly, System.Drawing.Imaging.PixelFormat.Format24bppRgb);
        try {
            int w = bmp.Width, h = bmp.Height, stride = Math.Abs(data.Stride);
            var buf = new byte[stride * h];
            Marshal.Copy(data.Scan0, buf, 0, buf.Length);
            var r = new double[49]; var n = new int[48]; double total = 0;
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
                int o = y * stride + x * 3; double l = 0.114 * buf[o] + 0.587 * buf[o + 1] + 0.299 * buf[o + 2];
                int c = (y * 6 / h) * 8 + (x * 8 / w); total += l; r[1 + c] += l; n[c]++;
            }
            r[0] = total / (w * h);
            for (int c = 0; c < 48; c++) r[1 + c] = n[c] > 0 ? r[1 + c] / n[c] : 0;
            return r;
        } finally { bmp.UnlockBits(data); }
    }
}
'@}
$menu=[ordered]@{samples=0;untied=0;failed_captures=0;settings_write_seen_at=$null;settings_write_cue='';settings_writes=@();tied_run=0;attempts=0;injected=0;attempt_times=@();key_results=@();release_unresolved=$false;foreground_refusals=0;changed=$false;changed_at=0;diffs=@();means=@();errors=@()}
$menuDir=Join-Path $out 'menu';$pressSig=$null;$pressAt=0;$lastSample=-100;$utcStart=$null;$windowUtc=$null;$seenWrites=@{}
# Every settings write after launch is noted; only one at least 20 s after the first window title counts as the
# menu signal, so a write while the game starts up cannot trigger the pass.
# A write is an exploratory input cue, not a measured "menu reached" (844): menu, loading and world claims stay with
# the images. Returns the first qualifying write as "name mtime", or nothing.
function Menu-Signal([int]$t){
 if(!$utcStart -or !$windowUtc){return}
 $w=@(Get-ChildItem -LiteralPath $settingsDir -Filter '*.settings' -File -ErrorAction SilentlyContinue|Where-Object{$_.LastWriteTimeUtc -gt $utcStart})
 foreach($f in $w){
  if($seenWrites[$f.Name] -ne $f.LastWriteTimeUtc){$seenWrites[$f.Name]=$f.LastWriteTimeUtc;$menu.settings_writes+=,@($f.Name,$f.LastWriteTimeUtc.ToString('o'),$t);Note ('menu '+$t+'s: '+$f.Name+' written '+$f.LastWriteTimeUtc.ToString('o'))}
 }
 $q=@($w|Where-Object{$_.LastWriteTimeUtc -ge $windowUtc.AddSeconds(20)}|Sort-Object LastWriteTimeUtc)
 if($q.Count){return ($q[0].Name+' '+$q[0].LastWriteTimeUtc.ToString('o'))}
}
# The game owns the foreground: its process, or with a generic profile any of its processes seen last (main and
# renderers, $gamePids; empty on the Witcher 3 route).
function Game-Foreground{$p=[uint32]0;$null=[Bc.Input]::GetWindowThreadProcessId([Bc.Input]::GetForegroundWindow(),[ref]$p);if($gamePids.Count){return [int]$p -in $gamePids};return [int]$p -eq $child.Id}
# At most one activation of the game's main window per call (a renderer's window when the main process has none).
function Focus-Game{
 if(Game-Foreground){return $true}
 $child.Refresh();$h=$child.MainWindowHandle
 if($h -eq [IntPtr]::Zero -and $gamePids.Count){$w=@(Get-Process -Id $gamePids -ErrorAction SilentlyContinue|Where-Object{$_.MainWindowHandle -ne [IntPtr]::Zero})|Select-Object -First 1;if($w){$h=$w.MainWindowHandle}}
 if($h -ne [IntPtr]::Zero){$null=[Bc.Input]::SetForegroundWindow($h);Start-Sleep -Milliseconds 300}
 return (Game-Foreground)
}
function Send-E{return ,[Bc.Input]::Key(0x12)}
# The input guard holds at the moment of input. An attempt is bounded (three in all); only SendInput 2/2 counts as
# injected (838), and no press is inferred from 0/1.
function Press-E([int]$t,$sig){
 if(!(Game-Foreground)){$menu.foreground_refusals++;Note ('menu '+$t+'s: game not foreground, no input');return}
 $k=Send-E
 $menu.attempts++;$menu.attempt_times+=,$t;$menu.key_results+=,@($t,$k[0],$k[1],$k[2])
 if($k[0] -eq 1 -and $k[1] -eq 1){$menu.injected++}
 # A key-down without its key-up stays injected after the game ends: record it and stop the pass.
 if($k[0] -eq 1 -and $k[1] -ne 1){$menu.release_unresolved=$true}
 $script:pressSig=$sig;$script:pressAt=$timer.Elapsed.TotalSeconds
 Note ('menu '+$t+'s: E attempt '+$menu.attempts+', down '+$k[0]+' up '+$k[1]+' release retries '+$k[2]+$(if($menu.release_unresolved){', RELEASE UNRESOLVED'}else{''}))
}
function Menu-Step([int]$t){
 # 101: a capture every ~4.5 s plus host shots slowed the lab; one capture per 8 s at most, 40 in all.
 if($menu.samples -ge 40 -or $timer.Elapsed.TotalSeconds-$script:lastSample -lt 8){return}
 $script:lastSample=$timer.Elapsed.TotalSeconds
 $active=$menu.attempts -lt 3 -and !$menu.changed -and !$menu.release_unresolved
 $cue=Menu-Signal $t
 if($active -and $null -eq $menu.settings_write_seen_at -and $cue){$menu.settings_write_seen_at=$t;$menu.settings_write_cue=$cue;Note ('menu '+$t+'s: settings_write_seen '+$cue)}
 # The desktop can be in front: a sample speaks for the game only if the game owned the foreground before and
 # after the capture (838); an untied sample resets the run and is never a signature.
 $before=if($active -and $null -ne $menu.settings_write_seen_at){Focus-Game}else{Game-Foreground}
 if(!(Test-Path -LiteralPath $menuDir)){$null=New-Item -ItemType Directory -Path $menuDir}
 $file=Join-Path $menuDir ('sample-{0:d3}-{1:d3}s.png' -f $menu.samples,$t)
 $menu.samples++
 Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:2250/screenshot?scale=0.25&format=png&overlay=0' -OutFile $file -TimeoutSec 5
 $tied=$before -and (Game-Foreground)
 $s=[Bc.Input]::Stats($file);$mean=[math]::Round($s[0],1);$menu.means+=,@($t,$mean,$tied)
 if(!$active){Note ('menu '+$t+'s: mean '+$mean+' tied '+$tied);return}
 if(!$tied){$menu.untied++;$menu.tied_run=0;Note ('menu '+$t+'s: mean '+$mean+' not tied to the game foreground, discarded');return}
 if(!$menu.attempts){
  if($null -ne $menu.settings_write_seen_at){$menu.tied_run++}
  Note ('menu '+$t+'s: mean '+$mean+' cue '+$(if($null -ne $menu.settings_write_seen_at){$menu.settings_write_seen_at}else{'none'})+' tied run '+$menu.tied_run)
  if($menu.tied_run -ge 2){Press-E $t $s}
  return
 }
 if($timer.Elapsed.TotalSeconds-$pressAt -lt 8){Note ('menu '+$t+'s: mean '+$mean);return}
 $diff=0.0;for($i=1;$i -le 48;$i++){$diff+=[math]::Abs($s[$i]-$pressSig[$i])};$diff=[math]::Round($diff/48,2)
 $menu.diffs+=,@($t,$diff)
 Note ('menu '+$t+'s: mean '+$mean+' grid diff '+$diff)
 if($diff -ge 4){$menu.changed=$true;$menu.changed_at=$t;Note ('menu '+$t+'s: grid changed, transition candidate')}else{Press-E $t $s}
}
# 143+ (owner, 2026-09-30): once the world is up, Geralt walks and looks around: forward with the key held,
# a full turn, a look up and down, strafes, backwards; each step at most 3 s so the sampling above goes on.
# The world is assumed 25 s after the transition candidate (140: loading took ~50 s after it; the game ignores
# input while loading). Same foreground guard as Press-E; scan codes W 0x11, A 0x1E, S 0x1F, D 0x20.
$walk=[ordered]@{started_at=$null;steps=0;refusals=0;inputs=@();errors=@()}
function Walk-Step([int]$t){
 if(!$menu.changed -or $t-$menu.changed_at -lt 25){return}
 if(!(Focus-Game)){$walk.refusals++;if($walk.refusals -le 3){Note ('walk '+$t+'s: game not foreground, no input')};return}
 if($null -eq $walk.started_at){$walk.started_at=$t;Note ('walk '+$t+'s: start')}
 $r=$null;$what=''
 switch($walk.steps % 8){
  0{$r=[Bc.Input]::Hold(0x11,3000);$what='W 3 s'}
  1{$r=[Bc.Input]::Look(40,0,60,33);$what='turn right 2400 px'}
  2{$r=[Bc.Input]::Hold(0x11,3000);$what='W 3 s'}
  3{$r=@([Bc.Input]::Look(0,-8,25,33),[Bc.Input]::Look(0,16,25,33),[Bc.Input]::Look(0,-8,25,33));$what='look up, down, back'}
  4{$r=[Bc.Input]::Hold(0x1E,2000);$what='A 2 s'}
  5{$r=[Bc.Input]::Hold(0x20,2000);$what='D 2 s'}
  6{$r=[Bc.Input]::Look(-40,0,30,33);$what='turn left 1200 px'}
  7{$r=[Bc.Input]::Hold(0x1F,2000);$what='S 2 s'}
 }
 $walk.steps++;$walk.inputs+=,@($t,$what,($r -join '/'))
 Note ('walk '+$t+'s: '+$what+' -> '+($r -join '/'))
}
# Interactive session (owner, 2026-09-30 16:05Z; config.interactive_game): once the world is up the operator drives
# the game from the development PC. Commands arrive as C:\BC250\tmp\control\<attempt>\cmd-NNN.txt written by
# control-exec.ps1 (one ssh connection per command), actions separated by ';':
#   tap:SCAN | tapx:SCAN | hold:SCAN:MS | look:DX:DY:N:MS | click:left|right:MS | point:FX:FY | wait:MS |
#   shot:SCALE | ocr | note:TEXT | quit
# (256+) tapx sends an extended key (arrows Up 48, Down 50, Left 4B, Right 4D); point moves the cursor to fractions
# of the screen; ocr reads the whole screen at native resolution on the lab (ocr-frame.ps1) and writes
# ocr-NNN.json, text and boxes only: the frame itself is deleted at once.
# SCAN is a hex keyboard scan code (W 11, A 1E, S 1F, D 20, E 12, Esc 01, Space 39, Tab 0F, Enter 1C, M 32,
# Shift 2A, Ctrl 1D). shot saves shot-NNN.jpg (overlay off) at SCALE for the host. Each command ends with
# done-NNN.txt holding one result line per action; quit ends the session (stop_reason operator-quit).
# Game runner: on the Witcher 3 route the channel opens with the menu transition; a generic profile opens it
# readiness.control_after_window_seconds after the game's first window (launchers, dialogs and menus are the
# operator's), there a command of only shot/ocr/note/wait/quit runs without taking the foreground, and note:world
# marks the world (the walk marker below).
$control=[ordered]@{enabled=[bool]$stageConfig.interactive_game;dir='';commands=0;actions=0;shots=0;refusals=0;quit=$false;walk_marked=$false;errors=@()}
if($control.enabled){$control.dir=Join-Path 'C:\BC250\tmp\control' (Split-Path $d -Leaf);if(!$DryRun){$null=New-Item -ItemType Directory -Force -Path $control.dir;. "$d\ocr-frame.ps1"}}
function Run-Action([string]$a){
 $p=$a -split ':'
 switch($p[0]){
  'tap'{$r=[Bc.Input]::Key([uint16][Convert]::ToInt32($p[1],16));return 'tap '+$p[1]+' -> '+($r -join '/')}
  'hold'{$r=[Bc.Input]::Hold([uint16][Convert]::ToInt32($p[1],16),[int]$p[2]);return 'hold '+$p[1]+' '+$p[2]+' ms -> '+($r -join '/')}
  'look'{$r=[Bc.Input]::Look([int]$p[1],[int]$p[2],[int]$p[3],[int]$p[4]);return 'look '+$p[1]+','+$p[2]+' x'+$p[3]+' -> '+$r}
  'click'{$f=if($p[1] -eq 'right'){@(0x0008,0x0010)}else{@(0x0002,0x0004)};$ms=if($p.Count -gt 2){[int]$p[2]}else{80};$r=[Bc.Input]::Click($f[0],$f[1],$ms);return 'click '+$p[1]+' -> '+$r}
  'tapx'{$r=[Bc.Input]::KeyExtended([uint16][Convert]::ToInt32($p[1],16));return 'tapx '+$p[1]+' -> '+($r -join '/')}
  'point'{$fx=[double]::Parse($p[1],[Globalization.CultureInfo]::InvariantCulture);$fy=[double]::Parse($p[2],[Globalization.CultureInfo]::InvariantCulture);$r=[Bc.Input]::Point($fx,$fy);return 'point '+$p[1]+','+$p[2]+' -> '+$r}
  'ocr'{$o=Get-ScreenOcr (Join-Path $control.dir ('ocr-{0:d3}.png' -f $control.commands))
   [IO.File]::WriteAllText((Join-Path $control.dir ('ocr-{0:d3}.json' -f $control.commands)),$o.json,(New-Object Text.UTF8Encoding($false)))
   return 'ocr '+$o.count+' lines '+$o.ms+' ms'}
  'wait'{Start-Sleep -Milliseconds ([int]$p[1]);return 'wait '+$p[1]}
  'shot'{$scale=if($p.Count -gt 1){$p[1]}else{'0.33'};$file=Join-Path $control.dir ('shot-{0:d3}.jpg' -f $control.commands)
   Invoke-WebRequest -UseBasicParsing -Uri ('http://127.0.0.1:2250/screenshot?scale='+$scale+'&format=jpg&quality=60&overlay=0') -OutFile $file -TimeoutSec 8
   $control.shots++;return 'shot '+(Split-Path $file -Leaf)+' '+(Get-Item -LiteralPath $file).Length}
  'note'{Note ('mark '+$a.Substring(5));return 'note '+($a.Substring(5))}
  'quit'{$control.quit=$true;return 'quit'}
  default{return 'unknown action '+$a}
 }
}
# Since 179 (K31): config.game_priority (AboveNormal|High, from BC250_TRIAL_GAME_PRIORITY at Stage) raises the
# game's priority class at the first input command, after loading; the note records the class read back.
function Set-GamePriority{
 $want=[string]$stageConfig.game_priority
 if(!$want){return}
 foreach($g in @(Get-Process -Name @($gp.processes.renderers) -ErrorAction SilentlyContinue)){
  try{$g.PriorityClass=$want;Note ('priority '+$g.Id+' '+$g.PriorityClass)}catch{Note ('priority '+$g.Id+' failed: '+$_.Exception.Message)}
 }
}
# 366: a menu the automatic pass does not read (the account panel of the first start after a boot) held the channel
# shut for the whole session. After 180 s without a transition the channel opens and the menu is the operator's.
$script:menuOperatorNoted=$false
function Control-Open{
 if(!$witcherMenu){return [bool]$generic.channel_open}
 if($menu.changed){return $true}
 if($timer.Elapsed.TotalSeconds -lt 180){return $false}
 if(!$script:menuOperatorNoted){$script:menuOperatorNoted=$true;Note ('control: no menu transition after 180 s, channel open to the operator')}
 return $true
}
function Control-Step([int]$t){
 if(!$control.enabled -or !(Control-Open)){return}
 $pending=@(Get-ChildItem -LiteralPath $control.dir -Filter 'cmd-*.txt' -File|Where-Object{!(Test-Path -LiteralPath ($_.FullName -replace 'cmd-','done-'))}|Sort-Object Name)
 foreach($c in $pending){
  $control.commands++
  if($control.commands -eq 1){Note ('control '+$t+'s: session open')}
  $lines=@()
  $actions=(Get-Content -LiteralPath $c.FullName -Raw).Trim() -split ';'
  # 170: the walk marker (the ETW window B trigger) belongs to the first INPUT command (hold/tap/look/click), not
  # to a note or a screenshot: the operator confirms the world on a shot first, then starts moving. In 170 the
  # marker came from a note sent during the loading screen and window B measured the loading screen.
  # Game runner: a generic profile's channel opens before its menus, so its first input is a menu press; there the
  # operator marks the world with the action note:world once a shot shows it.
  $walkCue=if($witcherMenu -and $menu.changed){'^(hold|tap|look|click):'}else{'^note:world$'}  # 367: an operator-driven menu marks the world with note:world
  if(!$control.walk_marked -and @($actions|Where-Object{$_.Trim() -match $walkCue}).Count){$control.walk_marked=$true;Note ('walk '+$t+'s: start');Set-GamePriority}
  $viewOnly=!$witcherMenu -and !@($actions|Where-Object{$_.Trim() -and $_.Trim() -notmatch '^(shot|ocr|note|wait|quit)(:|$)'}).Count
  if(!$viewOnly -and !(Focus-Game)){$control.refusals++;$lines+=,'refused: game not foreground'}
  else{
   foreach($a in $actions){$a=$a.Trim();if(!$a){continue};$control.actions++
    try{$lines+=,(Run-Action $a)}catch{$lines+=,('error '+$a+': '+$_.Exception.Message);$control.errors+=,$_.Exception.Message}
    if($control.quit){break}
   }
  }
  Note ('control '+$t+'s: '+$c.Name+' '+($lines -join ' | '))
  [IO.File]::WriteAllText(($c.FullName -replace 'cmd-','done-'),(($lines -join "`n")+"`n"),(New-Object Text.UTF8Encoding($false)))
  if($control.quit){break}
 }
}
# Generic readiness (game runner, readiness.mode generic): no menu pass and no automatic walk. Each turn reads the
# game's processes (main and renderers) and notes once each: the first window of any of them, the control channel
# opening, and readiness: one process with a window, the witness module loaded and at least readiness.min_jobs KMD
# jobs of its own (the gfx job lines of game-kernel.log, written next to this script by kmdlog-stream.ps1). The count
# is a lower bound (the stream leaves out older lines of a busy interval); the KMD's Present counters are
# machine-wide, so no per-process present count exists. What was on the screen stays with the images.
$generic=[ordered]@{window_at=$null;window_process='';window_title='';channel_open=$false;channel_at=$null;ready=$false;ready_at=$null;ready_process='';ready_pid=0;ready_jobs=0;jobs=@{};kernel_offset=[int64]0;errors=@()}
$gameModules=@{}
function Get-GameProcesses{@(Get-Process -Name $gameNames -ErrorAction SilentlyContinue)}
function Read-KernelJobs{
 $path=Join-Path $d 'game-kernel.log'
 if(!(Test-Path -LiteralPath $path)){return}
 # The stream keeps the file open for writing (FileShare.Read): read with ReadWrite sharing, whole lines only.
 $fs=New-Object IO.FileStream($path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
 try{
  $left=$fs.Length-$generic.kernel_offset
  if($left -le 0){return}
  $want=[int][Math]::Min([int64]4MB,$left)
  $buf=New-Object byte[] $want
  $null=$fs.Seek($generic.kernel_offset,[IO.SeekOrigin]::Begin)
  $got=$fs.Read($buf,0,$want)
  if($got -le 0){return}
  $end=[Array]::LastIndexOf($buf,[byte]10,$got-1)
  if($end -lt 0){if($got -eq 4MB){$generic.kernel_offset+=$got};return}
  $generic.kernel_offset+=$end+1
  foreach($m in [regex]::Matches([Text.Encoding]::UTF8.GetString($buf,0,$end+1),'gfx: job seq \d+ .*? pid (\d+) ')){
   $k=$m.Groups[1].Value
   $generic.jobs[$k]=1+$(if($generic.jobs.ContainsKey($k)){[int]$generic.jobs[$k]}else{0})
  }
 }finally{$fs.Dispose()}
}
function Generic-Step([int]$t,$procs){
 $script:gamePids=@($procs|ForEach-Object{$_.Id})
 if($null -eq $generic.window_at){
  $w=@($procs|Where-Object{$_.MainWindowTitle})|Select-Object -First 1
  if($w){$generic.window_at=$t;$generic.window_process=$w.ProcessName+' '+$w.Id;$generic.window_title=$w.MainWindowTitle;Note ('game window '+$t+'s: '+$generic.window_process+' "'+$w.MainWindowTitle+'"')}
 }
 if($control.enabled -and !$generic.channel_open -and $null -ne $generic.window_at -and $t-$generic.window_at -ge [int]$gp.readiness.control_after_window_seconds){
  $generic.channel_open=$true;$generic.channel_at=$t;Note ('control '+$t+'s: channel open, '+$generic.window_process+' "'+$generic.window_title+'"')
 }
 if($generic.ready){return}
 Read-KernelJobs
 foreach($p in $procs){
  $k=[string]$p.Id
  $jobs=if($generic.jobs.ContainsKey($k)){[int]$generic.jobs[$k]}else{0}
  if($p.MainWindowTitle -and $gameModules.ContainsKey($k) -and $gameModules[$k].ContainsKey([string]$witness.module) -and $jobs -ge [int]$gp.readiness.min_jobs){
   $generic.ready=$true;$generic.ready_at=$t;$generic.ready_process=$p.ProcessName;$generic.ready_pid=$p.Id;$generic.ready_jobs=$jobs
   Note ('ready '+$t+'s: '+$p.ProcessName+' pid '+$p.Id+' window "'+$p.MainWindowTitle+'" module '+$witness.module+' jobs '+$jobs)
   return
  }
 }
}
# The M14.1 application router's answer for an image, read as bc250d3d_router.dll reads it at OpenAdapter10_2
# (scratch\m15\app-route\router\router-policy.h DecideApp, router.cpp ReadAppConfig): 'gpu <reason>' or
# 'cpu <reason>'. Mode and GpuUmdPath count only as REG_SZ, the kind approute.py writes; any other kind reads as
# invalid here.
function Open-RouterKey{[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SOFTWARE\amdgpu-wddm\AppRouter')}
function Get-RouterDecision([string]$image){
 $key=Open-RouterKey
 if(!$key){return 'cpu app-mode-cpu (no AppRouter key)'}
 try{
  $names=@($key.GetValueNames());$kind=@{}
  foreach($n in 'Mode','GpuUmdPath','Allow','Deny'){$kind[$n]=if($names -contains $n){[string]$key.GetValueKind($n)}else{''}}
  $mode=if(!$kind.Mode){'cpu'}elseif($kind.Mode -eq 'String'){([string]$key.GetValue('Mode')).ToLowerInvariant()}else{'invalid'}
  if($mode -notin 'cpu','allowlist','gpu-default'){$mode='invalid'}
  # A list that exists but is not a multi-string makes the whole mode invalid (the router's fail-safe).
  if($mode -ne 'cpu' -and (($kind.Allow -and $kind.Allow -ne 'MultiString') -or ($kind.Deny -and $kind.Deny -ne 'MultiString'))){$mode='invalid'}
  if($mode -eq 'cpu'){return 'cpu app-mode-cpu'}
  if($mode -eq 'invalid'){return 'cpu app-mode-invalid'}
  if($image -in 'logonui.exe','consent.exe','lockapp.exe','credentialuibroker.exe','winlogon.exe'){return 'cpu app-protected'}
  $deny=if($kind.Deny){@($key.GetValue('Deny'))}else{@()}
  $allow=if($kind.Allow){@($key.GetValue('Allow'))}else{@()}
  if($deny -contains $image){return 'cpu app-denied'}
  if($mode -eq 'allowlist' -and $allow -notcontains $image){return 'cpu app-not-allowed'}
  $gpu=if($kind.GpuUmdPath -eq 'String'){[string]$key.GetValue('GpuUmdPath')}else{''}
  if($gpu -notmatch '^[A-Za-z]:\\'){return 'cpu app-gpu-umd-unset'}
  if($mode -eq 'allowlist'){return 'gpu app-allowed'};return 'gpu app-default'
 }finally{$key.Close()}
}
# The application GPU UMD the router loads for an allowed image: GpuUmdPath (REG_SZ, as above) and that file's hash,
# '' for either when unset or absent.
function Get-RouterGpuUmd{
 $key=Open-RouterKey
 if(!$key){return @('','')}
 try{$path=if(@($key.GetValueNames()) -contains 'GpuUmdPath' -and [string]$key.GetValueKind('GpuUmdPath') -eq 'String'){[string]$key.GetValue('GpuUmdPath')}else{''}}finally{$key.Close()}
 $hash=if($path -and (Test-Path -LiteralPath $path -PathType Leaf)){(Get-FileHash -LiteralPath $path).Hash}else{''}
 return @($path,$hash)
}
# The router's GPU UMD is the profile's d3d11 witness only with the witness's file name and pinned hash: watch.ps1's
# runtime_entered compares both in a D3D11 session, and the RotTR smoke of 2026-10-01 ran on the app-route-throttle
# UMD (7E9F1692) while the profiles pinned app-route-001's (E748418C).
function Test-RouterUmd($umd){[IO.Path]::GetFileName([string]$umd[0]) -eq [string]$routerUmd.module -and $umd[1] -and $umd[1] -eq [string]$routerUmd.sha256}
# The profile's router rule, before the launch: allow = the first router image must reach the application GPU UMD
# (a D3D11 game on our driver, or a D3D12 game's D3D11 probe device) and that UMD must be the profile's d3d11
# witness; not-allow = no router image may (a D3D12 game would load a second amdgpu_wddm_radv.dll through a D3D11
# device of its own); unchecked = not read (Witcher 3 is on the router's built-in deny list).
function Router-Gate{
 $rule=[string]$gp.apis.$gameApi.router
 if($rule -eq 'unchecked'){return}
 $result.router=[ordered]@{rule=$rule;images=[ordered]@{}}
 foreach($image in $routerImages){$result.router.images[[string]$image]=Get-RouterDecision $image}
 Note ('router '+$rule+': '+(@($result.router.images.Keys|ForEach-Object{$_+' '+$result.router.images[$_]}) -join ', '))
 $first=[string]$routerImages[0]
 if($rule -eq 'allow' -and $result.router.images[$first] -notlike 'gpu *'){throw ('App router keeps '+$first+' on the CPU UMD ('+$result.router.images[$first]+'): set the allowlist first (profile app_router.add)')}
 if($rule -eq 'not-allow' -and @($result.router.images.Values|Where-Object{$_ -like 'gpu *'}).Count){throw 'App router sends an image of this D3D12 game to the D3D11 GPU UMD (a second amdgpu_wddm_radv.dll): remove it from the allowlist first'}
 if($rule -eq 'allow'){
  $umd=Get-RouterGpuUmd
  $result.router.gpu_umd=[ordered]@{path=[string]$umd[0];sha256=[string]$umd[1]}
  Note ('router gpu umd '+$umd[0]+' '+$(if($umd[1]){$umd[1]}else{'no file'}))
  if(!(Test-RouterUmd $umd)){throw ('App router GPU UMD '+$umd[0]+' '+$(if($umd[1]){$umd[1].Substring(0,8)}else{'(no file)'})+' is not the d3d11 witness '+$routerUmd.module+' '+([string]$routerUmd.sha256).Substring(0,8)+': point GpuUmdPath at the witness file or pin the profile''s d3d11 witness to this UMD')}
 }
}
# The game's own settings that the session's API needs, read only, before the launch (profile apis.<api>.registry: a
# key below the current user's hive - the console user's in a session, the SSH user's in a dry run, both the lab's
# account - a value name, the DWORD wanted and the game's own default for an absent value). The key itself, then its
# subkeys breadth first: the first key holding the name counts, the one smoke\rottr-settings.ps1 sets. RotTR:
# EnableDX12, its DirectX 12 option; a session on another API than the game's option measures nothing.
function Open-UserKey([string]$path){[Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($path)}
function Read-UserValue($need){
 $open=New-Object Collections.ArrayList
 try{
  $queue=New-Object Collections.Queue
  $k=Open-UserKey ([string]$need.key)
  if($k){$null=$open.Add($k);$queue.Enqueue($k)}
  while($queue.Count){
   $k=$queue.Dequeue()
   if(@($k.GetValueNames()) -contains [string]$need.name){return [pscustomobject]@{where=[string]$k.Name;value=$k.GetValue([string]$need.name)}}
   foreach($s in @($k.GetSubKeyNames())){$sub=$k.OpenSubKey($s);if($sub){$null=$open.Add($sub);$queue.Enqueue($sub)}}
  }
  return [pscustomobject]@{where='';value=$null}
 }finally{foreach($k in $open){$k.Close()}}
}
function Test-RegistryNeeds{
 foreach($need in $registryNeeds){
  $found=Read-UserValue $need
  $value=if($null -ne $found.value){$found.value}else{$need.default}
  [pscustomobject]@{need=$need;where=$found.where;found=$found.value;ok=($null -ne $value -and [string]$value -eq [string]$need.value)}
 }
}
function Registry-Text($r){if($null -ne $r.found){[string]$r.found+' at '+($r.where -replace '^HKEY_CURRENT_USER\\','HKCU\')}elseif($null -ne $r.need.default){'absent (the game''s default '+$r.need.default+')'}else{'absent'}}
function Registry-Gate{
 if(!$registryNeeds.Count){return}
 $checked=@(Test-RegistryNeeds)
 $result.registry=@($checked|ForEach-Object{[ordered]@{name=[string]$_.need.name;key=[string]$_.need.key;want=$_.need.value;found=$_.found;where=[string]$_.where;ok=$_.ok}})
 foreach($r in $checked){Note ('registry '+$r.need.name+' '+(Registry-Text $r)+'; '+$gameApi+' needs '+$r.need.value)}
 $bad=@($checked|Where-Object{!$_.ok})
 if($bad.Count){throw ((@($bad|ForEach-Object{$_.need.name+' is '+(Registry-Text $_)+$(if($null -eq $_.found){' under HKCU\'+$_.need.key}else{''})}) -join '; ')+': the '+$gameApi+' session needs '+(@($bad|ForEach-Object{$_.need.name+' '+$_.need.value}) -join ', ')+'; set it before the session (profile apis.'+$gameApi+'.selection)')}
}
# StateFlags and build id of the profile's app from its Steam manifest.
function Read-SteamManifest{
 $acf=Get-Content -LiteralPath ([IO.Path]::Combine($library,'appmanifest_'+$gp.app_id+'.acf')) -Raw
 $flags=if($acf -match '"StateFlags"\s+"(\d+)"'){[int]$Matches[1]}else{-1}
 $build=if($acf -match '"buildid"\s+"(\d+)"'){$Matches[1]}else{''}
 return @($flags,$build)
}
if($DryRun){
 # The plan this script would run, then (without -Offline) the read-only gates on this machine: nothing is started,
 # created or written; the lab variant runs on a copy outside any attempt.
 $launchMode=if($Launch){$Launch}elseif($stageConfig.launch -eq 'steam'){'steam'}else{'direct'}
 $routerRule=[string]$gp.apis.$gameApi.router
 'profile '+$gp.id+' app '+$gp.app_id+' api '+$gameApi+' readiness '+$gp.readiness.mode+' settings '+$settingsPolicy
 if($launchMode -eq 'steam'){'launch steam: "'+$steamClient+'" '+('-applaunch '+$gp.app_id+' '+$launchArguments).Trim()}
 else{'launch direct: "'+$exe+'"'+$(if($directArguments){' '+$directArguments}else{''})+' in "'+$game+'"'}
 'environment SteamAppId='+$gp.app_id+' SteamGameId='+$gp.app_id+' AMDGPU_WDDM_D3D12_EXPERIMENT='+$(if($stageConfig.experiment){$stageConfig.experiment}else{'none'})
 if($launchMode -eq 'steam'){
  'steam wait '+$gp.steam.wait_seconds+' s for '+(@($gp.processes.launch_chain) -join ',')+'; manifest "'+[IO.Path]::Combine($library,'appmanifest_'+$gp.app_id+'.acf')+'" StateFlags '+$gp.steam.require_state_flags
  'app profile HKLM:\SOFTWARE\amdgpu-wddm\D3D12\Applications\'+$appImage
 }
 'game processes '+($gameNames -join ',')+'; main '+$gp.processes.main+'; end at cleanup '+(@($gp.processes.end_at_cleanup) -join ',')+'; survivors '+(@($gp.processes.survivors) -join ',')+'; settings writers '+(@($gp.processes.writers) -join ',')
 'router '+$routerRule+$(if($routerRule -ne 'unchecked'){' '+($routerImages -join ',')}else{''})+$(if($routerRule -eq 'allow'){'; GPU UMD the d3d11 witness '+$routerUmd.module+' '+$routerUmd.sha256}else{''})
 foreach($need in $registryNeeds){'registry HKCU\'+$need.key+' '+$need.name+' = '+$need.value+$(if($null -ne $need.default){' (absent: the game''s default '+$need.default+')'}else{''})}
 # Offline the profile's own paths: their variables belong to the lab's account, not to this machine's.
 'settings '+$settingsPolicy+$(if($settingsNames.Count){' "'+$(if($Offline){[string]$gp.settings.directory}else{$settingsDir})+'" '+($settingsNames -join ',')}else{''})
 'saves '+$(if($saves){'"'+$(if($Offline){[string]$gp.saves}else{$saves})+'"'}else{'none'})
 'modules '+($watch -join ',')
 'witness '+$witness.module+' '+$witness.sha256
 'bound '+$bound+' s; control '+$(if(!$control.enabled){'off'}elseif($witcherMenu){'after the menu transition'}else{'open '+$gp.readiness.control_after_window_seconds+' s after the first game window'})
 if(!$witcherMenu){'readiness: a game process with a window, '+$witness.module+' loaded and '+$gp.readiness.min_jobs+' KMD jobs'}
 if($Offline){exit 0}
 $fail=0
 function Gate([string]$name,[bool]$ok,[string]$detail){if(!$ok){$script:fail++};'{0} {1}: {2}' -f $(if($ok){'PASS'}else{'FAIL'}),$name,$detail}
 $present=Test-Path -LiteralPath $exe
 Gate 'executable' $present $exe
 if($present){'exe sha256 '+(Get-FileHash -LiteralPath $exe).Hash+' version '+[string](Get-Item -LiteralPath $exe).VersionInfo.FileVersion}
 $running=@(Get-GameProcesses)
 Gate 'not running' (!$running.Count) $(if($running.Count){@($running|ForEach-Object{$_.ProcessName+' '+$_.Id}) -join ', '}else{'none of '+($gameNames -join ',')})
 $shadow=@(@('d3d12.dll','d3d12core.dll','dxgi.dll','d3d11.dll')|Where-Object{Test-Path -LiteralPath (Join-Path $game $_)})
 Gate 'no replacement DLL' (!$shadow.Count) $(if($shadow.Count){$shadow -join ','}else{'none in '+$game})
 # Not a gate: a C++ runtime next to the executable is the copy every in-process DLL importing it by name gets (RotTR
 # 825e4a49: the game's old msvcp140.dll under a newer STL's mutex). The session's msvcp140.dll module note shows it.
 $crt=@(@('msvcp140.dll','vcruntime140.dll','vcruntime140_1.dll')|Where-Object{Test-Path -LiteralPath (Join-Path $game $_)})
 'game-local C++ runtime '+$(if($crt.Count){@($crt|ForEach-Object{$_+' '+[string](Get-Item -LiteralPath (Join-Path $game $_)).VersionInfo.FileVersion}) -join ', '}else{'none in '+$game})
 $mb=[int](New-Object Diagnostics.PerformanceCounter('Memory','Available MBytes')).NextValue()
 Gate 'available memory' ($mb -ge 3500) ([string]$mb+' MB of 3500 (the low-memory pre-step otherwise)')
 $tempNow=Temp-Now
 Gate 'temperature' ($tempNow -ge 0 -and $tempNow -lt 80) ('tctl '+$tempNow+' through '+$script:ClockCli)
 if($launchMode -eq 'steam'){
  $sp=@(Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($steamClient)) -ErrorAction SilentlyContinue).Count
  Gate 'steam client running' ([bool]$sp) ([string]$sp+' processes')
  $m=try{Read-SteamManifest}catch{@(-1,'')}
  Gate 'steam state' ($m[0] -eq [int]$gp.steam.require_state_flags) ('StateFlags '+$m[0]+' build '+$m[1])
  'app profile '+$(try{[string](Get-ItemProperty -LiteralPath ('HKLM:\SOFTWARE\amdgpu-wddm\D3D12\Applications\'+$appImage) -Name Experiment -ErrorAction Stop).Experiment}catch{'none'})
 }
 foreach($image in $routerImages){
  $decision=Get-RouterDecision $image
  if($routerRule -eq 'allow' -and $image -eq [string]$routerImages[0]){Gate ('router '+$image) ($decision -like 'gpu *') $decision}
  elseif($routerRule -eq 'not-allow'){Gate ('router '+$image) ($decision -notlike 'gpu *') $decision}
  else{'router '+$image+' '+$decision}
 }
 if($routerRule -eq 'allow'){
  $umd=Get-RouterGpuUmd
  Gate 'router gpu umd' (Test-RouterUmd $umd) ($(if($umd[0]){$umd[0]}else{'GpuUmdPath unset'})+' '+$(if($umd[1]){$umd[1].Substring(0,8)}else{'(no file)'})+'; d3d11 witness '+$routerUmd.module+' '+([string]$routerUmd.sha256).Substring(0,8))
 }
 foreach($r in @(Test-RegistryNeeds)){Gate ('registry '+$r.need.name) $r.ok ((Registry-Text $r)+'; '+$gameApi+' needs '+$r.need.value+' (user '+[Security.Principal.WindowsIdentity]::GetCurrent().Name+')')}
 foreach($n in $settingsNames){$p=Join-Path $settingsDir $n;'settings '+$n+' '+$(if(Test-Path -LiteralPath $p){(Get-FileHash -LiteralPath $p).Hash.Substring(0,16)}else{'absent'})}
 if($saves){'saves '+(Save-State).count}
 if($fail){'dry run: '+$fail+' gate(s) fail';exit 1}
 'dry run: all gates pass';exit 0
}
try{
 if(!(Test-Path -LiteralPath $exe)){throw 'Game executable missing'}
 if(Get-Process -Name $gameNames -ErrorAction SilentlyContinue){throw 'Game already running'}
 foreach($n in @('d3d12.dll','d3d12core.dll','dxgi.dll','d3d11.dll')){if(Test-Path -LiteralPath (Join-Path $game $n)){$result.shadow_dlls+=,$n}}
 if($result.shadow_dlls.Count){throw 'Replacement DLL next to the game executable; not a native run'}
 $result.exe_sha256=(Get-FileHash -LiteralPath $exe).Hash
 $result.exe_version=[string](Get-Item -LiteralPath $exe).VersionInfo.FileVersion
 if($settingsPolicy -eq 'backup-restore'){Capture-Settings}elseif($settingsPolicy -eq 'record'){Record-Settings 'before'}
 if($saves){$result.saves.before=Save-State}
 # 101 started with 2898 MB available and paged at the menu (616-750 MB); the owner's low-memory pre-step
 # (Edge closed, Steam lighter) brings it to about 4.2 GB.
 $result.available_mb_before=[int](New-Object Diagnostics.PerformanceCounter('Memory','Available MBytes')).NextValue()
 Note ('available_mb_before '+$result.available_mb_before)
 $result.admission_min_available_mb=3500
 if($result.available_mb_before -lt $result.admission_min_available_mb){throw 'Less than 3500 MB available; run the low-memory pre-step'}
 $result.temperature_before=Temp-Now
 Note ('temperature_before '+$result.temperature_before+' through '+$script:ClockCli)
 if($result.temperature_before -lt 0 -or $result.temperature_before -ge 80){throw ('Temperature before launch unreadable or at the limit: '+$result.temperature_before)}
 Router-Gate
 Registry-Gate
 # Started outside the Steam client: the game's Steam module needs its application id (066 ended in it).
 $env:SteamAppId=[string]$gp.app_id;$env:SteamGameId=[string]$gp.app_id
 # One named driver deviation for this measurement (config.experiment, set at Stage from BC250_TRIAL_EXPERIMENT),
 # inherited by the game as by the client in runtime.ps1. 105: present-cached.
 $config=Get-Content "$d\config.json" -Raw|ConvertFrom-Json
 $result.experiment=if($config.PSObject.Properties.Name -contains 'experiment'){[string]$config.experiment}else{''}
 # adapter109+: without the variable the shell reads the image's application profile (HKLM\SOFTWARE\amdgpu-wddm\
 # D3D12\Applications\witcher3.exe), so a trial without an experiment says "none" and stays independent of it.
 # Older shells read "none" as an unknown name, i.e. no switch, as before.
 $env:AMDGPU_WDDM_D3D12_EXPERIMENT=if($result.experiment){$result.experiment}else{'none'}
 Note ('experiment '+$(if($result.experiment){$result.experiment}else{'none'}))
 # 143+: engine A5734525 writes its pipeline creation log (hits/misses per path, save lines) only when asked;
 # cache-pull.py fetches it as <archive>.pso-log.txt for scratch\m15\etw\135\tmp\pso_log.py. 150: the slow-start
 # A/B needs a run without the log (the log's fopen in CreateDevice is the one engine suspect left), armed by a
 # marker file on the lab, recorded in the result, and removed again by the operator after that run.
 $result.pso_log=!(Test-Path -LiteralPath 'C:\BC250\tools\pso-log.off')
 if($result.pso_log){$env:AMDGPU_WDDM_VKD3D_PSO_LOG='1'}else{Remove-Item Env:AMDGPU_WDDM_VKD3D_PSO_LOG -ErrorAction SilentlyContinue}
 Note ('pso_log '+$result.pso_log)
 # 251+: one RADV tuning knob per session (KNOWLEDGE frontier 1b: RT wave size), armed by a marker file on the lab
 # holding the RADV_PERFTEST value, recorded in the result, removed again by the operator after that session.
 $perftestFile='C:\BC250\tools\radv-perftest.txt'
 $result.radv_perftest=if(Test-Path -LiteralPath $perftestFile){[string](Get-Content -LiteralPath $perftestFile -Raw).Trim()}else{''}
 if($result.radv_perftest){$env:RADV_PERFTEST=$result.radv_perftest}else{Remove-Item Env:RADV_PERFTEST -ErrorAction SilentlyContinue}
 Note ('radv_perftest '+$(if($result.radv_perftest){$result.radv_perftest}else{'none'}))
 # 111 ended in a ray tracing pipeline whose SPIR-V the ICD refused (vtn_structured_cfg.c:744, then the debug
 # build's break). Mesa writes a refused module here, and only a refused one, for offline analysis.
 $spirvFail=Join-Path $out 'spirv-fail'
 $null=New-Item -ItemType Directory -Force -Path $spirvFail
 $env:MESA_SPIRV_FAIL_DUMP_PATH=$spirvFail
 # Every shader the engine receives (DXIL, RT libraries as <hash>.lib.dxil), armed by a marker file on the lab
 # and written outside the attempt so the archive stays small; 113 needs the DXIL of 2d05bfd12deaa283.
 if(Test-Path -LiteralPath 'C:\BC250\tools\shader-dump.on'){
  $shaderDump=Join-Path 'C:\BC250\tmp' ('shader-dump-'+(Split-Path $d -Leaf))
  $null=New-Item -ItemType Directory -Force -Path $shaderDump
  $env:VKD3D_SHADER_DUMP_PATH=$shaderDump
  Note ('shader dump '+$shaderDump)
 }else{Remove-Item Env:VKD3D_SHADER_DUMP_PATH -ErrorAction SilentlyContinue}
 $result.steam_processes=@(Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($steamClient)) -ErrorAction SilentlyContinue).Count
 # Since 254 (M15.7): config.launch 'steam' has the running Steam client start the game; 'direct' is the executable.
 $result.launch=if($stageConfig.launch -eq 'steam'){'steam'}else{'direct'}
 $psi=New-Object Diagnostics.ProcessStartInfo
 $psi.FileName=$exe;$psi.WorkingDirectory=$game;$psi.UseShellExecute=$false
 # The profile's direct arguments plus the API's (e.g. -dx11); Witcher 3 has none.
 if($directArguments){$psi.Arguments=$directArguments}
 # Optional user-mode debugger, armed on the lab by a marker file: stack and minidump at the first
 # breakpoint or second-chance access violation (069 ended in 0x80000003 with no record of where).
 $cdb='C:\BC250\tools\cdb\cdb.exe'
 $result.debugger=[bool]($result.launch -eq 'direct' -and (Test-Path -LiteralPath $cdb) -and (Test-Path -LiteralPath 'C:\BC250\tools\cdb\use-for-game.on'))
 if($result.debugger){
  $init=Join-Path $out 'cdb-init.txt'
  $ascii=New-Object Text.ASCIIEncoding
  # Backslashes doubled: the debugger reads escapes inside a quoted command (070 lost its command file to them).
  $onBreak='.lastevent;r;kn 80;.echo ALLTHREADS;~*kn 30;.echo MODULES;lm;.dump /m '+(Join-Path $out 'game-break.dmp').Replace('\','\\')+';'+
   # Objects of the caller of the faulting function (frame 1), a few hundred bytes (request of review 793).
   # The frame's registers are copied once: later commands of the chain fall back to frame 0 (081).
   '.echo PROBE;.catch {.frame /c 1;r $t1=@r14;r $t2=@r15;r;dq @$t1 L30};.for (r $t0=0; @$t0<0x30; r $t0=@$t0+1) {.echo SLOT;? @$t0;.catch {dq poi(@$t1+@$t0*8) L6}};.catch {dq @$t2 L4};.echo PROBE-END;q'
  # The runtime announces a device removal on the thread that decides it: keep that stack, then go on.
  $onRemoval='.echo REMOVAL;r;kn 80;.dump /m '+(Join-Path $out 'game-removal.dmp').Replace('\','\\')+';g'
  # "D3D12: R" as bytes and as UTF-16 (the output filter of the debugger did not fire in 072).
  # By address: the lab's debugger does not load the exports of KERNELBASE (073, 074), and system
  # libraries share their base between processes of one boot.
  Add-Type -Namespace Bc -Name Native -MemberDefinition '[DllImport("kernel32",CharSet=CharSet.Ansi)]public static extern IntPtr GetModuleHandleA(string n);[DllImport("kernel32",CharSet=CharSet.Ansi)]public static extern IntPtr GetProcAddress(IntPtr m,string n);'
  $kb=[Bc.Native]::GetModuleHandleA('kernelbase.dll')
  $odsA='0x{0:x}' -f [Bc.Native]::GetProcAddress($kb,'OutputDebugStringA').ToInt64()
  $odsW='0x{0:x}' -f [Bc.Native]::GetProcAddress($kb,'OutputDebugStringW').ToInt64()
  if($odsA -eq '0x0' -or $odsW -eq '0x0'){throw 'Debug output functions not found'}
  $bpA='bp '+$odsA+' ".if (poi(@rcx)==0x52203a3231443344) {'+$onRemoval+'} .else {g}"'
  $bpW='bp '+$odsW+' ".if (poi(@rcx)==0x0031004400330044 & poi(@rcx+8)==0x00520020003a0032) {'+$onRemoval+'} .else {g}"'
  # Render target life of the game's executable C272B2C2 (offsets from its disassembly, Codex 798/799; the
  # init/cleanup points of 084 were never hit): bulk initializer entry, its resource helper result, the
  # handle store, the pool heap creation (HRESULT unchecked by the game) and the two allocation-info returns.
  $rt=''
  if($result.exe_sha256 -eq 'C272B2C2E61F84C758E28FAB69AB2915944DD1E539DBB435FAE9FC67494C7E25'){
   # 086-088 also broke at 0x1BE9000/0x1BE9217 (initializer entry, helper result), the second allocation-info
   # return 0x1EC85FE, and in our DLL (088: deferred breakpoints there stalled the game; not again).
   $rt='bp witcher3+0x1BE964B ".echo RTSTORE;r r13,r14;g"'+"`r`n"+
       'bp witcher3+0x1EC8DA9 ".echo POOLHEAP;r eax,rdi;dq @rsp+20 L6;dq @rdi+0xF440 L1;g"'+"`r`n"+
       'bp witcher3+0x1EC84CC ".echo ALLOCINFO1;dq @rbp-30 L2;.echo DESC1;dq poi(@rsp+20) L7;g"'+"`r`n"
  }
  # Operator's extra commands for one diagnosis (342: return values of the D3D11 shell's surface path, by its
  # private PDB in umdsym). Absent file = no change.
  $extraInit='C:\BC250\tools\cdb\extra-init.txt'
  if(Test-Path -LiteralPath $extraInit){$rt+=[IO.File]::ReadAllText($extraInit).TrimEnd()+"`r`n"}
  [IO.File]::WriteAllText($init,($rt+$bpA+"`r`n"+$bpW+"`r`n"+'sxe -c "'+$onBreak+'" bpe'+"`r`n"+'sxd -c2 "'+$onBreak+'" av'+"`r`n"+'g'+"`r`n"),$ascii)
  # Failures-only driver notes reach the debugger's log.
  $env:AMDGPU_WDDM_DDI_TRACE='2'
  $env:_NO_DEBUG_HEAP='1'
  $null=New-Item -ItemType Directory -Force -Path 'C:\BC250\tools\cdb\nosym'
  $psi.FileName=$cdb;$psi.CreateNoWindow=$true
  $psi.Arguments='-G -hd -y C:\BC250\tools\cdb\nosym;C:\BC250\tools\cdb\umdsym -logo "'+(Join-Path $out 'cdb.log')+'" -cf "'+$init+'" "'+$exe+'"'+$(if($directArguments){' '+$directArguments}else{''})
 }
 $utcStart=[DateTime]::UtcNow
 if($result.launch -eq 'steam'){
  # The game inherits Steam's environment, not this script's: a D3D12 shell finds no experiment variable and reads
  # the application profile recorded here (the profile's app_profile.image). Witcher 3: Steam's only launch entry of
  # app 292030 runs redprelauncher.exe, which starts bin\x64_dx12\witcher3.exe (the one executable of
  # launcher-configuration.json).
  $result.app_profile=try{[string](Get-ItemProperty -LiteralPath ('HKLM:\SOFTWARE\amdgpu-wddm\D3D12\Applications\'+$appImage) -Name Experiment -ErrorAction Stop).Experiment}catch{''}
  Note ('app_profile '+$(if($result.app_profile){$result.app_profile}else{'none'}))
  if(!$result.steam_processes){throw 'Steam client not running'}
  # 254: with an update pending, -applaunch starts the update (45 GB staged there) and launches the game only after
  # it, outside the trial. Launch only an app Steam reports as fully installed and current (StateFlags 4).
  $manifest=Read-SteamManifest
  $result.steam_state_flags=$manifest[0]
  $result.steam_build_id=$manifest[1]
  Note ('steam state flags '+$result.steam_state_flags+' build '+$result.steam_build_id)
  if($result.steam_state_flags -ne [int]$gp.steam.require_state_flags){throw ('Steam reports app '+$gp.app_id+' not ready (StateFlags '+$result.steam_state_flags+'): no launch')}
  # The profile's Steam arguments and the API's: Steam appends the arguments after the app id to its launch entry,
  # as the game's launch options in Steam would. Witcher 3: --launcher-skip (a REDprelauncher option, "Skipping
  # REDlauncher due to commandline options"); without it the prelauncher of the 2026-10-01 update installed
  # REDlauncher 5.5.0.5 and never started the game (254).
  $result.steam_arguments=('-applaunch '+$gp.app_id+' '+$launchArguments).Trim()
  $steamStart=[Diagnostics.Process]::Start($steamClient,$result.steam_arguments)
  Note ('steam '+$result.steam_arguments+' pid '+$steamStart.Id)
  $child=$null;$seenLaunch=@{};$steamWait=[int]$gp.steam.wait_seconds
  for($i=1;$i -le $steamWait -and !$child;$i++){
   Start-Sleep -Seconds 1
   foreach($p in @(Get-Process -Name @($gp.processes.launch_chain) -ErrorAction SilentlyContinue)){
    if(!$seenLaunch.ContainsKey($p.Id)){$seenLaunch[$p.Id]=$true;$result.launch_chain+=,($p.ProcessName+' '+$p.Id+' at '+$i+'s');Note ('launch '+$i+'s: '+$p.ProcessName+' '+$p.Id)}
   }
   $child=Get-Process -Name ([string]$gp.processes.main) -ErrorAction SilentlyContinue|Select-Object -First 1
   if(!$child -and $i -in 15,40,70,$steamWait){try{Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:2250/screenshot?scale=0.5&format=png&overlay=0' -OutFile (Join-Path $out ('launch-'+$i+'.png')) -TimeoutSec 5;$result.screenshots++;Note ('launch screenshot at '+$i+'s')}catch{}}
   if(Stop-Requested){$result.stop_reason='owner-stop';break}
  }
  if(!$child){throw ('Game did not start through Steam within '+$steamWait+' s')}
  $null=$child.Handle
  $result.launch_seconds=[int]([DateTime]::UtcNow-$utcStart).TotalSeconds
  # The task limit counts from the trial start: the launch wait comes out of the game's bound.
  $bound-=$result.launch_seconds
  Note ('launch through steam '+$result.launch_seconds+' s, bound now '+$bound+' s')
 }else{
 $child=[Diagnostics.Process]::Start($psi)
 $null=$child.Handle
 }
 if($result.debugger){
  $debugger=$child;$child=$null
  for($i=0;$i -lt 40 -and !$child;$i++){Start-Sleep -Milliseconds 250;$child=Get-Process -Name ([string]$gp.processes.main) -ErrorAction SilentlyContinue|Select-Object -First 1}
  if(!$child){throw 'Game did not start under the debugger'}
  $null=$child.Handle
 }
 $result.launched=$true;$result.pid=$child.Id;$result.session=$child.SessionId
 Note ('pid '+$child.Id+' session '+$child.SessionId)
 $timer=[Diagnostics.Stopwatch]::StartNew()
 $shots=@(25,70,120,160,190,205);$taken=0;$tempUnreadable=0;$hotSince=$null
 while(!$child.HasExited -and $timer.Elapsed.TotalSeconds -lt $bound){
  $t=[int]$timer.Elapsed.TotalSeconds
  # The Witcher 3 route reads its one process; a generic profile reads every process of the game (main and
  # renderers) and keeps per process which watched modules it loaded; the first sighting of a module is recorded
  # with its process.
  $procs=if($witcherMenu){@($child)}else{Get-GameProcesses}
  foreach($gproc in $procs){
   try{
    foreach($m in $gproc.Modules){
     $name=$m.ModuleName.ToLowerInvariant()
     if($name -notin $watch){continue}
     if(!$witcherMenu){$key=[string]$gproc.Id;if(!$gameModules.ContainsKey($key)){$gameModules[$key]=@{}};$gameModules[$key][$name]=$true}
     if(!$result.modules.ContainsKey($name)){
      $result.modules[$name]=@{path=$m.FileName;sha256=(Get-FileHash -LiteralPath $m.FileName).Hash;seen_at=$t}
      if(!$witcherMenu){$result.modules[$name].process=$gproc.ProcessName;$result.modules[$name].pid=$gproc.Id}
      Note ('module '+$t+'s '+$name+' '+$result.modules[$name].sha256+' '+$m.FileName+$(if(!$witcherMenu){' in '+$gproc.ProcessName+' '+$gproc.Id}else{''}))
     }
    }
   }catch{}
  }
  $child.Refresh()
  if($child.HasExited){break}
  $temp=Temp-Now
  try{
   if($child.MainWindowTitle -and $child.MainWindowTitle -ne $result.last_title){$result.last_title=$child.MainWindowTitle;if(!$windowUtc){$windowUtc=[DateTime]::UtcNow};Note ('window '+$t+'s '+$result.last_title)}
   $result.responding_last=[bool]$child.Responding
   $ws=[int]($child.WorkingSet64/1MB);if($ws -gt $result.peak_ws_mb){$result.peak_ws_mb=$ws}
   $threads=$child.Threads.Count;if($threads -gt $result.peak_threads){$result.peak_threads=$threads}
   Note ('t='+$t+' tctl='+$temp+' ws_mb='+$ws+' threads='+$threads+' responding='+$result.responding_last)
  }catch{Note ('sample failed: '+$_.Exception.Message)}
  # The thermal guard outside the sample's try: -1 is unreadable, never cool; two in a row end the session.
  # Owner rule 2026-10-04: stop when Tctl stays >= 87 C for 10 s, or at once at >= 89 C (scene loads spike for a
  # few seconds). An unreadable sample (-1) keeps the hot run: only a readable sample below 87 C ends it.
  $nowUtc=[DateTime]::UtcNow
  if($temp -ge 87){if(!$hotSince){$hotSince=$nowUtc}}elseif($temp -ge 0){$hotSince=$null}
  $hotSeconds=if($hotSince){($nowUtc-$hotSince).TotalSeconds}else{0}
  if($temp -ge 89 -or $hotSeconds -ge 10){$result.stop_reason='thermal';Note ('t='+$t+' thermal stop at '+$temp+' C, hot for '+[math]::Round($hotSeconds,1)+' s');break}
  if($temp -lt 0){$tempUnreadable++}else{$tempUnreadable=0}
  if($tempUnreadable -ge 2){$result.stop_reason='thermal-unreadable';Note ('t='+$t+' temperature unreadable twice, stop');break}
  if(Stop-Requested){$result.stop_reason='owner-stop';break}
  if($taken -lt $shots.Count -and $t -ge $shots[$taken]){
   try{
    Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:2250/screenshot?scale=0.5&format=png&overlay=0' -OutFile (Join-Path $out ('screen-'+$taken+'.png')) -TimeoutSec 5
    $result.screenshots++;Note ('screenshot '+$taken+' at '+$t+'s')
   }catch{Note ('screenshot '+$taken+' failed: '+$_.Exception.Message)}
   $taken++
  }
  if($witcherMenu){
   # A failed capture resets readiness like an untied one (844).
   if($result.last_title){try{Menu-Step $t}catch{$menu.failed_captures++;$menu.tied_run=0;$menu.errors+=,$_.Exception.Message;Note ('menu '+$t+'s failed: '+$_.Exception.Message)}}
  }else{try{Generic-Step $t $procs}catch{$generic.errors+=,$_.Exception.Message;Note ('generic '+$t+'s failed: '+$_.Exception.Message)}}
  if($control.enabled){
   try{Control-Step $t}catch{$control.errors+=,$_.Exception.Message;Note ('control '+$t+'s failed: '+$_.Exception.Message)}
   if($control.quit){$result.stop_reason='operator-quit';break}
  }elseif($menu.changed){try{Walk-Step $t}catch{$walk.errors+=,$_.Exception.Message;Note ('walk '+$t+'s failed: '+$_.Exception.Message)}}
  Start-Sleep -Seconds $(if($control.enabled -and (Control-Open)){1}else{3})
 }
 $result.elapsed_seconds=[int]$timer.Elapsed.TotalSeconds
 $child.Refresh()
 $result.has_exited=[bool]$child.HasExited
 if($child.HasExited){$result.exit_code=$child.ExitCode}
 else{$result.bounded_stop=$true;if(!$result.stop_reason){$result.stop_reason='bound'}}
}catch{
 $result.errors+=,$_.Exception.Message
}finally{
 try{
  if($child -and !$child.HasExited){Note ('ending pid '+$child.Id);Stop-Process -Id $child.Id -Force;$null=$child.WaitForExit(10000)}
  if($debugger){$null=$debugger.WaitForExit(8000)}
  Get-Process -Name @($gp.processes.end_at_cleanup) -ErrorAction SilentlyContinue|ForEach-Object{Note ('stray '+$_.ProcessName+' '+$_.Id);Stop-Process -Id $_.Id -Force}
  Start-Sleep -Seconds 2
  # Settings are restored only once the game and its debugger are seen gone: a live writer could overwrite (844).
  if($settingsPolicy -eq 'backup-restore'){Restore-Settings (Writers-Gone @($gp.processes.writers))}
  elseif($settingsPolicy -eq 'record' -and $result.settings_record){Record-Settings 'after'}
  if($result.launched){
   try{
    $events=@(Get-WinEvent -FilterHashtable @{LogName='Application';StartTime=$utcStart.ToLocalTime().AddSeconds(-2);Id=1000,1001,1002} -MaxEvents 20 -ErrorAction SilentlyContinue|Where-Object{$_.Message -match [string]$gp.events_filter})
    $text=($events|ForEach-Object{('{0:o} id {1}'+"`n"+'{2}'+"`n") -f $_.TimeCreated.ToUniversalTime(),$_.Id,$_.Message}) -join "`n"
    if($text.Length -gt 65536){$text=$text.Substring(0,65536)}
    [IO.File]::WriteAllText((Join-Path $out 'application-events.txt'),$text,(New-Object Text.UTF8Encoding($false)))
    $result.application_events=$events.Count
   }catch{$result.errors+=,('events: '+$_.Exception.Message)}
  }
  Note ('temperature_after '+(Temp-Now))
  if($result.saves.before){
   $result.saves.after=Save-State
   if($utcStart -and (Test-Path -LiteralPath $saves)){$result.saves.new=@(Get-ChildItem -LiteralPath $saves -File|Where-Object{$_.LastWriteTimeUtc -gt $utcStart}|ForEach-Object{$_.Name})}
  }
 }catch{$result.errors+=,('cleanup: '+$_.Exception.Message)}
 $result.menu_pass=$menu
 $result.walk=$walk
 $result.control=$control
 $result.game_profile=[ordered]@{id=[string]$gp.id;app_id=$gp.app_id;api=$gameApi;readiness=[string]$gp.readiness.mode;settings=$settingsPolicy}
 if(!$witcherMenu){$result.generic=$generic}
 try{Note 'wrapper end'}catch{}
 $noteFile.Dispose()
 Write-DurableText (Join-Path $out 'game-result.json') ($result|ConvertTo-Json -Depth 5)
}
if(Get-Process -Name $gameNames -ErrorAction SilentlyContinue){exit 3}
if(!$result.launched){exit 2}
exit 0
