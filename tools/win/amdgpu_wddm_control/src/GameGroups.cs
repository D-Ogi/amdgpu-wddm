// Per-game compatibility settings in plain groups (WU-028) over the existing D3D12 switches (F-PROF), with the origin
// of each value (WU-015: driver default, recommended by the installer, this game). A group the user does not touch
// keeps its exact tokens, partial groups included, and names the app does not know are always kept (A6, G-PROF): an
// edit that changes nothing writes nothing. The diagnostic switches and unknown names are shown only with the
// support options; they are never dropped.
using System;
using System.Collections.Generic;
using System.Linq;

namespace AmdgpuWddmControl
{
    public enum GroupState { Off, On, Partial }
    public enum ValueOrigin { DriverDefault, Recommended, ThisGame }

    public sealed class GameGroup
    {
        public string Id;
        public string[] Tokens;
        public bool SupportOnly;
        // An Invert group names the off switches of behaviours the driver does by default (Profiles.cs): the box
        // is checked while the value names none of them, and turning it off writes them all. The group's title
        // keeps naming the feature, so what the user reads does not change with the name of the switch.
        public bool Invert;
        public string Title { get { return Strings.T("game.group." + Id); } }
        public string Explain { get { return Strings.T("game.group." + Id + ".explain"); } }
        public string Cost { get { return Strings.T("game.group." + Id + ".cost"); } }
    }

    public static class GameGroups
    {
        public static readonly GameGroup[] All =
        {
            new GameGroup { Id = "rt", Tokens = new[] { "raytracing-tier-off" }, Invert = true },
            new GameGroup { Id = "present", Tokens = new[] { "present-noprimary", "present-cached" } },
            new GameGroup { Id = "cpu", Tokens = new[] { "recording-bind-off", "retire-handoff-off", "deferred-replay-off", "direct-entry-off" }, Invert = true },
            new GameGroup { Id = "diag", Tokens = new[] { "release-two-phase-off", "import-progress-gate-off", "import-quarantine-off", "shared-create-retry-off", "fence-wait-accept-off", "replay-log" }, SupportOnly = true },
        };

        public static GameGroup Find(string id) { return All.FirstOrDefault(g => g.Id == id); }

        static List<string> Names(string value) { var v = Profiles.Parse(value); return v.Known.Concat(v.Unknown).ToList(); }

        public static GroupState State(GameGroup g, string value)
        {
            var names = new HashSet<string>(Names(value));
            int n = g.Tokens.Count(names.Contains);
            if (g.Invert) return n == 0 ? GroupState.On : n == g.Tokens.Length ? GroupState.Off : GroupState.Partial;
            return n == 0 ? GroupState.Off : n == g.Tokens.Length ? GroupState.On : GroupState.Partial;
        }

        // The state a game with no value of its own is in: Off for a group of opt-in names, On for an Invert group,
        // whose names subtract from what the driver already does.
        public static GroupState Default(GameGroup g) { return g.Invert ? GroupState.On : GroupState.Off; }

        // stored: the game's stored value, null when the game has no key; recommended: the release's value for this
        // file name (manifest defaults.d3d12_applications), null when none.
        public static ValueOrigin Origin(GameGroup g, string stored, string recommended)
        {
            if (stored == null) return ValueOrigin.DriverDefault;
            var def = Default(g);
            var now = State(g, stored);
            if (recommended != null && State(g, recommended) != def && now == State(g, recommended)) return ValueOrigin.Recommended;
            return now == def ? ValueOrigin.DriverDefault : ValueOrigin.ThisGame;
        }

        public static string OriginText(ValueOrigin o)
        {
            return Strings.T(o == ValueOrigin.DriverDefault ? "game.origin.driver" : o == ValueOrigin.Recommended ? "game.origin.recommended" : "game.origin.game");
        }

        // The names after the user's changes: untouched groups and unknown names exactly as stored, a changed group
        // fully on or fully off.
        public static List<string> Apply(string stored, IDictionary<string, bool> changes)
        {
            var names = Names(stored);
            foreach (var c in changes ?? new Dictionary<string, bool>())
            {
                var g = Find(c.Key);
                if (g == null) throw new ArgumentException("unknown group " + c.Key);
                names.RemoveAll(n => g.Tokens.Contains(n));
                if (c.Value != g.Invert) names.AddRange(g.Tokens);
            }
            return names;
        }

        // The write of an edit, or None when it changes nothing (the settings rule: no names = remove the key).
        // shaderModel: the chosen ceiling ("6.8", "6.7", "6.6"), null when the person did not change it.
        public static ProfileWrite Plan(string image, string stored, IDictionary<string, bool> changes, string shaderModel = null)
        {
            var names = Apply(stored, changes);
            if (shaderModel != null) ShaderModelCeiling.Apply(names, shaderModel);
            return Profiles.PlanWrite(image, stored, names);
        }

        internal static List<string> NamesOf(string value) { return Names(value); }

        // The names that only the support view shows: the diagnostic group and names outside the catalog.
        public static List<string> Hidden(string stored)
        {
            var v = Profiles.Parse(stored);
            return v.Known.Where(n => All.Any(g => g.SupportOnly && g.Tokens.Contains(n))).Concat(v.Unknown).ToList();
        }
    }

    // The highest shader model the adapter reports to one game (driver/umd/d3d12/adapter-caps.cpp of branch
    // m15/shader-model-68): 6.8 by default, 6.7 with shader-model-68-off, 6.6 with shader-model-67-off, which wins
    // when both are named. One choice of three, for a game that fails with a newer shader model.
    public static class ShaderModelCeiling
    {
        public const string Off68 = "shader-model-68-off", Off67 = "shader-model-67-off", Default = "6.8";
        public static readonly string[] Choices = { "6.8", "6.7", "6.6" };

        public static string State(string value)
        {
            var names = GameGroups.NamesOf(value);
            return names.Contains(Off67) ? "6.6" : names.Contains(Off68) ? "6.7" : Default;
        }

        // The names with the choice applied: both switches removed, then the one the choice needs added. Every other
        // name stays as it is.
        public static void Apply(List<string> names, string choice)
        {
            if (!Choices.Contains(choice)) throw new ArgumentException("not a shader model choice: " + choice);
            names.RemoveAll(n => n == Off68 || n == Off67);
            if (choice == "6.7") names.Add(Off68);
            else if (choice == "6.6") names.Add(Off67);
        }

        public static ValueOrigin Origin(string stored, string recommended)
        {
            if (stored == null) return ValueOrigin.DriverDefault;
            var now = State(stored);
            if (recommended != null && State(recommended) != Default && now == State(recommended)) return ValueOrigin.Recommended;
            return now == Default ? ValueOrigin.DriverDefault : ValueOrigin.ThisGame;
        }

        public static string Label(string choice) { return Strings.T("games.sm." + choice.Replace(".", "")); }
    }
}
