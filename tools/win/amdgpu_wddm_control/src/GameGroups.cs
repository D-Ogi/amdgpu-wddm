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
        public string Title { get { return Strings.T("game.group." + Id); } }
        public string Explain { get { return Strings.T("game.group." + Id + ".explain"); } }
        public string Cost { get { return Strings.T("game.group." + Id + ".cost"); } }
    }

    public static class GameGroups
    {
        public static readonly GameGroup[] All =
        {
            new GameGroup { Id = "rt", Tokens = new[] { "raytracing-tier" } },
            new GameGroup { Id = "present", Tokens = new[] { "present-noprimary", "present-cached" } },
            new GameGroup { Id = "cpu", Tokens = new[] { "recording-bind", "retire-handoff", "deferred-replay" } },
            new GameGroup { Id = "diag", Tokens = new[] { "release-two-phase-off", "import-progress-gate-off", "import-quarantine-off" }, SupportOnly = true },
        };

        public static GameGroup Find(string id) { return All.FirstOrDefault(g => g.Id == id); }

        static List<string> Names(string value) { var v = Profiles.Parse(value); return v.Known.Concat(v.Unknown).ToList(); }

        public static GroupState State(GameGroup g, string value)
        {
            var names = new HashSet<string>(Names(value));
            int n = g.Tokens.Count(names.Contains);
            return n == 0 ? GroupState.Off : n == g.Tokens.Length ? GroupState.On : GroupState.Partial;
        }

        // stored: the game's stored value, null when the game has no key; recommended: the release's value for this
        // file name (manifest defaults.d3d12_applications), null when none.
        public static ValueOrigin Origin(GameGroup g, string stored, string recommended)
        {
            if (stored == null) return ValueOrigin.DriverDefault;
            var now = State(g, stored);
            if (recommended != null && State(g, recommended) != GroupState.Off && now == State(g, recommended)) return ValueOrigin.Recommended;
            return now == GroupState.Off ? ValueOrigin.DriverDefault : ValueOrigin.ThisGame;
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
                if (c.Value) names.AddRange(g.Tokens);
            }
            return names;
        }

        // The write of an edit, or None when it changes nothing (the settings rule: no names = remove the key).
        public static ProfileWrite Plan(string image, string stored, IDictionary<string, bool> changes)
        {
            return Profiles.PlanWrite(image, stored, Apply(stored, changes));
        }

        // The names that only the support view shows: the diagnostic group and names outside the catalog.
        public static List<string> Hidden(string stored)
        {
            var v = Profiles.Parse(stored);
            return v.Known.Where(n => All.Any(g => g.SupportOnly && g.Tokens.Contains(n))).Concat(v.Unknown).ToList();
        }
    }
}
