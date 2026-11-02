# The in-process KMD reader (BD-051): the clock reading, the start-health line and the driver log ring of the installed
# driver, read through the release bc250control.dll inside the calling PowerShell process. No process is started for
# a reading.
#
# Why. A polling loop that starts bc250kmd_cli once a second, inside an ssh session or beside it, is one half of the
# BD-051 sshd accept stall (trials 152, 154, 220, sessions 424 and 427). The reader sends the same escapes as
# "bc250kmd_cli clock read", "bc250kmd_cli health read" and "bc250kmd_cli log <from>" through the same exported code of
# the release DLL (Bc250ClockControl, Bc250StartHealth, Bc250LogRead), and gives the same text. Only the process start
# goes away.
#
# Use (Windows PowerShell 5.1 or PowerShell 7, x64):
#   . <dir>\kmd-reader.ps1
#   Initialize-KmdReader                     # once; Add-Type compiles the class once per process (one csc.exe)
#   $c = Read-KmdClock                       # .Ok .Line .Reason .MHz .Vid .TemperatureMc .Ready
#   $h = Read-KmdHealth                      # the line of "bc250kmd_cli health read", or $null
#   $t = Read-KmdLog -From 123               # text as "bc250kmd_cli log 123" prints it, or $null (Get-KmdReaderLoss)
#
# Every reading is bounded (-TimeoutMs, default 2000): one resident worker thread sends the escape and the caller waits
# for it at most that long. A reading that does not come back in time is a loss ("no answer in 2000 ms"). While it
# stays out, the next reading is a loss at once ("the reading of N ms ago has not come back") and sends nothing, so
# one hung escape never piles up threads or escapes. When it comes back, the next reading goes out again.
#
# The DLL: -Dll, else $env:BC250_KMD_READER_DLL, else the installed release (HKLM\SOFTWARE\amdgpu-wddm\Release
# InstallRoot) tools\bc250control.dll beside bc250kmd_cli.exe, else its control\bc250control.dll. The reader holds the
# DLL loaded until its process ends, so a long sampler holds the file for that time (as the overlay does).

# Safe under Set-StrictMode (the HIP arm library sets 2.0), and a second dot-source keeps a loaded reader.
if (-not (Get-Variable -Name KmdReader -Scope Script -ErrorAction SilentlyContinue)) { $script:KmdReader = $null }

function Initialize-KmdReaderType {
    if ('Bc250Tools.KmdReader' -as [type]) { return }
    Add-Type -ErrorAction Stop -TypeDefinition @'
using System;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

namespace Bc250Tools {
 public sealed class KmdClock {
  public bool Ok; public string Line; public string Reason;
  public uint Abi; public uint MHz; public uint Vid; public int TemperatureMc; public uint Ready;
 }

 public sealed class KmdReader {
  [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
  static extern IntPtr LoadLibraryExW(string path, IntPtr file, uint flags);
  [DllImport("kernel32.dll", CharSet = CharSet.Ansi, ExactSpelling = true, SetLastError = true)]
  static extern IntPtr GetProcAddress(IntPtr module, string name);

  [UnmanagedFunctionPointer(CallingConvention.Winapi)]
  delegate int ClockControlFn(uint op, uint mhz, uint mv, IntPtr data, uint bytes);
  [UnmanagedFunctionPointer(CallingConvention.Winapi)]
  delegate int LogReadFn(uint from, IntPtr data, uint bytes);
  [UnmanagedFunctionPointer(CallingConvention.Winapi)]
  delegate int StartHealthFn(uint op, ulong generation, ulong epoch, IntPtr data, uint bytes);

  // driver/kmd/bc250kmd_escape.h. The exports refuse any other size (STATUS_INVALID_PARAMETER), so a layout that
  // moves shows as a refusal, never as a misread. The host test compares every text with the CLI's on one fake driver.
  const int ClockBytes = 80;                // BC250_ESCAPE_CLOCK: 20 ULONGs
  const int ClockAbi = 20, ClockObservedMHz = 36, ClockObservedVid = 40, ClockTemperatureMc = 44, ClockReady = 64;
  const uint ClockOpRead = 0;               // BC250_CLOCK_OP_READ
  const int LogLineBytes = 168;             // BC250_LOG_LINE: Sequence, Milliseconds, char Text[160]
  const int LogTextBytes = 160;             // BC250_LOG_TEXT
  const int LogMaxLines = 64;               // BC250_LOG_MAX_LINES
  const int LogBytes = 60 + LogMaxLines * LogLineBytes;    // BC250_ESCAPE_LOG
  const int LogFlags = 20, LogTotal = 28, LogLost = 32, LogAbove = 36, LogReturned = 40, LogNext = 44;
  const int LogHeadLines = 48, LogRingLines = 52, LogSummaryFrom = 56, LogLines = 60;
  const uint FlagFullWddm = 256;            // BC250_ESCAPE_FLAG_FULL_WDDM
  const uint SummarySeq = 0xF0000000u;      // BC250_LOG_SUMMARY_SEQ
  const uint SummaryLines = 512;            // BC250_LOG_SUMMARY_LINES
  const uint FromSummary = 0xFFFFFFFFu;     // BC250_LOG_FROM_SUMMARY, which Bc250LogRead refuses
  const int HealthBytes = 96;               // BC250_ESCAPE_START_HEALTH, ABI 1
  const int HealthVersion = 12, HealthAbi = 20, HealthFlags = 28, HealthGeneration = 32, HealthEpoch = 40;
  const int HealthCompleted = 48, HealthAgeMs = 56, HealthReadyMs = 64;
  const uint HealthRead = 0;                // BC250_START_HEALTH_READ

  readonly ClockControlFn clock;
  readonly LogReadFn logRead;
  readonly StartHealthFn health;
  readonly IntPtr clockBuffer, logBuffer, healthBuffer;   // touched by the worker thread only
  public readonly string DllPath;

  readonly object gate = new object();
  readonly AutoResetEvent wake = new AutoResetEvent(false);
  Func<object> job;                         // the reading the worker runs next, or runs now
  object jobResult;
  Exception jobError;
  bool jobOut;                              // a reading was sent and has not come back
  long jobStartTicks;
  // Set by the worker under the gate, together with jobOut = false, so a reading that starts after a late answer
  // never sees the late answer's signal: it can start only when that signal is already given.
  readonly ManualResetEvent jobDone = new ManualResetEvent(true);
  public long Readings, Losses, Escapes;
  public string LastLoss = "";

  public KmdReader(string dllPath) {
   if (String.IsNullOrEmpty(dllPath)) throw new ArgumentException("no bc250control.dll path");
   IntPtr module = LoadLibraryExW(dllPath, IntPtr.Zero, 0x00000008);    // LOAD_WITH_ALTERED_SEARCH_PATH
   if (module == IntPtr.Zero) throw new InvalidOperationException("LoadLibraryEx " + dllPath + " failed, error " + Marshal.GetLastWin32Error().ToString(CultureInfo.InvariantCulture));
   clock = (ClockControlFn)Bind(module, dllPath, "Bc250ClockControl", typeof(ClockControlFn));
   logRead = (LogReadFn)Bind(module, dllPath, "Bc250LogRead", typeof(LogReadFn));
   health = (StartHealthFn)Bind(module, dllPath, "Bc250StartHealth", typeof(StartHealthFn));
   DllPath = dllPath;
   clockBuffer = Marshal.AllocHGlobal(ClockBytes);
   logBuffer = Marshal.AllocHGlobal(LogBytes);
   healthBuffer = Marshal.AllocHGlobal(HealthBytes);
   Thread worker = new Thread(Work);
   worker.IsBackground = true;               // a hung escape never keeps the process alive
   worker.Name = "kmd-reader";
   worker.Start();
  }

  static Delegate Bind(IntPtr module, string path, string name, Type type) {
   IntPtr entry = GetProcAddress(module, name);
   if (entry == IntPtr.Zero) throw new InvalidOperationException(path + " has no export " + name + " (a release before the control DLL had it)");
   return Marshal.GetDelegateForFunctionPointer(entry, type);
  }

  void Work() {
   for (;;) {
    wake.WaitOne();
    Func<object> run;
    lock (gate) { run = job; }
    if (run == null) continue;
    object result = null; Exception error = null;
    try { result = run(); } catch (Exception e) { error = e; }
    lock (gate) { jobResult = result; jobError = error; job = null; jobOut = false; jobDone.Set(); }
   }
  }

  // One bounded reading. null with LastLoss set when it failed, did not come back in time, or one is still out.
  object Call(Func<object> reading, int timeoutMs, out string loss) {
   lock (gate) {
    Readings++;
    if (jobOut) {
     long ms = (DateTime.UtcNow.Ticks - jobStartTicks) / TimeSpan.TicksPerMillisecond;
     loss = "the reading of " + ms.ToString(CultureInfo.InvariantCulture) + " ms ago has not come back; nothing sent";
     Losses++; LastLoss = loss;
     return null;
    }
    jobOut = true; job = reading; jobResult = null; jobError = null;
    jobStartTicks = DateTime.UtcNow.Ticks;
    jobDone.Reset();
   }
   wake.Set();
   if (!jobDone.WaitOne(timeoutMs < 1 ? 1 : timeoutMs)) {
    loss = "no answer in " + timeoutMs.ToString(CultureInfo.InvariantCulture) + " ms";
    lock (gate) { Losses++; LastLoss = loss; }
    return null;
   }
   lock (gate) {
    if (jobError != null) {
     // A refusal of the driver is an InvalidOperationException whose message is the CLI's own line.
     loss = jobError is InvalidOperationException ? jobError.Message : jobError.GetType().Name + ": " + jobError.Message;
     Losses++; LastLoss = loss; return null;
    }
    loss = ""; return jobResult;
   }
  }

  static string Hex(int status) { return "0x" + unchecked((uint)status).ToString("X8", CultureInfo.InvariantCulture); }
  static uint U32(IntPtr b, int offset) { return unchecked((uint)Marshal.ReadInt32(b, offset)); }

  // "bc250kmd_cli clock read": Bc250ClockControl READ, which sends BC250_ESCAPE_RUN_CLOCK with
  // NoAdapterSynchronization alone, and the CLI's line (bc250kmd_cli.c Clock).
  public KmdClock ReadClock(int timeoutMs) {
   string loss;
   object r = Call(delegate {
    int status = clock(ClockOpRead, 0, 0, clockBuffer, ClockBytes);
    Interlocked.Increment(ref Escapes);
    KmdClock c = new KmdClock();
    if (status < 0) { c.Ok = false; c.Reason = "KMD clock -> " + Hex(status); return c; }
    c.Abi = U32(clockBuffer, ClockAbi); c.MHz = U32(clockBuffer, ClockObservedMHz); c.Vid = U32(clockBuffer, ClockObservedVid);
    c.TemperatureMc = Marshal.ReadInt32(clockBuffer, ClockTemperatureMc); c.Ready = U32(clockBuffer, ClockReady);
    c.Line = String.Format(CultureInfo.InvariantCulture, "clock backend=kmd-smu abi={0} MHz={1} VID={2} temperature_mc={3} ready={4}",
                           c.Abi, c.MHz, c.Vid, c.TemperatureMc, c.Ready);
    c.Ok = true; c.Reason = "";
    return c;
   }, timeoutMs, out loss);
   KmdClock k = r as KmdClock;
   if (k == null) { k = new KmdClock(); k.Ok = false; k.Reason = loss; return k; }
   if (!k.Ok) lock (gate) { Losses++; LastLoss = k.Reason; }
   return k;
  }

  // "bc250kmd_cli health read": Bc250StartHealth READ (BC250_ESCAPE_RUN_START_HEALTH, NoAdapterSynchronization alone)
  // and the CLI's line (bc250kmd_cli.c StartHealth). null when it failed (LastLoss says why, as the CLI's line).
  public string ReadHealth(int timeoutMs) {
   string loss;
   object r = Call(delegate {
    int status = health(HealthRead, 0, 0, healthBuffer, HealthBytes);
    Interlocked.Increment(ref Escapes);
    if (status < 0) throw new InvalidOperationException("KMD start health -> " + Hex(status));
    return (object)String.Format(CultureInfo.InvariantCulture,
     "health abi={0} version=0x{1:X8} flags={2} generation={3} epoch={4} completed={5} age_ms={6} ready_ms={7}",
     U32(healthBuffer, HealthAbi), U32(healthBuffer, HealthVersion), U32(healthBuffer, HealthFlags),
     U64(healthBuffer, HealthGeneration), U64(healthBuffer, HealthEpoch), U64(healthBuffer, HealthCompleted),
     U64(healthBuffer, HealthAgeMs), U64(healthBuffer, HealthReadyMs));
   }, timeoutMs, out loss);
   return r as string;
  }

  static ulong U64(IntPtr b, int offset) { return unchecked((ulong)Marshal.ReadInt64(b, offset)); }

  // "bc250kmd_cli log <from>": pages of Bc250LogRead (BC250_ESCAPE_GET_LOG, NoAdapterSynchronization alone) and the
  // CLI's text for them (bc250kmd_cli.c Log, the ring form), line ends included. null when the first page failed;
  // a later page that fails ends the text with the CLI's refusal line, and LastLoss names it.
  public string ReadLog(uint from, int timeoutMs) {
   if (from == FromSummary) { LastLoss = "log: the summary sentinel is not a position"; return null; }
   string loss;
   object r = Call(delegate { return (object)LogText(from); }, timeoutMs, out loss);
   if (r == null) return null;
   string[] pair = (string[])r;
   if (pair[1].Length > 0) lock (gate) { Losses++; LastLoss = pair[1]; }
   return pair[0];
  }

  static string Refusal(int status) {
   uint s = unchecked((uint)status);
   if (s == 0xC0000022u) return "refused: caller is not an administrator";
   if (s == 0xC00000BBu) return "refused: this driver build has no log command (0.7.0 or older)";
   return "D3DKMTEscape -> " + Hex(status);
  }

  string[] LogText(uint from) {
   StringBuilder text = new StringBuilder();
   uint printed = 0, block = 0, soft = 0;
   bool first = true;
   for (;;) {
    int status = logRead(from, logBuffer, LogBytes);
    Interlocked.Increment(ref Escapes);
    if (status < 0) {
     string why = Refusal(status);
     if (first) throw new InvalidOperationException(why);
     text.Append(why).Append("\r\n");
     return new string[] { text.ToString(), why };
    }
    soft++;
    uint returned = U32(logBuffer, LogReturned);
    if (first)
     text.AppendFormat(CultureInfo.InvariantCulture,
      "log          {0} lines since this driver load, {1} lost to the wrap, {2} dropped above DISPATCH_LEVEL; ring {3} lines of which the first {4} are kept; table {5}\r\n",
      U32(logBuffer, LogTotal), U32(logBuffer, LogLost), U32(logBuffer, LogAbove), U32(logBuffer, LogRingLines),
      U32(logBuffer, LogHeadLines),
      (U32(logBuffer, LogFlags) & FlagFullWddm) != 0 ? "FULL WDDM (the gate was open at DriverEntry)" : "display-only");
    first = false;
    for (uint i = 0; i < returned && i < LogMaxLines; i++) {
     int line = LogLines + (int)i * LogLineBytes;
     uint ms = U32(logBuffer, line + 4);
     byte[] raw = new byte[LogTextBytes];
     Marshal.Copy(new IntPtr(logBuffer.ToInt64() + line + 8), raw, 0, LogTextBytes);
     int n = Array.IndexOf(raw, (byte)0);
     if (n < 0 || n > LogTextBytes - 1) n = LogTextBytes - 1;   // the CLI terminates at Text[159]
     text.AppendFormat(CultureInfo.InvariantCulture, "{0,6} {1,6}.{2:000} {3}\r\n", U32(logBuffer, line), ms / 1000, ms % 1000,
                       Encoding.GetEncoding(28591).GetString(raw, 0, n));
     printed++;
    }
    if (from >= SummarySeq) block = SummarySeq + (from - SummarySeq) / SummaryLines * SummaryLines;
    uint summaryFrom = U32(logBuffer, LogSummaryFrom);
    if (returned == 0 && block != 0 && summaryFrom >= SummarySeq && summaryFrom != block)
     text.AppendFormat(CultureInfo.InvariantCulture,
      "             the block beside the ring was replaced by a newer summary after {0} lines; the one held now reads from {1}\r\n",
      printed, summaryFrom);
    uint next = U32(logBuffer, LogNext);
    if (returned == 0 || next <= from) break;
    from = next;
    if (printed > 4 * U32(logBuffer, LogRingLines)) {
     text.AppendFormat(CultureInfo.InvariantCulture, "             stopped at {0} lines; ask again from {1}\r\n", printed, from);
     break;
    }
   }
   text.AppendFormat(CultureInfo.InvariantCulture, "             {0} lines printed\r\n", printed);
   text.AppendFormat(CultureInfo.InvariantCulture, "             escapes: {0} without adapter synchronization, 0 with HardwareAccess\r\n", soft);
   return new string[] { text.ToString(), "" };
  }
 }
}
'@
}

# The DLL that a reader of this process uses: the argument, the test override, else the installed release's copy.
function Resolve-KmdReaderDll([string]$Dll = '') {
    if ($Dll) { return $Dll }
    if ($env:BC250_KMD_READER_DLL) { return $env:BC250_KMD_READER_DLL }
    $root = ''
    try { $root = [string](Get-ItemProperty 'HKLM:\SOFTWARE\amdgpu-wddm\Release' -Name InstallRoot -ErrorAction Stop).InstallRoot } catch { }
    if (-not $root) { throw 'no installed release (HKLM\SOFTWARE\amdgpu-wddm\Release InstallRoot): no bc250control.dll to read with' }
    foreach ($candidate in (Join-Path $root 'tools\bc250control.dll'), (Join-Path $root 'control\bc250control.dll')) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
    }
    throw "the installed release $root has no tools\bc250control.dll and no control\bc250control.dll"
}

# Loads the reader once per process and returns it. Throws when the DLL is missing or lacks an export.
function Initialize-KmdReader([string]$Dll = '') {
    if ($script:KmdReader) { return $script:KmdReader }
    $path = Resolve-KmdReaderDll $Dll
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "no bc250control.dll at $path" }
    Initialize-KmdReaderType
    $script:KmdReader = New-Object Bc250Tools.KmdReader ([IO.Path]::GetFullPath($path))
    return $script:KmdReader
}

# One clock reading: .Ok, .Line (the CLI's "clock read" line), .Reason when not Ok, and the fields.
function Read-KmdClock([int]$TimeoutMs = 2000) {
    if (-not $script:KmdReader) {
        return [pscustomobject]@{ Ok = $false; Line = ''; Reason = 'the reader is not loaded (Initialize-KmdReader)' }
    }
    return $script:KmdReader.ReadClock($TimeoutMs)
}

# The log ring from -From on, as "bc250kmd_cli log <From>" prints it, or $null (Get-KmdReaderLoss says why).
function Read-KmdLog([Parameter(Mandatory)][uint32]$From, [int]$TimeoutMs = 2000) {
    if (-not $script:KmdReader) { return $null }
    return $script:KmdReader.ReadLog($From, $TimeoutMs)
}

# The KMD start-health line, as "bc250kmd_cli health read" prints it, or $null (Get-KmdReaderLoss says why).
function Read-KmdHealth([int]$TimeoutMs = 2000) {
    if (-not $script:KmdReader) { return $null }
    return $script:KmdReader.ReadHealth($TimeoutMs)
}

function Get-KmdReaderLoss {
    if (-not $script:KmdReader) { return 'the reader is not loaded (Initialize-KmdReader)' }
    return $script:KmdReader.LastLoss
}
