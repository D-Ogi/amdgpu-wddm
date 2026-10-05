// The app's own preferences: HKCU\Software\amdgpu-wddm\Control (docs/gui/interfaces.md section 5). App state only;
// the installer never writes them, so they survive updates (WU-049, C19). Rule R4: every preference is worded so that
// its default is "value absent"; checking a box writes one value, unchecking it removes the value, and reading never
// writes. The two owner exceptions are on while absent: D2 (update check at start) writes 0 when turned off and removes
// the value when turned on; D3 (recent launches, read by the shells) writes 1 or 0 on every change.
using System;
using System.Collections.Generic;
using System.Linq;
using Microsoft.Win32;

namespace AmdgpuWddmControl
{
    // The few operations the preferences need, so that tests run against a dictionary and the window against HKCU.
    public interface IPrefStore
    {
        // uint, string, string[]; null when absent; PrefValue.Unusable when present but of another type or unreadable
        object Get(string name);
        void SetDword(string name, uint value);
        void SetString(string name, string value);
        void SetMulti(string name, string[] value);
        void Delete(string name);
    }

    public static class PrefValue
    {
        public static readonly object Unusable = new object();
    }

    public sealed class MemoryPrefStore : IPrefStore
    {
        public readonly Dictionary<string, object> Values = new Dictionary<string, object>(StringComparer.OrdinalIgnoreCase);
        public int Writes;
        public object Get(string name) { object v; return Values.TryGetValue(name, out v) ? v : null; }
        public void SetDword(string name, uint value) { Values[name] = value; Writes++; }
        public void SetString(string name, string value) { Values[name] = value; Writes++; }
        public void SetMulti(string name, string[] value) { Values[name] = value; Writes++; }
        public void Delete(string name) { if (Values.Remove(name)) Writes++; }
    }

    public sealed class RegistryPrefStore : IPrefStore
    {
        public const string Path = @"Software\amdgpu-wddm\Control";

        public object Get(string name)
        {
            try
            {
                using (var k = Registry.CurrentUser.OpenSubKey(Path))
                {
                    if (k == null) return null;
                    var v = k.GetValue(name);
                    if (v == null) return null;
                    if (v is int) return unchecked((uint)(int)v);
                    return v is string || v is string[] ? v : PrefValue.Unusable;
                }
            }
            catch (Exception) { return PrefValue.Unusable; }
        }

        static void With(Action<RegistryKey> a)
        {
            try { using (var k = Registry.CurrentUser.CreateSubKey(Path)) a(k); }
            catch (Exception) { }
        }

        public void SetDword(string name, uint value) { With(k => k.SetValue(name, unchecked((int)value), RegistryValueKind.DWord)); }
        public void SetString(string name, string value) { With(k => k.SetValue(name, value, RegistryValueKind.String)); }
        public void SetMulti(string name, string[] value) { With(k => k.SetValue(name, value, RegistryValueKind.MultiString)); }

        public void Delete(string name)
        {
            try { using (var k = Registry.CurrentUser.OpenSubKey(Path, true)) if (k != null) k.DeleteValue(name, false); }
            catch (Exception) { }
        }
    }

    public sealed class AppPrefs
    {
        readonly IPrefStore _store;
        public AppPrefs(IPrefStore store) { _store = store; }

        public static AppPrefs Live() { return new AppPrefs(new RegistryPrefStore()); }

        uint? Dword(string name) { var v = _store.Get(name); return v is uint ? (uint?)(uint)v : null; }

        // Off unless the value is 1; checking writes 1, unchecking removes the value.
        bool Flag(string name) { return Dword(name) == 1; }
        void SetFlag(string name, bool on) { if (on) { if (Dword(name) != 1) _store.SetDword(name, 1); } else if (_store.Get(name) != null) _store.Delete(name); }

        // On unless the value is 0 (D2, D3); unchecking writes 0, checking removes the value.
        bool OnUnlessZero(string name) { return Dword(name) != 0; }
        void SetOnUnlessZero(string name, bool on) { if (on) { if (_store.Get(name) != null) _store.Delete(name); } else if (Dword(name) != 0) _store.SetDword(name, 0); }

        public bool ShowNagi { get { return Flag("ShowNagi"); } set { SetFlag("ShowNagi", value); } }
        public bool ShowTipsAutomatically { get { return Flag("ShowTipsAutomatically"); } set { SetFlag("ShowTipsAutomatically", value); } }
        public bool ReduceAnimations { get { return Flag("ReduceAnimations"); } set { SetFlag("ReduceAnimations", value); } }
        public bool ShowSupportOptions { get { return Flag("ShowSupportOptions"); } set { SetFlag("ShowSupportOptions", value); } }
        public bool GettingStartedDismissed { get { return Flag("GettingStartedDismissed"); } set { SetFlag("GettingStartedDismissed", value); } }
        public bool UpdateCheckAtStart { get { return OnUnlessZero("UpdateCheckAtStart"); } set { SetOnUnlessZero("UpdateCheckAtStart", value); } }
        // D3, read by the shells (docs/design/recent-launches.md "Switch"): absent or 1 = on, 0 = off, anything else
        // records nothing and shows as off, "not valid". A change writes 1 or 0.
        public RecentSwitch RecentLaunchesSwitch
        {
            get { var v = _store.Get(RecentLaunches.SwitchName); return v == null ? RecentSwitch.On : !(v is uint) ? RecentSwitch.Invalid : (uint)v == 1 ? RecentSwitch.On : (uint)v == 0 ? RecentSwitch.Off : RecentSwitch.Invalid; }
        }
        public bool RecordRecentLaunches { get { return RecentLaunchesSwitch == RecentSwitch.On; } set { if (value != RecordRecentLaunches || RecentLaunchesSwitch == RecentSwitch.Invalid) _store.SetDword(RecentLaunches.SwitchName, value ? 1u : 0u); } }

        // The chosen language; null: none chosen (Windows' language, else English). Written only on a choice.
        public string Language
        {
            get { var v = _store.Get("Language") as string; return Strings.Languages.Contains(v) ? v : null; }
            set { if (value == null) _store.Delete("Language"); else if (Strings.Languages.Contains(value) && Language != value) _store.SetString("Language", value); }
        }

        public string EffectiveLanguage { get { return Language ?? Strings.SystemLanguage(); } }

        public string LastSeenRelease
        {
            get { return _store.Get("LastSeenRelease") as string; }
            set { if (value == null) _store.Delete("LastSeenRelease"); else if (LastSeenRelease != value) _store.SetString("LastSeenRelease", value); }
        }

        // Hidden entries of the Games list (WU-013): their keys; hiding never touches a game's files or saves.
        public HashSet<string> HiddenGames
        {
            get { return new HashSet<string>(_store.Get("HiddenGames") as string[] ?? new string[0], StringComparer.OrdinalIgnoreCase); }
            set
            {
                var list = (value ?? new HashSet<string>()).OrderBy(k => k, StringComparer.OrdinalIgnoreCase).ToArray();
                if (list.Length == 0) { if (_store.Get("HiddenGames") != null) _store.Delete("HiddenGames"); }
                else if (!HiddenGames.SetEquals(list)) _store.SetMulti("HiddenGames", list);
            }
        }

        // Windows "Animation effects" (SPI_GETCLIENTAREAANIMATION); null when it cannot be read.
        public static bool? WindowsAnimations()
        {
            try { bool on; return SystemParametersInfo(SpiGetClientAreaAnimation, 0, out on, 0) ? (bool?)on : null; }
            catch (Exception) { return null; }
        }

        const uint SpiGetClientAreaAnimation = 0x1042;

        [System.Runtime.InteropServices.DllImport("user32.dll", SetLastError = true)]
        static extern bool SystemParametersInfo(uint action, uint param, out bool value, uint winIni);
    }
}
