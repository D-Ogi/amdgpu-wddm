// Settings search (WU-009, WU-071): a setting by its name or a common term, in the window's language and in English,
// and a jump to it. The text is normalized before comparing: NFKC (full-width and half-width forms agree), the
// invariant lower case, hiragana as katakana, and no spaces or hyphens ("V-Sync" = "vsync"). Terms for features the
// driver cannot do yet ("HDR", "sharpening") lead to their "Coming later" row; they do not count as the feature.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;

namespace AmdgpuWddmControl
{
    public sealed class SearchEntry
    {
        public string Id, Page;
        public bool ComingLater;
        public string Name { get { return Strings.T("search." + Id); } }
    }

    public sealed class SearchHit
    {
        public SearchEntry Entry;
        public int Score;          // 0 name starts with the query, 1 name contains it, 2 a term matches
    }

    public static class SettingsSearch
    {
        public static readonly SearchEntry[] Index =
        {
            E("graphics.clock-auto", "graphics"), E("graphics.clock-ceiling", "graphics"), E("graphics.cores", "graphics"), E("graphics.reset", "graphics"),
            E("graphics.tuning", "graphics"), E("graphics.cpu-tuning", "graphics"), E("graphics.cpu-cores", "graphics"),
            // The settings for all games (GraphicsSettings.cs); a game's own ones are on the Games page.
            E("graphics.fps", "graphics"), E("graphics.vsync", "graphics"), E("graphics.af", "graphics"), E("graphics.latency", "graphics"),
            // graphics.vk-present and graphics.vk-memory come back with their rows (GraphicsSettings.AwaitingIcd).
            E("graphics.overlay", "graphics"), E("graphics.amd-version", "graphics"),
            E("games.rt", "games"), E("games.present", "games"), E("games.cpu", "games"), E("games.sm", "games"), E("games.cpu-render", "games"),
            E("games.undo", "games"), E("games.add", "games"),
            E("display.resolution", "display"), E("display.scaling", "display"), E("display.identify", "display"), E("display.scale", "display"),
            E("perf.sensors", "performance"), E("perf.fan", "performance"), E("perf.cache", "performance"),
            E("driver.version", "driver"), E("driver.update", "driver"),
            E("settings.language", "settings"), E("settings.update-start", "settings"), E("settings.recent", "settings"), E("settings.nagi", "settings"),
            E("settings.tips", "settings"), E("settings.animations", "settings"), E("settings.support", "settings"), E("settings.data", "settings"),
            E("help.repair", "help"), E("help.report", "help"), E("help.restart", "help"), E("help.guides", "help"),
            L("later.sharpen", "graphics"), L("later.aa", "graphics"),
            L("later.hdr", "display"), L("later.vrr", "display"),
            L("later.power", "performance"), L("later.cache-clear", "performance"),
        };

        static SearchEntry E(string id, string page) { return new SearchEntry { Id = id, Page = page }; }
        static SearchEntry L(string id, string page) { return new SearchEntry { Id = id, Page = page, ComingLater = true }; }

        public static string Normalize(string text)
        {
            var s = (text ?? "").Normalize(NormalizationForm.FormKC).ToLowerInvariant();
            var w = new StringBuilder(s.Length);
            foreach (var c in s)
            {
                if (char.IsWhiteSpace(c) || c == '-' || c == '_' || c == '・' || c == '·') continue;
                // Hiragana U+3041-U+3096 -> katakana U+30A1-U+30F6.
                w.Append(c >= 'ぁ' && c <= 'ゖ' ? (char)(c + 0x60) : c);
            }
            return w.ToString();
        }

        static IEnumerable<string> Terms(string language, string id)
        {
            var t = Strings.In(language, "search." + id + ".terms");
            return t.StartsWith("[", StringComparison.Ordinal) ? Enumerable.Empty<string>() : t.Split('|');
        }

        public static List<SearchHit> Find(string query, string language)
        {
            var q = Normalize(query);
            var hits = new List<SearchHit>();
            if (q.Length == 0) return hits;
            foreach (var e in Index)
            {
                int score = int.MaxValue;
                foreach (var lang in new[] { language, "en" }.Distinct())
                {
                    var name = Normalize(Strings.In(lang, "search." + e.Id));
                    if (name.StartsWith(q, StringComparison.Ordinal)) score = Math.Min(score, 0);
                    else if (name.Contains(q)) score = Math.Min(score, 1);
                    else
                    {
                        // A term that contains the whole query beats a query that only contains a term ("FPS limit"
                        // is the frame-rate limit, not every setting with "limit" among its terms).
                        var terms = Terms(lang, e.Id).Select(Normalize).Where(t => t.Length > 0).ToList();
                        if (terms.Any(t => t.Contains(q))) score = Math.Min(score, 2);
                        else if (q.Length >= 3 && terms.Any(t => q.Contains(t))) score = Math.Min(score, 3);
                    }
                }
                if (score != int.MaxValue) hits.Add(new SearchHit { Entry = e, Score = score });
            }
            return hits.OrderBy(h => h.Score).ThenBy(h => Array.IndexOf(Index, h.Entry)).ToList();
        }
    }
}
