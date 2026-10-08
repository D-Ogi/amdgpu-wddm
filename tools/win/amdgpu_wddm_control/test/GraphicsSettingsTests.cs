// The graphics settings for games (src/GraphicsSettings.cs, the registry contract of 2026-10-08): every value of every
// setting, the precedence of a game's own value over the one for all games, stored values outside the contract (shown,
// never rewritten), the settings rule (no write for "Application decides", a game's empty key removed), the helper's
// argument, the Recovery plans that carry them, the shader model choice, and the Display page's pure helpers.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Linq;
using AmdgpuWddmControl;

static partial class UnitTests
{
    static GfxKey GKey(string path, int subKeys, params object[] nameValue)
    {
        var k = new GfxKey { Path = path, SubKeys = subKeys };
        for (int i = 0; i < nameValue.Length; i += 2)
            k.Values[(string)nameValue[i]] = nameValue[i + 1] is string ? GfxValue.Str((string)nameValue[i + 1]) : nameValue[i + 1] as GfxValue ?? GfxValue.Dword(Convert.ToInt64(nameValue[i + 1]));
        return k;
    }

    static string W3G { get { return GraphicsSettings.AppPath(GraphicsSettings.GraphicsPath, "witcher3.exe"); } }
    static string W3V { get { return GraphicsSettings.AppPath(GraphicsSettings.VulkanPath, "witcher3.exe"); } }

    static string WriteText(IEnumerable<RegWrite> writes) { return string.Join("; ", writes.Select(w => w.ToString())); }

    // A setting of the contract, offered or waiting for its ICD (GraphicsSettings.AwaitingIcd): the pure readers take it.
    static GfxSetting AnySetting(string name)
    {
        return GraphicsSettings.Find(name) ?? GraphicsSettings.AwaitingIcd.First(s => s.Name == name);
    }

    static void GraphicsSettingsTests()
    {
        Strings.Language = "en";
        GfxContract();
        GfxPrecedence();
        GfxChoices();
        GfxPlanning();
        GfxArguments();
        GfxPlans();
        GfxShaderModel();
        GfxDisplay();
    }

    // Every value of the contract: what is valid, how it reads and what it is called.
    static void GfxContract()
    {
        Equal(7, GraphicsSettings.All.Length, "contract: seven settings offered");
        Equal(2, GraphicsSettings.AwaitingIcd.Length, "contract: two Vulkan settings wait for their ICD");
        Check(GraphicsSettings.AwaitingIcd.All(s => GraphicsSettings.Find(s.Name) == null && s.Root == GraphicsSettings.VulkanPath), "contract: a Vulkan setting that waits is not offered");
        var contract = GraphicsSettings.All.Concat(GraphicsSettings.AwaitingIcd).ToArray();
        Equal(contract.Length, contract.Select(s => s.Id).Distinct().Count(), "contract: the ids are unique");
        foreach (var s in contract)
        {
            Check(s.Root == (s.Text ? GraphicsSettings.VulkanPath : GraphicsSettings.GraphicsPath), "contract: " + s.Name + " is under its root");
            Check(Strings.Has("search." + s.SearchId) && Strings.Has(s.AbsentTextId), "contract: " + s.Name + " has its texts");
            foreach (var v in s.Presets)
            {
                Equal(v, GraphicsSettings.Normalize(s, v), "contract: " + s.Name + " preset " + v + " is valid");
                var r = GraphicsSettings.Read(s, s.Text ? GfxValue.Str(v) : GfxValue.Dword(long.Parse(v)));
                Check(r.State == GfxState.Valid && r.Value == v, "contract: " + s.Name + " " + v + " reads as valid");
                var label = GraphicsSettings.Label(s, v);
                Check(label.Length > 0 && !label.StartsWith("[", StringComparison.Ordinal), "contract: " + s.Name + " " + v + " has a label: " + label);
                Check(!PlainWords.Findings(new[] { label }).Any(), "G-NOINT: the label of " + s.Name + " " + v);
            }
            Equal(s.Presets.Length, s.Presets.Select(v => GraphicsSettings.Label(s, v)).Distinct().Count(), "contract: the labels of " + s.Name + " differ");
        }
        // The DWORD ranges, value by value.
        Func<string, long, bool> valid = (name, n) => GraphicsSettings.Read(GraphicsSettings.Find(name), GfxValue.Dword(n)).State == GfxState.Valid;
        for (long n = 0; n <= 400; n++)
        {
            Equal(n == 0 || (n >= 20 && n <= 300), valid("FrameRateLimit", n), "FrameRateLimit " + n);
            Equal(n == 0 || n == 1, valid("VSync", n), "VSync " + n);
            Equal(n == 1 || n == 2 || n == 4 || n == 8 || n == 16, valid("Anisotropy", n), "Anisotropy " + n);
            Equal(n >= 1 && n <= 3, valid("MaxFrameLatency", n), "MaxFrameLatency " + n);
            foreach (var b in new[] { "PerformanceOverlay", "RenderOnCpu", "ReportAmdDriverVersion" }) Equal(n == 0 || n == 1, valid(b, n), b + " " + n);
        }
        Check(!valid("FrameRateLimit", 0xFFFFFFFFL) && !valid("FrameRateLimit", -1), "FrameRateLimit: 0xFFFFFFFF and -1 are not valid");
        var fps = GraphicsSettings.Find("FrameRateLimit");
        Equal("58", GraphicsSettings.Normalize(fps, "58"), "FrameRateLimit 58 is in the contract though not a preset");
        foreach (var bad in new[] { "19", "301", "-1", "+60", " 60", "60 ", "0x3C", "60.0", "", "abc", "99999999999" })
            Equal(null, GraphicsSettings.Normalize(fps, bad), "FrameRateLimit '" + bad + "' is refused");
        Equal("60", GraphicsSettings.Normalize(fps, "060"), "FrameRateLimit 060 is written as 60");
        Equal("No limit", GraphicsSettings.Label(fps, "0"), "label: no limit");
        Equal("144 fps", GraphicsSettings.Label(fps, "144"), "label: 144 fps");
        Equal("1x (off)", GraphicsSettings.Label(GraphicsSettings.Find("Anisotropy"), "1"), "label: 1x is off");
        Equal("16x", GraphicsSettings.Label(GraphicsSettings.Find("Anisotropy"), "16"), "label: 16x");
        Equal("1 frame ahead (lowest latency)", GraphicsSettings.Label(GraphicsSettings.Find("MaxFrameLatency"), "1"), "label: one frame ahead");
        Equal("3 frames ahead", GraphicsSettings.Label(GraphicsSettings.Find("MaxFrameLatency"), "3"), "label: three frames ahead");
        Equal("Always on", GraphicsSettings.Label(GraphicsSettings.Find("VSync"), "1"), "label: vsync on");

        // The Vulkan strings: case does not matter, an empty string is absent, dxgi-composition is valid but not offered.
        var route = AnySetting("WsiRoute");
        var mem = AnySetting("MemoryOverflow");
        Check(GraphicsSettings.Read(route, GfxValue.Str("GDI")).Value == "gdi", "WsiRoute: GDI reads as gdi");
        Equal(GfxState.Absent, GraphicsSettings.Read(route, GfxValue.Str("")).State, "WsiRoute: an empty string is absent");
        Equal(GfxState.Other, GraphicsSettings.Read(route, GfxValue.Str("DXGI-Composition")).State, "WsiRoute: dxgi-composition is set outside this app");
        Equal(GfxState.Invalid, GraphicsSettings.Read(route, GfxValue.Str("vulkan")).State, "WsiRoute: another text is not valid");
        Equal(GfxState.Invalid, GraphicsSettings.Read(route, GfxValue.Dword(1)).State, "WsiRoute: a number is not valid");
        Equal(GfxState.Invalid, GraphicsSettings.Read(fps, GfxValue.Str("60")).State, "FrameRateLimit: a text is not valid");
        Equal(GfxState.Invalid, GraphicsSettings.Read(fps, new GfxValue { Type = "QWord", Number = 60 }).State, "FrameRateLimit: a QWord is not valid");
        Equal("Compatibility", GraphicsSettings.Label(route, "gdi"), "label: gdi is Compatibility");
        Equal("Modern", GraphicsSettings.Label(route, "dxgi"), "label: dxgi is Modern");
        Equal("Set outside this app", GraphicsSettings.Label(route, "dxgi-composition"), "label: dxgi-composition");
        Check(GraphicsSettings.Label(mem, "strict").StartsWith("Report out of memory", StringComparison.Ordinal), "label: strict reports out of memory");
        Check(GraphicsSettings.Label(mem, "allow").StartsWith("Use system memory", StringComparison.Ordinal), "label: allow uses system memory");
        Equal("gdi", GraphicsSettings.Normalize(route, "GDI"), "WsiRoute: written in lower case");
        Equal(null, GraphicsSettings.Normalize(route, "dxgi-composition"), "WsiRoute: dxgi-composition is never written by this app");

        // The allow-lists: a setting's own name under its own root, per game unless it is for all games only.
        Check(GraphicsSettings.Allowed(GraphicsSettings.GraphicsPath, "FrameRateLimit") && GraphicsSettings.Allowed(W3G, "FrameRateLimit"), "Allowed: FrameRateLimit for all games and one game");
        Check(GraphicsSettings.Allowed(GraphicsSettings.GraphicsPath, "ReportAmdDriverVersion") && !GraphicsSettings.Allowed(W3G, "ReportAmdDriverVersion"), "Allowed: ReportAmdDriverVersion for all games only");
        Check(GraphicsSettings.Allowed(W3G, "VSync") && !GraphicsSettings.Allowed(W3V, "VSync"), "Allowed: each name under its own root only");
        Check(!GraphicsSettings.Allowed(W3V, "WsiRoute") && !GraphicsSettings.Allowed(GraphicsSettings.VulkanPath, "MemoryOverflow") && !GraphicsSettings.Allowed(W3G, "WsiRoute"),
            "Allowed: no Vulkan name while it waits for its ICD");
        Check(!GraphicsSettings.Allowed(GraphicsSettings.GraphicsPath, "Other") && !GraphicsSettings.Allowed(GraphicsSettings.GraphicsPath + @"\Applications", "VSync") &&
            !GraphicsSettings.Allowed(GraphicsSettings.GraphicsPath + @"\Applications\..\x.exe", "VSync") && !GraphicsSettings.Allowed(GraphicsSettings.GraphicsPath + @"\Other\witcher3.exe", "VSync") &&
            !GraphicsSettings.Allowed(@"SOFTWARE\amdgpu-wddm\Applications\witcher3.exe", "VSync"), "Allowed: nothing else");
        Check(Recovery.Allowed(W3G, "VSync") && !Recovery.Allowed(W3V, "MemoryOverflow") && !Recovery.Allowed(W3V, "Bogus"), "Recovery.Allowed takes the offered graphics settings");
        Check(GraphicsSettings.KeyRemovalAllowed(W3G) && GraphicsSettings.KeyRemovalAllowed(W3V), "KeyRemovalAllowed: one game's keys");
        Check(!GraphicsSettings.KeyRemovalAllowed(GraphicsSettings.GraphicsPath) && !GraphicsSettings.KeyRemovalAllowed(GraphicsSettings.VulkanPath) &&
            !GraphicsSettings.KeyRemovalAllowed(GraphicsSettings.GraphicsPath + @"\Applications") && !GraphicsSettings.KeyRemovalAllowed(Profiles.RegistryPath + @"\witcher3.exe"),
            "KeyRemovalAllowed: never a root, the Applications key or another key");
        Equal("witcher3.exe", GraphicsSettings.AppImage(W3V.ToUpperInvariant().Replace("WITCHER3.EXE", "witcher3.exe")), "AppImage: case of the path does not matter");
    }

    // A game's own value wins, valid or not; then the value for all games; then the driver's default.
    static void GfxPrecedence()
    {
        var keys = new List<GfxKey>
        {
            GKey(GraphicsSettings.GraphicsPath, 1, "FrameRateLimit", 60, "PerformanceOverlay", 7, "VSync", 1),
            GKey(GraphicsSettings.VulkanPath, 1, "WsiRoute", "gdi", "MemoryOverflow", "nonsense"),
            GKey(W3G, 0, "FrameRateLimit", 144, "Anisotropy", 7),
            GKey(W3V, 0, "WsiRoute", "dxgi-composition"),
        };
        Func<string, string, GfxView> view = (name, image) => GraphicsSettings.View(AnySetting(name), keys, image);
        var v = view("FrameRateLimit", "witcher3.exe");
        Check(v.Source == "game" && v.Effective == "144", "precedence: the game's 144 wins over 60");
        Equal("Set for this game", GraphicsSettings.OriginText(v, false), "origin: the game's own");
        v = view("FrameRateLimit", "other.exe");
        Check(v.Source == "global" && v.Effective == "60", "precedence: another game gets 60");
        Equal("From the settings for all games: 60 fps.", GraphicsSettings.OriginText(v, false), "origin: from all games");
        v = view("FrameRateLimit", null);
        Check(v.Source == "global" && v.Effective == "60" && GraphicsSettings.OriginText(v, false) == "Stored for all games.", "precedence: all games 60");
        v = view("Anisotropy", "witcher3.exe");
        Check(v.Source == "game-invalid" && v.Effective == null && v.Game.Raw == "7", "precedence: the game's 7 is not valid, its meaning is not known");
        Equal("The value stored for this game is not valid and is kept as it is.", GraphicsSettings.OriginText(v, false), "origin: invalid for the game, no guess at its meaning");
        v = view("PerformanceOverlay", "witcher3.exe");
        Check(v.Source == "global-invalid" && v.Global.Raw == "7", "precedence: an invalid value for all games");
        v = view("MemoryOverflow", "witcher3.exe");
        Check(v.Source == "global-invalid" && v.Effective == "allow", "precedence: an invalid Vulkan text means allow");
        Check(GraphicsSettings.OriginText(v, false).EndsWith("The driver uses: Use system memory (slower, keeps running).", StringComparison.Ordinal), "origin: what an invalid MemoryOverflow means");
        v = view("WsiRoute", "witcher3.exe");
        Check(v.Source == "game" && v.Effective == "dxgi-composition", "precedence: the game's dxgi-composition wins over gdi");
        v = view("WsiRoute", "other.exe");
        Check(v.Source == "global" && v.Effective == "gdi", "precedence: another game gets gdi");
        v = view("MaxFrameLatency", "witcher3.exe");
        Check(v.Source == "default" && v.Effective == null, "precedence: nothing stored, the application decides");
        Equal("Not set: Application decides.", GraphicsSettings.OriginText(v, false), "origin: not set");
        Equal("Changed, not applied yet", GraphicsSettings.OriginText(v, true), "origin: an edit");
        v = view("ReportAmdDriverVersion", "witcher3.exe");
        Check(v.Game.State == GfxState.Absent, "precedence: a setting for all games only has no game value");
        // A game's invalid value wins even over a valid one for all games (the readers stop at the first source).
        var k2 = new List<GfxKey> { GKey(GraphicsSettings.VulkanPath, 1, "WsiRoute", "dxgi"), GKey(W3V, 0, "WsiRoute", "bogus") };
        v = GraphicsSettings.View(AnySetting("WsiRoute"), k2, "witcher3.exe");
        Check(v.Source == "game-invalid" && v.Effective == "gdi", "precedence: the game's invalid route wins and means gdi");
        // An empty text for the game is absent: the value for all games applies.
        k2 = new List<GfxKey> { GKey(GraphicsSettings.VulkanPath, 1, "WsiRoute", "gdi"), GKey(W3V, 0, "WsiRoute", "") };
        v = GraphicsSettings.View(AnySetting("WsiRoute"), k2, "witcher3.exe");
        Check(v.Source == "global" && v.Effective == "gdi", "precedence: an empty text is absent");
        Check(GraphicsSettings.View(GraphicsSettings.Find("VSync"), null, "witcher3.exe").Source == "default", "precedence: no keys at all");

        // The report: stored values and effective values per scope, invalid ones named.
        var report = GraphicsSettings.Report(keys);
        foreach (var want in new[] { @"[HKLM\SOFTWARE\amdgpu-wddm\Graphics\Applications\witcher3.exe]", "FrameRateLimit = 144 (DWord)", "MemoryOverflow = \"nonsense\" (String)",
            "effective, all games:", "effective, witcher3.exe:", "FrameRateLimit = 144 (game)", "Anisotropy = not known (game-invalid, stored value not valid (7))",
            "PerformanceOverlay = not known (global-invalid, stored value for all games not valid (7))", "WsiRoute = \"dxgi-composition\" (String)", "MaxFrameLatency = application decides (default)" })
            Check(report.Contains(want), "report: " + want);
        Check(!report.Contains("WsiRoute = dxgi-composition (game)") && !report.Contains("  MemoryOverflow ="), "report: stored Vulkan values, but no effective line while they wait for their ICD");
        Check(GraphicsSettings.Report(null).Contains("unreadable"), "report: unreadable keys");
        Check(GraphicsSettings.Report(new List<GfxKey>()).Contains("no Graphics or Vulkan settings keys"), "report: no keys");
        CollectionEqual(new[] { "witcher3.exe" }, GraphicsSettings.Games(keys), "Games: the games with keys");
        Check(GraphicsSettings.HasOwn(keys, "witcher3.exe") && !GraphicsSettings.HasOwn(keys, "other.exe"), "HasOwn");
    }

    static void CollectionEqual(IEnumerable<string> expected, IEnumerable<string> actual, string what)
    {
        Equal(string.Join(",", expected), string.Join(",", actual), what);
    }

    // The choice list and the selection: a stored value is shown as it is and never rewritten by looking at it.
    static void GfxChoices()
    {
        var keys = new List<GfxKey> { GKey(GraphicsSettings.GraphicsPath, 1, "FrameRateLimit", 58, "PerformanceOverlay", 7), GKey(W3V, 0, "WsiRoute", "dxgi-composition") };
        var fps = GraphicsSettings.Find("FrameRateLimit");
        var all = GraphicsSettings.View(fps, keys, null);
        var items = GraphicsSettings.Items(all);
        Check(items[0].Absent && items[0].Text == "Application decides", "items: the first entry for all games is Application decides");
        Check(!items.Any(i => !i.Absent && i.Value == "0"), "items: for all games, No limit (the same as absent) is not offered unless stored");
        int at58 = items.FindIndex(i => i.Value == "58");
        Check(at58 > 0 && items[at58 - 1].Value == "50" && items[at58 + 1].Value == "60", "items: a stored 58 sits between 50 and 60");
        Equal(at58, GraphicsSettings.Selected(items, all.Here, false, null), "selected: the stored 58");
        var game = GraphicsSettings.View(fps, keys, "witcher3.exe");
        var gItems = GraphicsSettings.Items(game);
        Check(gItems[0].Absent && gItems[0].Text == "Same as all games: 58 fps", "items: a game starts with Same as all games, with the value");
        Check(gItems.Any(i => i.Value == "0" && i.Text == "No limit"), "items: a game can have No limit against a limit for all games");
        Check(!gItems.Any(i => i.Value == "58"), "items: the game list has the presets only");
        Equal(0, GraphicsSettings.Selected(gItems, game.Here, false, null), "selected: a game with nothing of its own");
        Equal(gItems.FindIndex(i => i.Value == "144"), GraphicsSettings.Selected(gItems, game.Here, true, "144"), "selected: the edit");
        Equal(0, GraphicsSettings.Selected(gItems, game.Here, true, null), "selected: an edit back to Same as all games");

        // Invalid for all games: shown as not valid, selected, and choosing it is no change.
        var overlay = GraphicsSettings.View(GraphicsSettings.Find("PerformanceOverlay"), keys, null);
        var oItems = GraphicsSettings.Items(overlay);
        var kept = oItems.Last();
        Check(kept.Kept && kept.Text == "Stored value not valid (7)", "items: the invalid 7 is shown");
        Equal(oItems.Count - 1, GraphicsSettings.Selected(oItems, overlay.Here, false, null), "selected: the invalid value");
        Check(oItems[0].Text == "Off" && oItems.Count(i => !i.Absent && !i.Kept) == 1 && oItems[1].Value == "1", "items: overlay for all games: Off (absent), On, not Off twice");
        var edits = new Dictionary<string, string>();
        GraphicsSettings.Choose(edits, overlay.Setting, overlay.Here, kept);
        Equal(0, edits.Count, "choose: the kept invalid value is no change");
        GraphicsSettings.Choose(edits, overlay.Setting, overlay.Here, oItems[0]);
        Check(edits.ContainsKey("PerformanceOverlay") && edits["PerformanceOverlay"] == null, "choose: Off removes the invalid value");
        GraphicsSettings.Choose(edits, overlay.Setting, overlay.Here, kept);
        Equal(0, edits.Count, "choose: back to the stored value drops the edit");
        GraphicsSettings.Choose(edits, fps, all.Here, items[at58]);
        Equal(0, edits.Count, "choose: the stored value is no change");
        GraphicsSettings.Choose(edits, fps, all.Here, items.First(i => i.Value == "60"));
        Equal("60", edits["FrameRateLimit"], "choose: 60");

        // Set outside this app (dxgi-composition): kept and shown as such.
        var route = GraphicsSettings.View(AnySetting("WsiRoute"), keys, "witcher3.exe");
        var rItems = GraphicsSettings.Items(route);
        Check(rItems.Last().Kept && rItems.Last().Text == "Set outside this app", "items: dxgi-composition is set outside this app");
        Check(rItems.Any(i => i.Value == "dxgi") && rItems.Any(i => i.Value == "gdi"), "items: a game can pick Modern or Compatibility");
        Equal(rItems.Count - 1, GraphicsSettings.Selected(rItems, route.Here, false, null), "selected: the value set outside");

        // The check box of ReportAmdDriverVersion.
        var amd = GraphicsSettings.Find("ReportAmdDriverVersion");
        var absent = new GfxRead { State = GfxState.Absent };
        var on = new GfxRead { State = GfxState.Valid, Value = "1" };
        var off = new GfxRead { State = GfxState.Valid, Value = "0" };
        var e = new Dictionary<string, string>();
        Check(!GraphicsSettings.Checked(absent, false, null) && GraphicsSettings.Checked(on, false, null) && !GraphicsSettings.Checked(off, false, null), "check box: stored state");
        GraphicsSettings.Check(e, amd, absent, true); Equal("1", e["ReportAmdDriverVersion"], "check box: on writes 1");
        GraphicsSettings.Check(e, amd, absent, false); Equal(0, e.Count, "check box: off again is no change");
        GraphicsSettings.Check(e, amd, on, false); Check(e.ContainsKey("ReportAmdDriverVersion") && e["ReportAmdDriverVersion"] == null, "check box: off removes a stored 1");
        GraphicsSettings.Check(e, amd, on, true); Equal(0, e.Count, "check box: on again is no change");
        GraphicsSettings.Check(e, amd, off, false); Equal(0, e.Count, "check box: a stored 0 stays when unchecked");
        Check(GraphicsSettings.Checked(off, true, "1") && !GraphicsSettings.Checked(on, true, null), "check box: an edit");
    }

    // The writes: nothing for no change, a removal for "Application decides", a game's empty key removed.
    static void GfxPlanning()
    {
        var keys = new List<GfxKey>
        {
            GKey(GraphicsSettings.GraphicsPath, 1, "FrameRateLimit", 60, "PerformanceOverlay", 7, "Unknown", 5),
            GKey(GraphicsSettings.VulkanPath, 1),
            GKey(W3G, 0, "FrameRateLimit", 144),
            GKey(W3V, 0, "WsiRoute", "GDI", "MemoryOverflow", "strict"),
            GKey(GraphicsSettings.AppPath(GraphicsSettings.GraphicsPath, "game.exe"), 1, "VSync", 1),
            GKey(GraphicsSettings.AppPath(GraphicsSettings.GraphicsPath, "cyber.exe"), 0, "VSync", 1, "Mine", 3),
        };
        Func<string, Dictionary<string, string>, string> plan = (image, edits) => WriteText(GraphicsSettings.PlanWrites(keys, image, edits));
        Equal("", plan(null, new Dictionary<string, string>()), "plan: no edits, no writes");
        Equal("", plan(null, new Dictionary<string, string> { { "FrameRateLimit", "60" } }), "plan: the stored value again, no write");
        Equal(@"set HKLM\SOFTWARE\amdgpu-wddm\Graphics FrameRateLimit = 30 (DWord)", plan(null, new Dictionary<string, string> { { "FrameRateLimit", "30" } }), "plan: 30 for all games");
        Equal(@"delete HKLM\SOFTWARE\amdgpu-wddm\Graphics FrameRateLimit", plan(null, new Dictionary<string, string> { { "FrameRateLimit", null } }), "plan: Application decides removes the value");
        Equal("", plan(null, new Dictionary<string, string> { { "VSync", null } }), "plan: Application decides on an absent value writes nothing");
        Equal(@"set HKLM\SOFTWARE\amdgpu-wddm\Graphics PerformanceOverlay = 1 (DWord)", plan(null, new Dictionary<string, string> { { "PerformanceOverlay", "1" } }), "plan: the invalid 7 is rewritten only on a choice");
        Equal(@"set HKLM\SOFTWARE\amdgpu-wddm\Graphics ReportAmdDriverVersion = 1 (DWord)", plan(null, new Dictionary<string, string> { { "ReportAmdDriverVersion", "1" } }), "plan: ReportAmdDriverVersion for all games");
        // The Vulkan names wait for their ICD (GraphicsSettings.AwaitingIcd): no write, for all games or for a game.
        Throws<ArgumentException>(() => GraphicsSettings.PlanWrites(keys, null, new Dictionary<string, string> { { "WsiRoute", "gdi" } }), "plan: a Vulkan name is refused while it waits");
        Throws<ArgumentException>(() => GraphicsSettings.PlanWrites(keys, "witcher3.exe", new Dictionary<string, string> { { "MemoryOverflow", null } }), "plan: a game's Vulkan value is refused while it waits");
        // A value of the other type is rewritten when chosen.
        var typed = new List<GfxKey> { GKey(GraphicsSettings.GraphicsPath, 0, "VSync", GfxValue.Str("1")) };
        Equal(@"set HKLM\SOFTWARE\amdgpu-wddm\Graphics VSync = 1 (DWord)", WriteText(GraphicsSettings.PlanWrites(typed, null, new Dictionary<string, string> { { "VSync", "1" } })), "plan: a text 1 becomes a DWORD 1 on a choice");
        Equal(@"delete HKLM\SOFTWARE\amdgpu-wddm\Graphics VSync", WriteText(GraphicsSettings.PlanWrites(typed, null, new Dictionary<string, string> { { "VSync", null } })), "plan: a value of the wrong type is removed on Application decides");
        // Per game: the last value removed takes the empty key with it; a key with a subkey or another value stays.
        Equal(@"delete HKLM\SOFTWARE\amdgpu-wddm\Graphics\Applications\witcher3.exe FrameRateLimit; delete key HKLM\SOFTWARE\amdgpu-wddm\Graphics\Applications\witcher3.exe (empty)",
            plan("witcher3.exe", new Dictionary<string, string> { { "FrameRateLimit", null } }), "plan: the game's last value and its key");
        Equal(@"delete HKLM\SOFTWARE\amdgpu-wddm\Graphics\Applications\game.exe VSync", plan("game.exe", new Dictionary<string, string> { { "VSync", null } }), "plan: a key with a subkey stays");
        Equal(@"delete HKLM\SOFTWARE\amdgpu-wddm\Graphics\Applications\cyber.exe VSync", plan("cyber.exe", new Dictionary<string, string> { { "VSync", null } }), "plan: a key with a value this app does not know stays");
        Equal(@"delete HKLM\SOFTWARE\amdgpu-wddm\Graphics\Applications\witcher3.exe FrameRateLimit; set HKLM\SOFTWARE\amdgpu-wddm\Graphics\Applications\witcher3.exe VSync = 0 (DWord)",
            plan("witcher3.exe", new Dictionary<string, string> { { "FrameRateLimit", null }, { "VSync", "0" } }), "plan: a value removed and one written keep the key");
        Equal(@"set HKLM\SOFTWARE\amdgpu-wddm\Graphics\Applications\new.exe RenderOnCpu = 1 (DWord)", plan("new.exe", new Dictionary<string, string> { { "RenderOnCpu", "1" } }), "plan: a new game key");
        Equal(@"set HKLM\SOFTWARE\amdgpu-wddm\Graphics\Applications\new.exe PerformanceOverlay = 0 (DWord)", plan("new.exe", new Dictionary<string, string> { { "PerformanceOverlay", "0" } }), "plan: Off for one game is written (it overrides On for all games)");
        Equal("", plan("new.exe", new Dictionary<string, string> { { "VSync", null } }), "plan: Same as all games on a game without a key writes nothing");
        Throws<ArgumentException>(() => GraphicsSettings.PlanWrites(keys, "witcher3.exe", new Dictionary<string, string> { { "ReportAmdDriverVersion", "1" } }), "plan: ReportAmdDriverVersion is not per game");
        Throws<ArgumentException>(() => GraphicsSettings.PlanWrites(keys, null, new Dictionary<string, string> { { "FrameRateLimit", "19" } }), "plan: 19 fps is refused");
        Throws<ArgumentException>(() => GraphicsSettings.PlanWrites(keys, null, new Dictionary<string, string> { { "Bogus", "1" } }), "plan: an unknown name is refused");
        Throws<ArgumentException>(() => GraphicsSettings.PlanWrites(keys, @"..\x.exe", new Dictionary<string, string> { { "VSync", "1" } }), "plan: a path is not a game");
        Throws<ArgumentException>(() => GraphicsSettings.PlanWrites(keys, null, new Dictionary<string, string> { { "WsiRoute", "dxgi-composition" } }), "plan: dxgi-composition is never written");
        Equal(@"set HKLM\SOFTWARE\amdgpu-wddm\Graphics FrameRateLimit = 30 (DWord)", WriteText(GraphicsSettings.PlanWrites(null, null, new Dictionary<string, string> { { "FrameRateLimit", "30" } })), "plan: no keys yet");
        Check(GraphicsSettings.PlanWrites(keys, "witcher3.exe", new Dictionary<string, string> { { "FrameRateLimit", null } }).All(GraphicsSettings.Owns), "plan: every write is one this app owns");

        // Restore defaults: the contract's values for all games, and with the games every game's values and empty keys.
        Equal(@"delete HKLM\SOFTWARE\amdgpu-wddm\Graphics FrameRateLimit; delete HKLM\SOFTWARE\amdgpu-wddm\Graphics PerformanceOverlay", WriteText(GraphicsSettings.ResetWrites(keys, false)),
            "reset: the values for all games; the unknown value stays");
        var reset = WriteText(GraphicsSettings.ResetWrites(keys, true));
        foreach (var want in new[] { @"delete HKLM\SOFTWARE\amdgpu-wddm\Graphics\Applications\witcher3.exe FrameRateLimit", @"delete key HKLM\SOFTWARE\amdgpu-wddm\Graphics\Applications\witcher3.exe (empty)",
            @"delete HKLM\SOFTWARE\amdgpu-wddm\Graphics\Applications\cyber.exe VSync" })
            Check(reset.Contains(want), "reset with games: " + want);
        Check(!reset.Contains(@"amdgpu-wddm\Vulkan"), "reset with games: the Vulkan values that wait for their ICD stay, and so does their key");
        Check(!reset.Contains(@"delete key HKLM\SOFTWARE\amdgpu-wddm\Graphics\Applications\game.exe") && !reset.Contains(@"delete key HKLM\SOFTWARE\amdgpu-wddm\Graphics\Applications\cyber.exe") &&
            !reset.Contains("Unknown") && !reset.Contains("Mine"), "reset with games: keys with a subkey or a foreign value stay, foreign values stay");
        Equal(0, GraphicsSettings.ResetWrites(null, true).Count, "reset: no keys, no writes");

        // The plain sentences of the confirmation, in every language and without internal names.
        foreach (var lang in Strings.Languages)
        {
            Strings.Language = lang;
            foreach (var w in GraphicsSettings.PlanWrites(keys, "witcher3.exe", new Dictionary<string, string> { { "FrameRateLimit", null } })
                .Concat(GraphicsSettings.PlanWrites(keys, null, new Dictionary<string, string> { { "FrameRateLimit", null }, { "VSync", "1" }, { "ReportAmdDriverVersion", "1" } })))
            {
                var line = GraphicsSettings.PlainLine(w);
                Check(!line.StartsWith("[", StringComparison.Ordinal) && !PlainWords.Findings(new[] { line }).Any(), "plain line " + lang + ": " + line);
                Equal(GraphicsSettings.LineId(w), PlainPlan.LineId(w), "plain line id " + lang + " " + w);
            }
        }
        Strings.Language = "en";
        Equal("All games, Frame rate limit: Application decides.", GraphicsSettings.PlainLine(RegWrite.Remove(GraphicsSettings.GraphicsPath, "FrameRateLimit")), "plain: all games back to Application decides");
        Equal("witcher3.exe, Frame rate limit: Same as all games.", GraphicsSettings.PlainLine(RegWrite.Remove(W3G, "FrameRateLimit")), "plain: a game back to Same as all games");
        Equal("witcher3.exe, Vertical sync (VSync): Always on.", GraphicsSettings.PlainLine(RegWrite.Dword(W3G, "VSync", 1)), "plain: a game's own value");
        Equal("witcher3.exe has no graphics settings of its own any more.", GraphicsSettings.PlainLine(RegWrite.RemoveKey(W3G)), "plain: the key");
        Equal("All games: FrameRateLimit = 30. witcher3.exe: VSync removed. Removes the empty key of witcher3.exe.",
            GraphicsSettings.Describe(new[] { RegWrite.Dword(GraphicsSettings.GraphicsPath, "FrameRateLimit", 30), RegWrite.Remove(W3G, "VSync"), RegWrite.RemoveKey(W3G) }), "describe: English for the log");
    }

    // The helper's --gfx argument: exact, one entry per setting, unset for a removal.
    static void GfxArguments()
    {
        var e = GraphicsSettings.ParseEdits("FrameRateLimit=60,Anisotropy=16,VSync=unset");
        Check(e != null && e.Count == 3 && e["FrameRateLimit"] == "60" && e["Anisotropy"] == "16" && e["VSync"] == null, "parse: three settings");
        Equal("FrameRateLimit=60,VSync=unset,Anisotropy=16", GraphicsSettings.FormatEdits(e), "format: in the contract's order");
        Equal("FrameRateLimit=60,VSync=unset,Anisotropy=16", GraphicsSettings.FormatEdits(GraphicsSettings.ParseEdits(GraphicsSettings.FormatEdits(e))), "format: round trip");
        // WsiRoute=gdi and MemoryOverflow=allow are in the contract but wait for their ICD (GraphicsSettings.AwaitingIcd).
        foreach (var bad in new[] { null, "", "FrameRateLimit", "FrameRateLimit=", "=60", "FrameRateLimit=19", "FrameRateLimit=060", "Bogus=1", "VSync=1,VSync=0", "WsiRoute=GDI",
            "WsiRoute=dxgi-composition", "WsiRoute=gdi", "MemoryOverflow=allow", "framerate limit=60", "VSync=1,", ",VSync=1", "VSync=1;Anisotropy=2", "FrameRateLimit=60 ", new string('x', 513) })
            Equal(null, GraphicsSettings.ParseEdits(bad), "parse refuses '" + (bad != null && bad.Length > 40 ? bad.Substring(0, 40) + "..." : bad) + "'");
        var every = string.Join(",", GraphicsSettings.All.Select(s => s.Name + "=" + s.Presets.Last()));
        Check(GraphicsSettings.ParseEdits(every) != null && every.Length <= 512, "parse: every setting at once fits");
        Equal("", GraphicsSettings.FormatEdits(null), "format: nothing");
    }

    // The Recovery plans that carry the settings.
    static void GfxPlans()
    {
        Func<RecoverySnapshot> snap = () =>
        {
            var s = WithDefaults();
            s.GfxKeys = new List<GfxKey> { GKey(GraphicsSettings.GraphicsPath, 1, "FrameRateLimit", 60), GKey(W3V, 0, "WsiRoute", "gdi") };
            return s;
        };
        // graphics-defaults
        var p = Recovery.Plan("graphics-defaults", snap(), more: new Recovery.PlanArgs { Gfx = "FrameRateLimit=90,VSync=1" });
        Check(!p.Refused && p.Undoable && !p.OfferRestart && p.Effect == "the next time a game starts", "graphics-defaults: undoable, no restart, at the next game start");
        Equal(@"set HKLM\SOFTWARE\amdgpu-wddm\Graphics FrameRateLimit = 90 (DWord); set HKLM\SOFTWARE\amdgpu-wddm\Graphics VSync = 1 (DWord)", WriteText(p.Writes), "graphics-defaults: the writes");
        Check(p.GameWrites.Count == 0 && p.Cu == null, "graphics-defaults: nothing else");
        var d = PlainPlan.Describe(p);
        Check(d.Title == "Settings for all games" && d.Notes[0] == Strings.T("plan.when.games") && !d.Restart, "graphics-defaults dialog: title and when");
        Check(d.Changes.SequenceEqual(new[] { "All games, Frame rate limit: 90 fps.", "All games, Vertical sync (VSync): Always on." }), "graphics-defaults dialog: " + string.Join(" | ", d.Changes));
        Check(Recovery.Plan("graphics-defaults", snap(), more: new Recovery.PlanArgs { Gfx = "FrameRateLimit=60" }).Refusal.Contains("stored already"), "graphics-defaults: the stored value is refused");
        Check(Recovery.Plan("graphics-defaults", snap(), more: new Recovery.PlanArgs { Gfx = "Bogus=1" }).Refused, "graphics-defaults: an invalid list is refused");
        Check(Recovery.Plan("graphics-defaults", snap(), more: new Recovery.PlanArgs()).Refused, "graphics-defaults: no list is refused");
        var unread = snap(); unread.GfxKeys = null;
        Check(Recovery.Plan("graphics-defaults", unread, more: new Recovery.PlanArgs { Gfx = "VSync=1" }).Refusal.Contains("cannot be read"), "graphics-defaults: unreadable keys are refused");
        var none = snap(); none.DriverInstalled = false;
        Check(Recovery.Plan("graphics-defaults", none, more: new Recovery.PlanArgs { Gfx = "VSync=1" }).Refused, "graphics-defaults: refused without the driver");

        // game-profile with graphics settings only, with switches only, and with both.
        var gp = snap();
        gp.GfxKeys.Add(GKey(W3G, 0, "VSync", 1));
        p = Recovery.Plan("game-profile", gp, more: new Recovery.PlanArgs { Image = "witcher3.exe", Gfx = "VSync=unset" });
        Check(!p.Refused && p.GameWrites.Count == 0 && p.GameImage == "witcher3.exe" && p.Effect == "the next time witcher3.exe starts", "game-profile gfx: a game change");
        Equal(@"delete HKLM\SOFTWARE\amdgpu-wddm\Graphics\Applications\witcher3.exe VSync; delete key HKLM\SOFTWARE\amdgpu-wddm\Graphics\Applications\witcher3.exe (empty)", WriteText(p.Writes), "game-profile gfx: value and key");
        d = PlainPlan.Describe(p);
        Check(d.Notes[0] == Strings.T("plan.when.game", "witcher3.exe") && d.Notes.Contains(Strings.T("plan.scope.game", "witcher3.exe")), "game-profile gfx dialog: when and scope");
        Check(Recovery.Plan("game-profile", gp, more: new Recovery.PlanArgs { Image = "witcher3.exe", Gfx = "VSync=1" }).Refusal.Contains("stored already"), "game-profile gfx: no change is refused");
        Check(Recovery.Plan("game-profile", snap(), more: new Recovery.PlanArgs { Image = "witcher3.exe", Gfx = "WsiRoute=unset" }).Refused, "game-profile gfx: a Vulkan name that waits for its ICD is refused");
        Check(Recovery.Plan("game-profile", snap(), more: new Recovery.PlanArgs { Image = "witcher3.exe" }).Refused, "game-profile: neither switches nor settings");
        Check(Recovery.Plan("game-profile", snap(), more: new Recovery.PlanArgs { Image = "witcher3.exe", Gfx = "ReportAmdDriverVersion=1" }).Refused, "game-profile: a setting for all games only is refused");
        var both = Recovery.Plan("game-profile", snap(), more: new Recovery.PlanArgs { Image = "witcher3.exe", Value = "raytracing-tier-off", Gfx = "FrameRateLimit=144" });
        Check(!both.Refused && both.GameWrites.Count == 0 && both.Writes.Count == 1 && both.Change.Contains("FrameRateLimit = 144"), "game-profile: switches stored already, a setting changes");
        both = Recovery.Plan("game-profile", snap(), more: new Recovery.PlanArgs { Image = "witcher3.exe", Value = "", Gfx = "FrameRateLimit=144" });
        Check(!both.Refused && both.GameWrites["witcher3.exe"] == "" && both.Writes.Count == 1 && both.Change.StartsWith("Removes the settings of witcher3.exe.", StringComparison.Ordinal), "game-profile: switches removed and a setting");
        var unreadGame = snap(); unreadGame.GfxKeys = null;
        Check(Recovery.Plan("game-profile", unreadGame, more: new Recovery.PlanArgs { Image = "witcher3.exe", Value = "", Gfx = "VSync=1" }).Refused, "game-profile: unreadable keys refuse the whole change");
        Check(!Recovery.Plan("game-profile", unreadGame, more: new Recovery.PlanArgs { Image = "witcher3.exe", Value = "" }).Refused, "game-profile: switches alone do not need the keys");

        // game-undo puts the game's values back and removes a key the undo leaves empty.
        var rec = Rec("g1", "2026-10-08T10:00:00Z", "game-profile", "witcher3.exe");
        rec.Values = new List<BackupValue>
        {
            new BackupValue { Path = W3G, Name = "FrameRateLimit", Existed = false },
        };
        var after = snap();
        after.GfxKeys.Add(GKey(W3G, 0, "FrameRateLimit", 144));
        p = Recovery.Plan("game-undo", after, backups: new[] { rec }, more: new Recovery.PlanArgs { Image = "witcher3.exe" });
        Check(!p.Refused && p.GameWrites.Count == 0 && p.Effect == "the next time witcher3.exe starts", "game-undo gfx: a game change");
        Equal(@"delete HKLM\SOFTWARE\amdgpu-wddm\Graphics\Applications\witcher3.exe FrameRateLimit; delete key HKLM\SOFTWARE\amdgpu-wddm\Graphics\Applications\witcher3.exe (empty)",
            WriteText(p.Writes), "game-undo gfx: the old value and the empty key");
        rec.Values.Add(new BackupValue { Path = W3V, Name = "WsiRoute", Existed = true, Kind = "String", Text = "dxgi" });
        Check(Recovery.Plan("game-undo", after, backups: new[] { rec }, more: new Recovery.PlanArgs { Image = "witcher3.exe" }).Refused, "game-undo: a Vulkan value that waits for its ICD is refused");
        rec.Values.RemoveAt(1);
        rec.Values.Add(new BackupValue { Path = W3G, Name = "NotOurs", Existed = false });
        Check(Recovery.Plan("game-undo", after, backups: new[] { rec }, more: new Recovery.PlanArgs { Image = "witcher3.exe" }).Refused, "game-undo: a value outside the contract is refused");
        rec.Values.RemoveAt(1);
        rec.Values.Add(new BackupValue { Path = GraphicsSettings.GraphicsPath, Name = "FrameRateLimit", Existed = true, Kind = "DWord", Number = 30 });
        p = Recovery.Plan("game-undo", after, backups: new[] { rec }, more: new Recovery.PlanArgs { Image = "witcher3.exe" });
        Check(!p.Writes.Any(w => w.Path == GraphicsSettings.GraphicsPath), "game-undo: the setting for all games is not this game's");

        // The general undo of graphics-defaults: no restart.
        var gd = new BackupRecord { File = "a1", Utc = "2026-10-08T11:00:00Z", Action = "graphics-defaults", Args = new[] { "gfx=FrameRateLimit=90" }, Undoable = true,
            Values = new List<BackupValue> { new BackupValue { Path = GraphicsSettings.GraphicsPath, Name = "FrameRateLimit", Existed = true, Kind = "DWord", Number = 60 } } };
        p = Recovery.Plan("undo", snap(), backups: new[] { gd });
        Check(!p.Refused && !p.OfferRestart && p.Effect == "the next time a game starts", "undo of graphics-defaults: no restart");
        Equal(@"set HKLM\SOFTWARE\amdgpu-wddm\Graphics FrameRateLimit = 60 (DWord)", WriteText(p.Writes), "undo of graphics-defaults: the old value");
        Check(PlainPlan.Describe(p).Notes[0] == Strings.T("plan.when.games"), "undo of graphics-defaults dialog: at the next game start");

        // reset-defaults: the settings for all games always, the games' own with the games.
        var rs = snap();
        rs.GfxKeys.Add(GKey(W3G, 0, "VSync", 1));
        p = Recovery.Plan("reset-defaults", rs, more: new Recovery.PlanArgs { Games = "keep" });
        Check(!p.Refused && p.Writes.Any(w => w.Path == GraphicsSettings.GraphicsPath && w.Name == "FrameRateLimit" && w.Delete) && !p.Writes.Any(w => GraphicsSettings.AppImage(w.Path) != null),
            "reset-defaults keep: all games back to Application decides, the games' own kept");
        Check(p.Change.Contains("graphics settings for games: 1 changes"), "reset-defaults: the change counts them: " + p.Change);
        p = Recovery.Plan("reset-defaults", rs, more: new Recovery.PlanArgs { Games = "reset" });
        var rt = WriteText(p.Writes);
        Check(rt.Contains(@"delete HKLM\SOFTWARE\amdgpu-wddm\Graphics\Applications\witcher3.exe VSync") && rt.Contains(@"delete key HKLM\SOFTWARE\amdgpu-wddm\Graphics\Applications\witcher3.exe (empty)")
            && !rt.Contains(@"amdgpu-wddm\Vulkan"), "reset-defaults reset: the games' own values and keys; the Vulkan values that wait for their ICD stay");
        foreach (var w in p.Writes) Check(PlainPlan.LineId(w) != null, "reset-defaults: a plain sentence for " + w);
        var rsUnread = snap(); rsUnread.GfxKeys = null;
        p = Recovery.Plan("reset-defaults", rsUnread, more: new Recovery.PlanArgs { Games = "keep" });
        Check(p.Notes.Any(n => n.Contains("graphics settings for games cannot be read")), "reset-defaults: unreadable keys are left with a note");
        Check(Recovery.Actions.Contains("graphics-defaults"), "the action list has graphics-defaults");
    }

    // The shader model choice: one of three, stored as two switches of the game's list.
    static void GfxShaderModel()
    {
        Equal("6.8", ShaderModelCeiling.State(null), "sm: nothing stored is 6.8");
        Equal("6.8", ShaderModelCeiling.State("raytracing-tier-off"), "sm: other switches are 6.8");
        Equal("6.7", ShaderModelCeiling.State("shader-model-68-off"), "sm: 68-off is 6.7");
        Equal("6.6", ShaderModelCeiling.State("shader-model-67-off"), "sm: 67-off is 6.6");
        Equal("6.6", ShaderModelCeiling.State("shader-model-68-off,shader-model-67-off"), "sm: 67-off wins over 68-off");
        foreach (var start in new[] { null, "raytracing-tier-off", "shader-model-68-off", "shader-model-67-off,raytracing-tier-off", "shader-model-68-off,shader-model-67-off" })
            foreach (var c in ShaderModelCeiling.Choices)
            {
                var names = GameGroups.NamesOf(start);
                ShaderModelCeiling.Apply(names, c);
                Equal(c, ShaderModelCeiling.State(string.Join(",", names)), "sm: " + (start ?? "nothing") + " to " + c);
                Equal(start != null && start.Contains("raytracing-tier-off"), names.Contains("raytracing-tier-off"), "sm: other switches stay (" + (start ?? "nothing") + " to " + c + ")");
                Check(names.Count(n => n.StartsWith("shader-model-", StringComparison.Ordinal)) == (c == "6.8" ? 0 : 1), "sm: one switch at most");
            }
        Throws<ArgumentException>(() => ShaderModelCeiling.Apply(new List<string>(), "6.5"), "sm: 6.5 is not a choice");
        Equal(ValueOrigin.DriverDefault, ShaderModelCeiling.Origin(null, null), "sm origin: nothing stored");
        Equal(ValueOrigin.ThisGame, ShaderModelCeiling.Origin("shader-model-68-off", null), "sm origin: this game");
        Equal(ValueOrigin.Recommended, ShaderModelCeiling.Origin("shader-model-68-off", "shader-model-68-off"), "sm origin: recommended");
        Equal(ValueOrigin.DriverDefault, ShaderModelCeiling.Origin("raytracing-tier-off", "raytracing-tier-off"), "sm origin: 6.8 is the default");
        foreach (var lang in Strings.Languages)
            foreach (var c in ShaderModelCeiling.Choices)
            {
                Strings.Language = lang;
                Check(!ShaderModelCeiling.Label(c).StartsWith("[", StringComparison.Ordinal), "sm label " + lang + " " + c);
            }
        Strings.Language = "en";
        Equal("6.8 (default)", ShaderModelCeiling.Label("6.8"), "sm label: 6.8 is the default");

        // GameGroups.Plan with the choice: the stored list, the groups' changes and the shader model together.
        var w = GameGroups.Plan("witcher3.exe", "raytracing-tier-off", new Dictionary<string, bool>(), "6.6");
        Check(w.Kind == ProfileWriteKind.Set && Profiles.Parse(w.Value).Known.Contains("shader-model-67-off") && w.Value.Contains("raytracing-tier-off"), "GameGroups.Plan: 6.6 added to the list: " + w.Value);
        w = GameGroups.Plan("witcher3.exe", "shader-model-68-off", new Dictionary<string, bool>(), "6.8");
        Equal(ProfileWriteKind.Delete, w.Kind, "GameGroups.Plan: back to 6.8 removes the last switch");
        w = GameGroups.Plan("witcher3.exe", "shader-model-68-off", new Dictionary<string, bool>(), "6.7");
        Equal(ProfileWriteKind.None, w.Kind, "GameGroups.Plan: the stored choice is no change");
        w = GameGroups.Plan("witcher3.exe", "shader-model-68-off", new Dictionary<string, bool>(), null);
        Equal(ProfileWriteKind.None, w.Kind, "GameGroups.Plan: no choice keeps the switch");
        Check(Profiles.IsValidValue("shader-model-68-off,shader-model-67-off"), "the shader model switches are in the catalog");
        var d = PlainPlan.Describe(Recovery.Plan("game-profile", WithDefaults(), more: new Recovery.PlanArgs { Image = "game.exe", Value = "deferred-replay-off,shader-model-68-off" }));
        Check(d.Changes.Any(c => c.Contains(Strings.T("plan.game.sm", "6.7"))), "dialog: the shader model is named: " + string.Join(" | ", d.Changes));
        Check(!d.Changes.Any(c => PlainWords.Findings(new[] { c }).Any()), "dialog: no switch names");
    }

    // The Display page's pure helpers.
    static void GfxDisplay()
    {
        var raw = new[]
        {
            new DisplayChoice { Width = 1920, Height = 1080, RefreshHz = 60 }, new DisplayChoice { Width = 1920, Height = 1080, RefreshHz = 144 },
            new DisplayChoice { Width = 1920, Height = 1080, RefreshHz = 60 }, new DisplayChoice { Width = 1280, Height = 720, RefreshHz = 60 },
            new DisplayChoice { Width = 2560, Height = 1440, RefreshHz = 120 }, new DisplayChoice { Width = 0, Height = 0, RefreshHz = 60 },
            new DisplayChoice { Width = 1920, Height = 1200, RefreshHz = 60 },
        };
        var modes = DisplayInfo.Distinct(raw);
        Equal(5, modes.Count, "display: duplicates and empty modes go");
        Equal("2560x1440@120,1920x1200@60,1920x1080@144,1920x1080@60,1280x720@60", string.Join(",", modes.Select(m => m.Width + "x" + m.Height + "@" + m.RefreshHz)), "display: largest first");
        Equal("2560x1440,1920x1200,1920x1080,1280x720", string.Join(",", DisplayInfo.Resolutions(modes).Select(s => s.Width + "x" + s.Height)), "display: resolutions");
        Equal("144,60", string.Join(",", DisplayInfo.Rates(modes, 1920, 1080)), "display: rates of 1920x1080");
        Equal(0, DisplayInfo.Rates(modes, 800, 600).Count, "display: an unlisted resolution has no rates");
        Equal(60, DisplayInfo.RateFor(modes, 1920, 1080, 60), "display: the rate in use stays when the resolution has it");
        Equal(120, DisplayInfo.RateFor(modes, 2560, 1440, 60), "display: else the highest rate of the new resolution");
        Equal(75, DisplayInfo.RateFor(modes, 800, 600, 75), "display: no rates, the rate asked for");
        Check(DisplayInfo.Listed(modes, 1920, 1080, 144) && !DisplayInfo.Listed(modes, 1920, 1080, 120), "display: Listed");
        Equal("full", DisplayInfo.ScalingOf(DisplayInfo.ScalingStretched), "scaling: stretched is full");
        Equal("aspect", DisplayInfo.ScalingOf(DisplayInfo.ScalingAspect), "scaling: aspect-ratio-centred-max keeps the aspect");
        Equal("center", DisplayInfo.ScalingOf(DisplayInfo.ScalingCentered), "scaling: centred");
        foreach (var w in new[] { DisplayInfo.ScalingIdentity, DisplayInfo.ScalingCustom, DisplayInfo.ScalingPreferred, 77u })
            Equal("windows", DisplayInfo.ScalingOf(w), "scaling: " + w + " is as Windows set it");
        Equal(null, DisplayInfo.ScalingOf(0), "scaling: 0 cannot be read");
        foreach (var c in DisplayInfo.ScalingChoices)
        {
            Equal(c, DisplayInfo.ScalingOf(DisplayInfo.ScalingValue(c)), "scaling: round trip " + c);
            Check(Strings.Has("display.scaling." + c), "scaling: " + c + " has a text");
        }
        Throws<ArgumentException>(() => DisplayInfo.ScalingValue("windows"), "scaling: as Windows set it is never written");
        Equal(15, DisplayInfo.KeepSeconds, "display: 15 s to keep a change, as in Windows");
        Check(Strings.T("display.keep.text", DisplayInfo.KeepSeconds).Contains("15"), "display: the countdown text");
    }
}
