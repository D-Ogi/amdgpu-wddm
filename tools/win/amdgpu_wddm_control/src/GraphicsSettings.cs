// Graphics and Vulkan settings for all games and for one game (the owner's registry contract of 2026-10-08). Pure:
// no registry access here. RecoveryProbe reads the keys into the snapshot (GfxKeys), Recovery.Plan plans the writes
// through PlanWrites, and the elevated helper writes them.
//
//   HKLM\SOFTWARE\amdgpu-wddm\Graphics                         REG_DWORD values for all games
//   HKLM\SOFTWARE\amdgpu-wddm\Graphics\Applications\<image>    REG_DWORD values for one game (by file name)
//     FrameRateLimit      0 (no limit) or 20-300 frames per second
//     VSync               0 (always off) or 1 (always on)
//     Anisotropy          1, 2, 4, 8 or 16
//     MaxFrameLatency     1-3 frames queued ahead ("Low latency")
//     PerformanceOverlay  1 on (works in DirectX 11 games for now), 0 off
//     RenderOnCpu         1: the game's DirectX 11 runs on the processor (much slower, a last resort), 0 off
//     ReportAmdDriverVersion (for all games only)  1: the driver reports an AMD-compatible version, 0 off
//   HKLM\SOFTWARE\amdgpu-wddm\Vulkan                           REG_SZ values for all games
//   HKLM\SOFTWARE\amdgpu-wddm\Vulkan\Applications\<image>      REG_SZ values for one game
//     WsiRoute            dxgi (the default) or gdi; the reader also takes dxgi-composition, which this app does not
//                         offer, and takes any other value as gdi (radv_wddm2_wsi_route.h)
//     MemoryOverflow      allow (the default) or strict; any other value selects allow (radv_wddm2_mem_overflow.h)
//     Case does not matter and an empty string counts as absent.
//   The two Vulkan values are not offered in this release (AwaitingIcd below).
//
// An absent value is "Application decides" (the driver's default); a game's own value wins over the one for all
// games. The settings rule (owner, 2026-10-03): nothing is written for "Application decides" or "Same as all games",
// choosing it removes the value, and a game's key with no values left is removed. A stored value outside the
// contract shows as not valid and is never rewritten unless the person chooses another value.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;

namespace AmdgpuWddmControl
{
    // One registry value as read: Type DWord, String or Other (any other registry type, named in Text).
    public sealed class GfxValue
    {
        public string Type { get; set; }
        public long Number { get; set; }
        public string Text { get; set; }

        public static GfxValue Dword(long n) { return new GfxValue { Type = "DWord", Number = n }; }
        public static GfxValue Str(string t) { return new GfxValue { Type = "String", Text = t }; }
    }

    // One existing key under Graphics or Vulkan: its path below HKLM, every value (the unnamed one as ""), and the
    // number of its subkeys.
    public sealed class GfxKey
    {
        public string Path { get; set; }
        public Dictionary<string, GfxValue> Values { get; set; }
        public int SubKeys { get; set; }

        public GfxKey() { Values = new Dictionary<string, GfxValue>(StringComparer.OrdinalIgnoreCase); }
    }

    public sealed class GfxSetting
    {
        public string Name, Id, Root;
        public bool Text;                   // REG_SZ, else REG_DWORD
        public bool GlobalOnly;             // no per-game value (ReportAmdDriverVersion)
        public bool GameFirst;              // offered per game; the card for all games shows it only when it is stored
        public string[] Presets;            // the values offered, in the order shown
        public Func<long, bool> Valid;      // REG_DWORD: the contract's range
        public string SameAsAbsent;         // the value that does what an absent value does; hidden for all games unless stored
        public string AbsentTextId;         // the text of an absent value for all games
        public string InvalidMeans;         // what the driver does with a value it does not take; null: not known
        public string[] OtherValid;         // values the driver takes that this app does not offer

        public string SearchId { get { return (GameFirst ? "games." : "graphics.") + Id; } }
        public string Title { get { return Strings.T("search." + SearchId); } }
    }

    public enum GfxState { Absent, Valid, Other, Invalid }

    public sealed class GfxRead
    {
        public GfxState State;
        public string Value;                // Valid and Other: the value in its written form (decimal, lower case)
        public string Raw;                  // Invalid: the stored number, null for a value of another type or text
    }

    // What one setting is for one game (or for all games when Image is null), and where it comes from.
    public sealed class GfxView
    {
        public GfxSetting Setting;
        public string Image;
        public GfxRead Game, Global;
        public string Source;               // game, game-invalid, global, global-invalid, default
        public string Effective;            // the value in force; null: the application decides, or not known
        public GfxRead Here { get { return Image == null ? Global : Game; } }
    }

    // One entry of a setting's choice list. Absent: "Application decides" (all games) or "Same as all games" (a
    // game); Kept: the stored value that this app does not offer, selected means "leave it as it is".
    public sealed class GfxItem
    {
        public string Value, Text;
        public bool Absent, Kept;
    }

    public static class GraphicsSettings
    {
        public const string GraphicsPath = @"SOFTWARE\amdgpu-wddm\Graphics";
        public const string VulkanPath = @"SOFTWARE\amdgpu-wddm\Vulkan";
        public const string AppsKey = "Applications";
        public static readonly string[] Roots = { GraphicsPath, VulkanPath };
        public const string Unset = "unset";

        static bool Bool(long n) { return n == 0 || n == 1; }

        public static readonly GfxSetting[] All =
        {
            new GfxSetting { Name = "FrameRateLimit", Id = "fps", Root = GraphicsPath, SameAsAbsent = "0", AbsentTextId = "gfx.choice.app",
                Presets = new[] { "0", "20", "24", "25", "30", "40", "45", "48", "50", "60", "72", "75", "90", "100", "120", "144", "165", "180", "200", "240", "300" },
                Valid = n => n == 0 || (n >= 20 && n <= 300) },
            new GfxSetting { Name = "VSync", Id = "vsync", Root = GraphicsPath, AbsentTextId = "gfx.choice.app", Presets = new[] { "0", "1" }, Valid = Bool },
            new GfxSetting { Name = "Anisotropy", Id = "af", Root = GraphicsPath, AbsentTextId = "gfx.choice.app", Presets = new[] { "1", "2", "4", "8", "16" },
                Valid = n => n == 1 || n == 2 || n == 4 || n == 8 || n == 16 },
            new GfxSetting { Name = "MaxFrameLatency", Id = "latency", Root = GraphicsPath, AbsentTextId = "gfx.choice.app", Presets = new[] { "1", "2", "3" },
                Valid = n => n >= 1 && n <= 3 },
            new GfxSetting { Name = "PerformanceOverlay", Id = "overlay", Root = GraphicsPath, SameAsAbsent = "0", AbsentTextId = "gfx.choice.off", Presets = new[] { "1", "0" }, Valid = Bool },
            new GfxSetting { Name = "RenderOnCpu", Id = "cpu-render", Root = GraphicsPath, GameFirst = true, SameAsAbsent = "0", AbsentTextId = "gfx.choice.off", Presets = new[] { "1", "0" }, Valid = Bool },
            new GfxSetting { Name = "ReportAmdDriverVersion", Id = "amd-version", Root = GraphicsPath, GlobalOnly = true, SameAsAbsent = "0", AbsentTextId = "gfx.choice.off", Presets = new[] { "1", "0" }, Valid = Bool },
        };

        // The Vulkan settings wait for the ICD that reads them: the vk-wsi-dxgi line and the per-application keys of the
        // Mesa fork, which the b23 release does not carry. No ICD of b23 reads WsiRoute, and the 64-bit system Vulkan ICD
        // does not read MemoryOverflow. The owner's GUI rule is no control that does nothing or works only for some
        // programs, so they are not in All: no row, no search entry, no write and no --gfx name. The support report
        // still lists their stored values. They go back into All with the train that carries that ICD line.
        public static readonly GfxSetting[] AwaitingIcd =
        {
            new GfxSetting { Name = "WsiRoute", Id = "vk-present", Root = VulkanPath, Text = true, SameAsAbsent = "dxgi", AbsentTextId = "gfx.choice.modern-default",
                Presets = new[] { "dxgi", "gdi" }, InvalidMeans = "gdi", OtherValid = new[] { "dxgi-composition" } },
            new GfxSetting { Name = "MemoryOverflow", Id = "vk-memory", Root = VulkanPath, Text = true, SameAsAbsent = "allow", AbsentTextId = "gfx.choice.sysmem-default",
                Presets = new[] { "allow", "strict" }, InvalidMeans = "allow" },
        };

        public static GfxSetting Find(string name) { return All.FirstOrDefault(s => s.Name == name); }

        public static string AppPath(string root, string image) { return root + "\\" + AppsKey + "\\" + image; }

        public static string KeyPath(GfxSetting s, string image) { return image == null ? s.Root : AppPath(s.Root, image); }

        // The game of a per-game path under either root, or null (all games, or not one of the two roots).
        public static string AppImage(string path)
        {
            if (path == null) return null;
            foreach (var root in Roots)
            {
                var prefix = root + "\\" + AppsKey + "\\";
                if (!path.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)) continue;
                var image = path.Substring(prefix.Length);
                return Profiles.IsValidImage(image) ? image : null;
            }
            return null;
        }

        static string RootOf(string path)
        {
            foreach (var root in Roots)
                if (string.Equals(path, root, StringComparison.OrdinalIgnoreCase) || (path != null && path.StartsWith(root + "\\" + AppsKey + "\\", StringComparison.OrdinalIgnoreCase)))
                    return root;
            return null;
        }

        // The values this app may write: a setting's own name under its own root, for all games, or for one game
        // when the setting is not for all games only.
        public static bool Allowed(string path, string name)
        {
            var root = RootOf(path);
            if (root == null || name == null) return false;
            bool global = string.Equals(path, root, StringComparison.OrdinalIgnoreCase);
            if (!global && AppImage(path) == null) return false;
            return All.Any(s => s.Root == root && s.Name == name && (global || !s.GlobalOnly));
        }

        // The only keys this app removes: one game's key under either root, and only when it is empty (the helper
        // checks again before it removes it).
        public static bool KeyRemovalAllowed(string path) { return AppImage(path) != null; }

        public static bool Owns(RegWrite w)
        {
            return w != null && (w.DeleteKey ? KeyRemovalAllowed(w.Path) : Allowed(w.Path, w.Name));
        }

        // ---- reading ---------------------------------------------------------------------------------------------

        public static GfxKey Key(IList<GfxKey> keys, string path)
        {
            return keys == null ? null : keys.FirstOrDefault(k => k != null && string.Equals(k.Path, path, StringComparison.OrdinalIgnoreCase));
        }

        public static GfxValue Value(IList<GfxKey> keys, string path, string name)
        {
            var k = Key(keys, path);
            if (k == null || k.Values == null) return null;
            foreach (var kv in k.Values) if (string.Equals(kv.Key, name, StringComparison.OrdinalIgnoreCase)) return kv.Value;
            return null;
        }

        public static GfxRead Read(GfxSetting s, GfxValue v)
        {
            if (v == null) return new GfxRead { State = GfxState.Absent };
            if (s.Text)
            {
                if (v.Type != "String") return new GfxRead { State = GfxState.Invalid };
                var t = (v.Text ?? "").ToLowerInvariant();
                if (t.Length == 0) return new GfxRead { State = GfxState.Absent };
                if (s.Presets.Contains(t)) return new GfxRead { State = GfxState.Valid, Value = t };
                if (s.OtherValid != null && s.OtherValid.Contains(t)) return new GfxRead { State = GfxState.Other, Value = t };
                return new GfxRead { State = GfxState.Invalid };
            }
            if (v.Type != "DWord") return new GfxRead { State = GfxState.Invalid };
            var raw = v.Number.ToString(CultureInfo.InvariantCulture);
            return s.Valid(v.Number) ? new GfxRead { State = GfxState.Valid, Value = raw } : new GfxRead { State = GfxState.Invalid, Raw = raw };
        }

        // Precedence: a game's own value (valid or not: the readers take the first source that names a value) wins,
        // then the value for all games, then the driver's default.
        public static GfxView View(GfxSetting s, IList<GfxKey> keys, string image)
        {
            var v = new GfxView { Setting = s, Image = image };
            v.Global = Read(s, Value(keys, s.Root, s.Name));
            v.Game = image == null || s.GlobalOnly ? new GfxRead { State = GfxState.Absent } : Read(s, Value(keys, AppPath(s.Root, image), s.Name));
            if (v.Game.State == GfxState.Valid || v.Game.State == GfxState.Other) { v.Source = "game"; v.Effective = v.Game.Value; }
            else if (v.Game.State == GfxState.Invalid) { v.Source = "game-invalid"; v.Effective = s.InvalidMeans; }
            else if (v.Global.State == GfxState.Valid || v.Global.State == GfxState.Other) { v.Source = "global"; v.Effective = v.Global.Value; }
            else if (v.Global.State == GfxState.Invalid) { v.Source = "global-invalid"; v.Effective = s.InvalidMeans; }
            else v.Source = "default";
            return v;
        }

        // ---- texts -------------------------------------------------------------------------------------------------

        public static string Label(GfxSetting s, string value)
        {
            if (s.OtherValid != null && s.OtherValid.Contains(value)) return Strings.T("gfx.choice.other");
            switch (s.Name)
            {
                case "FrameRateLimit": return value == "0" ? Strings.T("gfx.choice.no-limit") : Strings.T("gfx.choice.fps", value);
                case "VSync": return Strings.T(value == "1" ? "gfx.choice.always-on" : "gfx.choice.always-off");
                case "Anisotropy": return value == "1" ? Strings.T("gfx.choice.af-off") : Strings.T("gfx.choice.af", value);
                case "MaxFrameLatency": return value == "1" ? Strings.T("gfx.choice.latency-1") : Strings.T("gfx.choice.latency", value);
                case "WsiRoute": return Strings.T(value == "gdi" ? "gfx.choice.compat" : "gfx.choice.modern");
                case "MemoryOverflow": return Strings.T(value == "strict" ? "gfx.choice.oom" : "gfx.choice.sysmem");
                default: return Strings.T(value == "1" ? "gfx.choice.on" : "gfx.choice.off");
            }
        }

        public static string AbsentText(GfxSetting s) { return Strings.T(s.AbsentTextId); }

        static string ReadText(GfxSetting s, GfxRead r)
        {
            switch (r.State)
            {
                case GfxState.Valid: return Label(s, r.Value);
                case GfxState.Other: return Strings.T("gfx.choice.other");
                case GfxState.Invalid: return r.Raw != null ? Strings.T("gfx.choice.invalid", r.Raw) : Strings.T("gfx.choice.invalid-type");
                default: return AbsentText(s);
            }
        }

        // What "Same as all games" means for this setting now.
        public static string GlobalText(GfxView v) { return ReadText(v.Setting, v.Global); }

        // The line under a setting: where its value comes from, and what a value that is not valid means.
        public static string OriginText(GfxView v, bool edited)
        {
            if (edited) return Strings.T("games.changed");
            var s = v.Setting;
            string means = s.InvalidMeans != null ? " " + Strings.T("gfx.origin.means", Label(s, s.InvalidMeans)) : "";
            if (v.Image == null)
            {
                if (v.Global.State == GfxState.Invalid) return Strings.T("gfx.origin.invalid-global") + means;
                if (v.Global.State == GfxState.Absent) return Strings.T("gfx.origin.default", AbsentText(s));
                return Strings.T("gfx.origin.stored");
            }
            switch (v.Source)
            {
                case "game": return Strings.T("game.origin.game");
                case "game-invalid": return Strings.T("gfx.origin.invalid-game") + means;
                case "global": return Strings.T("gfx.origin.global", GlobalText(v));
                case "global-invalid": return Strings.T("gfx.origin.invalid-global") + means;
                default: return Strings.T("gfx.origin.default", AbsentText(s));
            }
        }

        // ---- the choice list ---------------------------------------------------------------------------------------

        public static List<GfxItem> Items(GfxView v)
        {
            var s = v.Setting;
            var here = v.Here;
            var items = new List<GfxItem> { new GfxItem { Absent = true, Text = v.Image == null ? AbsentText(s) : Strings.T("gfx.choice.inherit", GlobalText(v)) } };
            var values = s.Presets.Where(x => v.Image != null || x != s.SameAsAbsent || (here.State == GfxState.Valid && here.Value == x)).ToList();
            if (here.State == GfxState.Valid && !values.Contains(here.Value))
            {
                // A valid value the presets do not have (58 fps): in its numeric place.
                long n = long.Parse(here.Value, CultureInfo.InvariantCulture);
                int at = values.FindIndex(x => !s.Text && long.Parse(x, CultureInfo.InvariantCulture) > n);
                values.Insert(at < 0 ? values.Count : at, here.Value);
            }
            foreach (var x in values) items.Add(new GfxItem { Value = x, Text = Label(s, x) });
            if (here.State == GfxState.Other || here.State == GfxState.Invalid) items.Add(new GfxItem { Kept = true, Value = here.Value, Text = ReadText(s, here) });
            return items;
        }

        // The item shown as selected: the person's change when there is one, else the stored value.
        public static int Selected(List<GfxItem> items, GfxRead here, bool edited, string edit)
        {
            if (edited)
            {
                if (edit == null) return 0;
                int i = items.FindIndex(x => !x.Kept && !x.Absent && x.Value == edit);
                if (i >= 0) return i;
            }
            switch (here.State)
            {
                case GfxState.Valid: { int i = items.FindIndex(x => !x.Kept && !x.Absent && x.Value == here.Value); return i < 0 ? 0 : i; }
                case GfxState.Other:
                case GfxState.Invalid: { int i = items.FindIndex(x => x.Kept); return i < 0 ? 0 : i; }
                default: return 0;
            }
        }

        // Records the person's choice: a choice that equals what is stored is no change (the edit goes away), so a
        // stored value is never rewritten by looking at it.
        public static void Choose(IDictionary<string, string> edits, GfxSetting s, GfxRead here, GfxItem item)
        {
            bool same = item.Kept || (item.Absent && here.State == GfxState.Absent) ||
                (!item.Absent && here.State == GfxState.Valid && here.Value == item.Value);
            if (same) edits.Remove(s.Name); else edits[s.Name] = item.Absent ? null : item.Value;
        }

        // The check box of a setting with one "on" value (ReportAmdDriverVersion): checked = 1 stored or chosen.
        public static bool Checked(GfxRead here, bool edited, string edit)
        {
            return edited ? edit == "1" : here.State == GfxState.Valid && here.Value == "1";
        }

        public static void Check(IDictionary<string, string> edits, GfxSetting s, GfxRead here, bool on)
        {
            bool storedOn = here.State == GfxState.Valid && here.Value == "1";
            if (on == storedOn) { edits.Remove(s.Name); return; }
            if (on) edits[s.Name] = "1";
            else if (storedOn) edits[s.Name] = null;
            else edits.Remove(s.Name);
        }

        // ---- planning ----------------------------------------------------------------------------------------------

        // The written form of a value, or null when the contract does not take it.
        public static string Normalize(GfxSetting s, string value)
        {
            if (value == null) return null;
            if (s.Text)
            {
                var t = value.ToLowerInvariant();
                return s.Presets.Contains(t) ? t : null;
            }
            uint n;
            if (!uint.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out n) || !s.Valid(n)) return null;
            return n.ToString(CultureInfo.InvariantCulture);
        }

        // The writes of one scope (image null: all games). edits: setting name -> the chosen value, or null for
        // "Application decides" / "Same as all games". A setting without an edit is left exactly as stored.
        // Throws ArgumentException for an unknown name, a setting outside the scope or a value outside the contract.
        public static List<RegWrite> PlanWrites(IList<GfxKey> keys, string image, IDictionary<string, string> edits)
        {
            if (image != null && !Profiles.IsValidImage(image)) throw new ArgumentException("not a game file name: " + image);
            var writes = new List<RegWrite>();
            foreach (var name in (edits ?? new Dictionary<string, string>()).Keys)
                if (Find(name) == null) throw new ArgumentException("not a graphics setting: " + name);
            foreach (var s in All)
            {
                string edit;
                if (edits == null || !edits.TryGetValue(s.Name, out edit)) continue;
                if (image != null && s.GlobalOnly) throw new ArgumentException(s.Name + " is a setting for all games only");
                var path = KeyPath(s, image);
                var raw = Value(keys, path, s.Name);
                if (edit == null) { if (raw != null) writes.Add(RegWrite.Remove(path, s.Name)); continue; }
                var value = Normalize(s, edit);
                if (value == null) throw new ArgumentException(edit + " is not a value of " + s.Name);
                var stored = Read(s, raw);
                // The same value is no change, also in another case ("GDI" for gdi: the readers ignore case); a value
                // of another type is rewritten.
                if (stored.State == GfxState.Valid && stored.Value == value) continue;
                writes.Add(s.Text ? RegWrite.Str(path, s.Name, value) : RegWrite.Dword(path, s.Name, uint.Parse(value, CultureInfo.InvariantCulture)));
            }
            if (image != null) AddKeyRemovals(writes, keys);
            return writes;
        }

        // A game's key whose every value the writes remove, and which has no subkey, is removed after them (the
        // settings rule: no empty key is left behind). Keys for all games stay.
        public static void AddKeyRemovals(List<RegWrite> writes, IList<GfxKey> keys)
        {
            var paths = writes.Where(w => !w.DeleteKey && AppImage(w.Path) != null).Select(w => w.Path).Distinct(StringComparer.OrdinalIgnoreCase).ToList();
            foreach (var path in paths)
            {
                if (writes.Any(w => w.DeleteKey && string.Equals(w.Path, path, StringComparison.OrdinalIgnoreCase))) continue;
                var key = Key(keys, path);
                if (key == null || key.SubKeys > 0) continue;
                var names = new HashSet<string>(key.Values != null ? key.Values.Keys : Enumerable.Empty<string>(), StringComparer.OrdinalIgnoreCase);
                foreach (var w in writes.Where(x => !x.DeleteKey && string.Equals(x.Path, path, StringComparison.OrdinalIgnoreCase)))
                    if (w.Delete) names.Remove(w.Name); else names.Add(w.Name);
                if (names.Count == 0) writes.Add(RegWrite.RemoveKey(path));
            }
        }

        // "Restore defaults": every value of this contract for all games removed, and with the games also every
        // game's values (and the keys left empty). Values this app does not know stay.
        public static List<RegWrite> ResetWrites(IList<GfxKey> keys, bool games)
        {
            var writes = new List<RegWrite>();
            foreach (var s in All)
                if (Value(keys, s.Root, s.Name) != null) writes.Add(RegWrite.Remove(s.Root, s.Name));
            if (games && keys != null)
            {
                foreach (var k in keys.Where(k => k != null && AppImage(k.Path) != null).OrderBy(k => k.Path, StringComparer.OrdinalIgnoreCase))
                    foreach (var s in All.Where(x => !x.GlobalOnly && string.Equals(AppPath(x.Root, AppImage(k.Path)), k.Path, StringComparison.OrdinalIgnoreCase)))
                        if (Value(keys, k.Path, s.Name) != null) writes.Add(RegWrite.Remove(k.Path, s.Name));
                AddKeyRemovals(writes, keys);
            }
            return writes;
        }

        // The games with a key of their own under either root.
        public static List<string> Games(IList<GfxKey> keys)
        {
            return (keys ?? new List<GfxKey>()).Where(k => k != null).Select(k => AppImage(k.Path)).Where(i => i != null)
                .Distinct(StringComparer.OrdinalIgnoreCase).OrderBy(i => i, StringComparer.OrdinalIgnoreCase).ToList();
        }

        public static bool HasOwn(IList<GfxKey> keys, string image)
        {
            return All.Any(s => !s.GlobalOnly && Value(keys, AppPath(s.Root, image), s.Name) != null);
        }

        // ---- the helper's argument ---------------------------------------------------------------------------------

        // "--gfx FrameRateLimit=60,VSync=unset": one entry per setting, in any order. Null when a name is unknown or
        // repeated, a value is outside the contract, or the list is empty.
        public static Dictionary<string, string> ParseEdits(string text)
        {
            if (string.IsNullOrEmpty(text) || text.Length > 512) return null;
            var edits = new Dictionary<string, string>(StringComparer.Ordinal);
            foreach (var part in text.Split(','))
            {
                int eq = part.IndexOf('=');
                if (eq <= 0) return null;
                var s = Find(part.Substring(0, eq));
                var value = part.Substring(eq + 1);
                if (s == null || edits.ContainsKey(s.Name)) return null;
                if (value == Unset) { edits[s.Name] = null; continue; }
                var n = Normalize(s, value);
                if (n == null || n != value) return null;
                edits[s.Name] = n;
            }
            return edits.Count == 0 ? null : edits;
        }

        public static string FormatEdits(IDictionary<string, string> edits)
        {
            return string.Join(",", All.Where(s => edits != null && edits.ContainsKey(s.Name)).Select(s => s.Name + "=" + (edits[s.Name] ?? Unset)));
        }

        // ---- plain words -------------------------------------------------------------------------------------------

        public static string LineId(RegWrite w)
        {
            if (w.DeleteKey) return "plan.line.gfx-key";
            return AppImage(w.Path) != null ? "plan.line.gfx-game-set" : "plan.line.gfx-set";
        }

        // The dialog's sentence for one write (the window's language).
        public static string PlainLine(RegWrite w)
        {
            var image = AppImage(w.Path);
            if (w.DeleteKey) return Strings.T("plan.line.gfx-key", image);
            var root = RootOf(w.Path);
            var s = All.First(x => x.Root == root && x.Name == w.Name);
            string text = w.Delete ? (image == null ? AbsentText(s) : Strings.T("gfx.choice.inherit-short"))
                : Label(s, s.Text ? (w.Text ?? "").ToLowerInvariant() : w.Number.ToString(CultureInfo.InvariantCulture));
            return image == null ? Strings.T("plan.line.gfx-set", s.Title, text) : Strings.T("plan.line.gfx-game-set", image, s.Title, text);
        }

        // English, for the plan text and the log.
        public static string Describe(IEnumerable<RegWrite> writes)
        {
            return string.Join(" ", writes.Select(w => w.DeleteKey ? "Removes the empty key of " + AppImage(w.Path) + "." :
                (AppImage(w.Path) != null ? AppImage(w.Path) + ": " : "All games: ") + w.Name + (w.Delete ? " removed." : " = " + (w.Kind == "String" ? w.Text : w.Number.ToString(CultureInfo.InvariantCulture)) + ".")));
        }

        // ---- the support report ------------------------------------------------------------------------------------

        public static string Report(IList<GfxKey> keys)
        {
            var w = new StringBuilder();
            if (keys == null) { w.AppendLine("graphics settings: unreadable"); return w.ToString(); }
            foreach (var k in keys.Where(k => k != null).OrderBy(k => k.Path, StringComparer.OrdinalIgnoreCase))
            {
                w.AppendLine("[HKLM\\" + k.Path + "]" + (k.SubKeys > 0 ? " (" + k.SubKeys + " subkeys)" : ""));
                foreach (var kv in (k.Values ?? new Dictionary<string, GfxValue>()).OrderBy(x => x.Key, StringComparer.OrdinalIgnoreCase))
                    w.AppendLine((kv.Key.Length == 0 ? "(default)" : kv.Key) + " = " + (kv.Value == null ? "?" : kv.Value.Type == "DWord" ? kv.Value.Number.ToString(CultureInfo.InvariantCulture) + " (DWord)"
                        : kv.Value.Type == "String" ? "\"" + kv.Value.Text + "\" (String)" : "(" + kv.Value.Text + ")"));
            }
            if (keys.Count == 0) w.AppendLine("no Graphics or Vulkan settings keys");
            w.AppendLine("effective, all games:");
            foreach (var s in All) w.AppendLine("  " + s.Name + " = " + EffectiveLine(View(s, keys, null)));
            foreach (var image in Games(keys))
            {
                w.AppendLine("effective, " + image + ":");
                foreach (var s in All.Where(x => !x.GlobalOnly)) w.AppendLine("  " + s.Name + " = " + EffectiveLine(View(s, keys, image)));
            }
            return w.ToString();
        }

        static string EffectiveLine(GfxView v)
        {
            string value = v.Effective ?? (v.Source.EndsWith("-invalid", StringComparison.Ordinal) ? "not known" : "application decides");
            var here = v.Image == null ? v.Global : v.Game;
            string invalid = v.Source.EndsWith("-invalid", StringComparison.Ordinal) ? ", stored value not valid" + (here.Raw != null ? " (" + here.Raw + ")" : "") : "";
            if (v.Source == "global-invalid" && v.Global.Raw != null) invalid = ", stored value for all games not valid (" + v.Global.Raw + ")";
            return value + " (" + v.Source + invalid + ")";
        }
    }
}
