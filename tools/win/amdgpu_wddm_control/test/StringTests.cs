// G-STR: the string tables (ids, statuses, placeholders, source-hash freshness per A6) and the loader's fallbacks.
using System;
using System.Linq;
using AmdgpuWddmControl;

static partial class UnitTests
{
    static void StringTables()
    {
        var problems = Strings.Validate(Strings.Tables);
        foreach (var p in problems.Take(40)) Console.WriteLine("G-STR: " + p);
        Equal(0, problems.Count, "G-STR: string tables complete, fresh and consistent");
        Check(Strings.Tables["en"].Entries.Count > 0, "G-STR: English table loaded");

        // A6 by construction: a stale translation is refused by the gate whatever its review status, and the window
        // shows English for it.
        var en = Strings.Parse("en", "a.b|src|-|Hello {0}\nc.d|src|-|Two\n");
        var pl = Strings.Parse("pl", "a.b|rev|" + Strings.Hash8("Hello there {0}") + "|Czesc {0}\nc.d|mt|" + Strings.Hash8("Two") + "|Dwa {1}\n");
        var t = new System.Collections.Generic.Dictionary<string, StringTable> { { "en", en }, { "pl", pl } };
        var found = Strings.Validate(t);
        Check(found.Any(x => x.Contains("a.b is stale")), "G-STR: a reviewed but stale translation fails");
        Check(found.Any(x => x.Contains("c.d placeholders")), "G-STR: placeholder mismatch fails");
        Check(found.Any(x => x.StartsWith("ja: table missing")), "G-STR: a missing language fails");
        var dup = Strings.Parse("en", "a.b|src|-|x\na.b|src|-|y\nbad id|src|-|z\nno pipes\n");
        Equal(3, dup.Errors.Count, "G-STR: duplicate id, bad id and a malformed line are errors");
        Equal("{0}", "{" + Strings.Placeholders("a {0} b {0:N0}") + "}", "placeholders are a set");
        Equal("[no.such-id]", Strings.T("no.such-id"), "an unknown id shows as [id]");

        // Fallback to English for a stale translation, translation when fresh.
        var saved = Strings.Language;
        foreach (var lang in Strings.Languages)
        {
            Strings.Language = lang;
            Check(Strings.T("cu.run.standard", 24).Contains("24"), "G-STR: " + lang + " formats its argument");
        }
        Strings.Language = "xx";
        Equal("en", Strings.Language, "an unknown language falls back to English");
        Strings.Language = saved;
        // G-LANG: every language of the list shows its own name in its own script under every current language, the
        // label of the choice keeps the English word, and Windows' language maps every pt-* and es-* culture.
        var endonyms = new[] { "English", "Polski", "Português (Brasil)", "Español", "日本語", "한국어" };
        var languageBefore = Strings.Language;
        foreach (var ui in Strings.Languages)
        {
            Strings.Language = ui;
            Check(Strings.Languages.Select(Strings.Endonym).SequenceEqual(endonyms), "G-LANG: under " + ui + " every language shows its own name");
            var label = Strings.LanguageLabel();
            Check(label.Contains("Language") && (ui == "en" || label.Contains(" / ")), "G-LANG: under " + ui + " the language label keeps the English word (" + label + ")");
        }
        Strings.Language = languageBefore;
        var cultureBefore = System.Threading.Thread.CurrentThread.CurrentUICulture;
        try
        {
            foreach (var pair in new[] { "pt-BR:pt", "pt-PT:pt", "es-ES:es", "es-MX:es", "es-419:es", "es-AR:es", "pl-PL:pl", "ja-JP:ja", "ko-KR:ko", "en-GB:en", "de-DE:en", "fr-FR:en" })
            {
                var p = pair.Split(':');
                System.Threading.Thread.CurrentThread.CurrentUICulture = System.Globalization.CultureInfo.GetCultureInfo(p[0]);
                Equal(p[1], Strings.SystemLanguage(), "G-LANG: Windows in " + p[0] + " opens in " + p[1]);
            }
        }
        finally { System.Threading.Thread.CurrentThread.CurrentUICulture = cultureBefore; }
        // Every listed language has a table that holds every English id, and the gate fails a listed language whose
        // table is missing or lacks an id.
        foreach (var lang in Strings.Languages)
            Check(Strings.Tables.ContainsKey(lang) && Strings.Tables[lang].Errors.Count == 0 && Strings.Tables["en"].Entries.Keys.All(Strings.Tables[lang].Entries.ContainsKey),
                "G-STR: " + lang + " has a table with every English id");
        var probeEn = Strings.Parse("en", "a.b|src|-|One\nc.d|src|-|Two\n");
        var probe = new System.Collections.Generic.Dictionary<string, StringTable> { { "en", probeEn } };
        foreach (var lang in Strings.Languages.Where(l => l != "en" && l != "pt"))
            probe[lang] = Strings.Parse(lang, "a.b|mt|" + Strings.Hash8("One") + "|1\n" + (lang == "es" ? "" : "c.d|mt|" + Strings.Hash8("Two") + "|2\n"));
        var probeFound = Strings.Validate(probe);
        Check(probeFound.Contains("pt: table missing"), "G-STR: a listed language without a table fails");
        Check(probeFound.Contains("es: c.d missing"), "G-STR: a listed language without an id fails");
        Equal(2, probeFound.Count, "G-STR: only the missing table and the missing id fail (" + string.Join("; ", probeFound) + ")");
        var queue = Strings.ReviewQueue(Strings.Tables);
        Console.WriteLine("G-STR review queue: " + queue.Count + " translations not reviewed (" +
            string.Join(", ", Strings.Languages.Where(l => l != "en").Select(l => l + " " + queue.Count(q => q.StartsWith(l + " ")))) + ")");
    }
}
