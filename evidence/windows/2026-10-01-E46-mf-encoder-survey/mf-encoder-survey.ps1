# mf-encoder-survey.ps1 - E-A1 for M15.11: what Media Foundation offers on unit A today.
#
# READ-ONLY. It calls MFTEnumEx and reads registry values. It creates no object through
# IMFActivate::ActivateObject, writes no registry value, loads no driver, starts no capture.
# Run elevated through the usual tooling:
#   python bc250-win\tools\win\target.py ps scratch\m15\video-encode\mf-encoder-survey.ps1
# It prints a plain-text report on stdout; redirect it into the trial's evidence directory.
#
# Why: section 3.3 of scratch\m15\video-encode\BOOTSTRAP.md. The one thing we cannot read out
# of a header is the attribute set a Media Foundation client (Game Bar, Camera, Chromium)
# requires of a hardware encoder MFT. This enumerates what the system offers today, with every
# attribute of every candidate, so our own MFT can be built to match.

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

function Write-Head([string]$t) {
  Write-Output ''
  Write-Output ('=' * 78)
  Write-Output $t
  Write-Output ('=' * 78)
}

Write-Output ('mf-encoder-survey.ps1  UTC ' + (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ'))
Write-Output ('host ' + $env:COMPUTERNAME + '  user ' + $env:USERNAME + '  PS ' + $PSVersionTable.PSVersion.ToString())
Write-Output ('os   ' + (Get-CimInstance Win32_OperatingSystem).Caption + ' build ' + (Get-CimInstance Win32_OperatingSystem).BuildNumber)

$cs = @'
using System;
using System.Runtime.InteropServices;
using System.Text;

namespace Bc250Mf
{
    [ComImport, Guid("7FEE9E9A-4A89-47a6-899C-B6A53A70FB67"),
     InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IMFActivate
    {
        // IMFAttributes - every slot must be declared to keep the vtable order
        [PreserveSig] int GetItem(ref Guid key, IntPtr pValue);
        [PreserveSig] int GetItemType(ref Guid key, out int pType);
        [PreserveSig] int CompareItem(ref Guid key, IntPtr Value, out int pbResult);
        [PreserveSig] int Compare(IntPtr pTheirs, int MatchType, out int pbResult);
        [PreserveSig] int GetUINT32(ref Guid key, out uint punValue);
        [PreserveSig] int GetUINT64(ref Guid key, out ulong punValue);
        [PreserveSig] int GetDouble(ref Guid key, out double pfValue);
        [PreserveSig] int GetGUID(ref Guid key, out Guid pguidValue);
        [PreserveSig] int GetStringLength(ref Guid key, out uint pcchLength);
        [PreserveSig] int GetString(ref Guid key, [Out] StringBuilder pwszValue, uint cchBufSize, IntPtr pcchLength);
        [PreserveSig] int GetAllocatedString(ref Guid key, out IntPtr ppwszValue, out uint pcchLength);
        [PreserveSig] int GetBlobSize(ref Guid key, out uint pcbBlobSize);
        // LPArray: in a ComImport interface a byte[] defaults to a SAFEARRAY (AccessViolation, 2026-10-01)
        [PreserveSig] int GetBlob(ref Guid key, [Out, MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] byte[] pBuf, uint cbBufSize, IntPtr pcbBlobSize);
        [PreserveSig] int GetAllocatedBlob(ref Guid key, out IntPtr ppBuf, out uint pcbSize);
        [PreserveSig] int GetUnknown(ref Guid key, ref Guid riid, out IntPtr ppv);
        [PreserveSig] int SetItem(ref Guid key, IntPtr Value);
        [PreserveSig] int DeleteItem(ref Guid key);
        [PreserveSig] int DeleteAllItems();
        [PreserveSig] int SetUINT32(ref Guid key, uint unValue);
        [PreserveSig] int SetUINT64(ref Guid key, ulong unValue);
        [PreserveSig] int SetDouble(ref Guid key, double fValue);
        [PreserveSig] int SetGUID(ref Guid key, ref Guid guidValue);
        [PreserveSig] int SetString(ref Guid key, [MarshalAs(UnmanagedType.LPWStr)] string wszValue);
        [PreserveSig] int SetBlob(ref Guid key, [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] byte[] pBuf, uint cbBufSize);
        [PreserveSig] int SetUnknown(ref Guid key, IntPtr pUnknown);
        [PreserveSig] int LockStore();
        [PreserveSig] int UnlockStore();
        [PreserveSig] int GetCount(out uint pcItems);
        [PreserveSig] int GetItemByIndex(uint unIndex, out Guid pguidKey, IntPtr pValue);
        [PreserveSig] int CopyAllItems(IntPtr pDest);
        // IMFActivate - declared, never called by this script
        [PreserveSig] int ActivateObject(ref Guid riid, out IntPtr ppv);
        [PreserveSig] int ShutdownObject();
        [PreserveSig] int DetachObject();
    }

    public static class Mf
    {
        [DllImport("mfplat.dll", ExactSpelling = true)]
        public static extern int MFStartup(uint Version, uint dwFlags);
        [DllImport("mfplat.dll", ExactSpelling = true)]
        public static extern int MFShutdown();
        [DllImport("mfplat.dll", ExactSpelling = true)]
        public static extern int MFTEnumEx(Guid guidCategory, uint Flags,
            IntPtr pInputType, IntPtr pOutputType,
            out IntPtr ppMFTActivate, out uint pnumMFTActivate);
        [DllImport("ole32.dll")]
        public static extern void CoTaskMemFree(IntPtr pv);

        public const uint MF_VERSION = 0x00020070;   // mfapi.h:31-40 (SDK 10.0.26100)

        public static int Startup() { return MFStartup(MF_VERSION, 0); }

        // Returns the IMFActivate objects for one category/flag combination.
        public static object[] EnumMfts(Guid category, uint flags, out int hr)
        {
            IntPtr arr = IntPtr.Zero; uint n = 0;
            hr = MFTEnumEx(category, flags, IntPtr.Zero, IntPtr.Zero, out arr, out n);
            if (hr < 0 || arr == IntPtr.Zero || n == 0)
            {
                if (arr != IntPtr.Zero) CoTaskMemFree(arr);
                return new object[0];
            }
            object[] res = new object[n];
            for (int i = 0; i < (int)n; i++)
            {
                IntPtr p = Marshal.ReadIntPtr(arr, i * IntPtr.Size);
                res[i] = Marshal.GetObjectForIUnknown(p);
                Marshal.Release(p);
            }
            CoTaskMemFree(arr);
            return res;
        }

        public static string GetStr(object o, Guid key)
        {
            IMFActivate a = (IMFActivate)o;
            uint len = 0;
            if (a.GetStringLength(ref key, out len) < 0) return null;
            StringBuilder sb = new StringBuilder((int)len + 1);
            if (a.GetString(ref key, sb, len + 1, IntPtr.Zero) < 0) return null;
            return sb.ToString();
        }

        public static string GetU32(object o, Guid key)
        {
            IMFActivate a = (IMFActivate)o;
            uint v = 0;
            if (a.GetUINT32(ref key, out v) < 0) return null;
            return v.ToString();
        }

        public static string GetGuidStr(object o, Guid key)
        {
            IMFActivate a = (IMFActivate)o;
            Guid g = Guid.Empty;
            if (a.GetGUID(ref key, out g) < 0) return null;
            return g.ToString("B");
        }

        public static uint Count(object o)
        {
            IMFActivate a = (IMFActivate)o;
            uint c = 0;
            if (a.GetCount(out c) < 0) return 0;
            return c;
        }

        public static Guid KeyAt(object o, uint i)
        {
            IMFActivate a = (IMFActivate)o;
            Guid g = Guid.Empty;
            a.GetItemByIndex(i, out g, IntPtr.Zero);
            return g;
        }

        public static int TypeOf(object o, Guid key)
        {
            IMFActivate a = (IMFActivate)o;
            int t = 0;
            if (a.GetItemType(ref key, out t) < 0) return -1;
            return t;
        }

        public static uint BlobSize(object o, Guid key)
        {
            IMFActivate a = (IMFActivate)o;
            uint n = 0;
            if (a.GetBlobSize(ref key, out n) < 0) return 0;
            return n;
        }

        public static byte[] Blob(object o, Guid key)
        {
            IMFActivate a = (IMFActivate)o;
            uint n = BlobSize(a, key);
            if (n == 0) return new byte[0];
            byte[] b = new byte[n];
            if (a.GetBlob(ref key, b, n, IntPtr.Zero) < 0) return new byte[0];
            return b;
        }
    }
}
'@

Add-Type -TypeDefinition $cs -Language CSharp | Out-Null

# GUIDs taken verbatim from the SDK headers in toolchain\nuget (10.0.26100.0):
#   mfapi.h:1831-1833, mftransform.h:1618,1633,1640,1644-1648, mfd3d12.h:334
$catEncoder = [Guid]'f79eac7d-e545-4387-bdee-d647d7bde42a'   # MFT_CATEGORY_VIDEO_ENCODER
$catDecoder = [Guid]'d6c02d4b-6833-45b4-971a-05a4b04bab91'   # MFT_CATEGORY_VIDEO_DECODER (for contrast)
$known = @{
  '{314ffbae-5b41-4c95-9c19-4e7d586face3}' = 'MFT_FRIENDLY_NAME_Attribute';
  '{6821c42b-65a4-4e82-99bc-9a88205ecd0c}' = 'MFT_TRANSFORM_CLSID_Attribute';
  '{4276c9b1-759d-4bf3-9cd0-0d723d138f96}' = 'MFT_INPUT_TYPES_Attributes';
  '{8eae8cf3-a44f-4306-ba5c-bf5dda242818}' = 'MFT_OUTPUT_TYPES_Attributes';
  '{2fb866ac-b078-4942-ab6c-003d05cda674}' = 'MFT_ENUM_HARDWARE_URL_Attribute';
  '{3aecb0cc-035b-4bcc-8185-2b8d551ef3af}' = 'MFT_ENUM_HARDWARE_VENDOR_ID_Attribute';
  '{f81a699a-649a-497d-8c73-29f8fed6ad7a}' = 'MF_TRANSFORM_ASYNC';
  '{206b4fc8-fcf9-4c51-afe3-9764369e33a0}' = 'MF_SA_D3D11_AWARE';
  '{77f0bacb-17a8-4a50-9a7d-a5cc09d39d44}' = 'MF_SA_D3D12_AWARE'
}
$gName = [Guid]'314ffbae-5b41-4c95-9c19-4e7d586face3'
$gClsid = [Guid]'6821c42b-65a4-4e82-99bc-9a88205ecd0c'
$gHwUrl = [Guid]'2fb866ac-b078-4942-ab6c-003d05cda674'
$gHwVid = [Guid]'3aecb0cc-035b-4bcc-8185-2b8d551ef3af'
$gAsync = [Guid]'f81a699a-649a-497d-8c73-29f8fed6ad7a'
$gD3D11 = [Guid]'206b4fc8-fcf9-4c51-afe3-9764369e33a0'
$gD3D12 = [Guid]'77f0bacb-17a8-4a50-9a7d-a5cc09d39d44'
$gInTypes = [Guid]'4276c9b1-759d-4bf3-9cd0-0d723d138f96'
$gOutTypes = [Guid]'8eae8cf3-a44f-4306-ba5c-bf5dda242818'

# MF_ATTRIBUTE_TYPE values (mfobjects.h): UINT32 19, UINT64 21, DOUBLE 5, GUID 72, STRING 31, BLOB 0x1011, IUNKNOWN 13
function Type-Name([int]$t) {
  switch ($t) {
    19 { 'UINT32' } 21 { 'UINT64' } 5 { 'DOUBLE' } 72 { 'GUID' }
    31 { 'STRING' } 4113 { 'BLOB' } 13 { 'IUNKNOWN' } default { ('type=' + $t) }
  }
}

$hr = [Bc250Mf.Mf]::Startup()
Write-Output ('MFStartup -> 0x{0:X8}' -f $hr)
if ($hr -lt 0) { Write-Output 'MFStartup failed; nothing else can be enumerated.'; exit 1 }

$flagSets = @(
  @{ n = 'HARDWARE only (what a hardware-encoder client looks for)'; v = 0x00000004 },
  @{ n = 'ASYNCMFT (software async, V2)'; v = 0x00000002 },
  @{ n = 'SYNCMFT (software sync, V1)'; v = 0x00000001 },
  @{ n = 'LOCALMFT'; v = 0x00000010 },
  @{ n = 'ALL (SYNC|ASYNC|HARDWARE|FIELDOFUSE|LOCAL|TRANSCODE)'; v = 0x0000003F },
  @{ n = 'ALL | SORTANDFILTER'; v = 0x0000007F }
)

foreach ($cat in @(@{ n = 'MFT_CATEGORY_VIDEO_ENCODER'; g = $catEncoder }, @{ n = 'MFT_CATEGORY_VIDEO_DECODER'; g = $catDecoder })) {
  Write-Head ('category ' + $cat.n + '  ' + $cat.g.ToString('B'))
  foreach ($fs in $flagSets) {
    $e = 0
    $list = [Bc250Mf.Mf]::EnumMfts($cat.g, [uint32]$fs.v, [ref]$e)
    Write-Output ''
    Write-Output ('-- flags 0x{0:X8}  {1}  -> hr 0x{2:X8}  count {3}' -f $fs.v, $fs.n, $e, $list.Count)
    $i = 0
    foreach ($o in $list) {
      $a = $o  # helpers cast in C#; a PowerShell cast does not QueryInterface
      $nm = [Bc250Mf.Mf]::GetStr($a, $gName)
      $cl = [Bc250Mf.Mf]::GetGuidStr($a, $gClsid)
      Write-Output ('   [{0}] name="{1}" clsid={2}' -f $i, $nm, $cl)
      Write-Output ('        hwUrl={0} hwVendor={1} async={2} d3d11={3} d3d12={4}' -f `
        [Bc250Mf.Mf]::GetStr($a, $gHwUrl), [Bc250Mf.Mf]::GetStr($a, $gHwVid), `
        [Bc250Mf.Mf]::GetU32($a, $gAsync), [Bc250Mf.Mf]::GetU32($a, $gD3D11), `
        [Bc250Mf.Mf]::GetU32($a, $gD3D12))
      $inb = [Bc250Mf.Mf]::Blob($a, $gInTypes)
      $outb = [Bc250Mf.Mf]::Blob($a, $gOutTypes)
      Write-Output ('        inTypes={0}B outTypes={1}B  (each MFT_REGISTER_TYPE_INFO is 2 GUIDs = 32 B)' -f $inb.Length, $outb.Length)
      for ($k = 0; $k + 32 -le $inb.Length; $k += 32) {
        $maj = New-Object Guid (, [byte[]]($inb[$k..($k + 15)]))
        $sub = New-Object Guid (, [byte[]]($inb[($k + 16)..($k + 31)]))
        Write-Output ('          in  major={0} subtype={1}' -f $maj.ToString('B'), $sub.ToString('B'))
      }
      for ($k = 0; $k + 32 -le $outb.Length; $k += 32) {
        $maj = New-Object Guid (, [byte[]]($outb[$k..($k + 15)]))
        $sub = New-Object Guid (, [byte[]]($outb[($k + 16)..($k + 31)]))
        Write-Output ('          out major={0} subtype={1}' -f $maj.ToString('B'), $sub.ToString('B'))
      }
      # every attribute, so nothing a client might read is missed
      $cnt = [Bc250Mf.Mf]::Count($a)
      Write-Output ('        attributes: {0}' -f $cnt)
      for ($j = 0; $j -lt $cnt; $j++) {
        $key = [Bc250Mf.Mf]::KeyAt($a, [uint32]$j)
        $ks = $key.ToString('B')
        $t = [Bc250Mf.Mf]::TypeOf($a, $key)
        $label = '?'
        if ($known.ContainsKey($ks)) { $label = $known[$ks] }
        $val = switch ($t) {
          19 { [Bc250Mf.Mf]::GetU32($a, $key) }
          31 { '"' + [Bc250Mf.Mf]::GetStr($a, $key) + '"' }
          72 { [Bc250Mf.Mf]::GetGuidStr($a, $key) }
          4113 { ('<blob ' + [Bc250Mf.Mf]::BlobSize($a, $key) + ' B>') }
          default { '<not read>' }
        }
        Write-Output ('          {0} {1,-8} {2} = {3}' -f $ks, (Type-Name $t), $label, $val)
      }
      [void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($o)
      $i++
    }
  }
}

[void][Bc250Mf.Mf]::MFShutdown()

Write-Head 'registry: Media Foundation hardware-MFT gates (read-only)'
foreach ($p in @(
    'HKLM:\SOFTWARE\Microsoft\Windows Media Foundation\HardwareMFT',
    'HKLM:\SOFTWARE\Microsoft\Windows Media Foundation\Platform',
    'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Media Foundation\HardwareMFT')) {
  Write-Output ('-- ' + $p)
  if (Test-Path $p) { Get-ItemProperty $p | Format-List | Out-String | Write-Output }
  else { Write-Output '   (absent)' }
}

Write-Head 'registry: Game DVR / Game Bar policy and settings (read-only)'
foreach ($p in @(
    'HKCU:\System\GameConfigStore',
    'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\GameDVR',
    'HKLM:\SOFTWARE\Policies\Microsoft\Windows\GameDVR',
    'HKLM:\SOFTWARE\Microsoft\PolicyManager\default\ApplicationManagement\AllowGameDVR')) {
  Write-Output ('-- ' + $p)
  if (Test-Path $p) { Get-ItemProperty $p | Format-List | Out-String | Write-Output }
  else { Write-Output '   (absent)' }
}

Write-Head 'registry: video-encoder MFTs the system has registered (read-only)'
$catKey = 'HKLM:\SOFTWARE\Classes\MediaFoundation\Transforms\Categories\{f79eac7d-e545-4387-bdee-d647d7bde42a}'
if (Test-Path $catKey) {
  foreach ($k in (Get-ChildItem $catKey)) {
    $clsid = $k.PSChildName
    $tk = 'HKLM:\SOFTWARE\Classes\MediaFoundation\Transforms\' + $clsid
    $nm = ''
    if (Test-Path $tk) { try { $nm = (Get-ItemProperty $tk).'(default)' } catch { $nm = '' } }
    $ik = 'HKLM:\SOFTWARE\Classes\CLSID\' + $clsid + '\InprocServer32'
    $dll = ''
    if (Test-Path $ik) { try { $dll = (Get-ItemProperty $ik).'(default)' } catch { $dll = '' } }
    Write-Output ('   {0}  name="{1}"  server="{2}"' -f $clsid, $nm, $dll)
  }
} else { Write-Output '   (category key absent)' }

Write-Head 'adapters and our driver (read-only)'
Get-CimInstance Win32_VideoController |
  Select-Object Name, PNPDeviceID, DriverVersion, DriverDate, AdapterRAM, VideoProcessor |
  Format-List | Out-String | Write-Output

Write-Head 'mfplat / inbox encoder binaries present (read-only)'
foreach ($f in @('mfplat.dll', 'mfh264enc.dll', 'mfh263enc.dll', 'msmpeg2enc.dll', 'mfx_mft_h264ve_64.dll', 'amfrt64.dll', 'nvEncodeAPI64.dll')) {
  $p = Join-Path $env:SystemRoot ('System32\' + $f)
  if (Test-Path $p) {
    $v = (Get-Item $p).VersionInfo
    Write-Output ('   {0,-24} present  {1}' -f $f, $v.FileVersion)
  } else { Write-Output ('   {0,-24} absent' -f $f) }
}

Write-Output ''
Write-Output 'done (no object activated, no value written)'
