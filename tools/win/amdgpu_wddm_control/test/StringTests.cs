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
        var queue = Strings.ReviewQueue(Strings.Tables);
        Console.WriteLine("G-STR review queue: " + queue.Count + " translations not reviewed (" +
            string.Join(", ", Strings.Languages.Where(l => l != "en").Select(l => l + " " + queue.Count(q => q.StartsWith(l + " ")))) + ")");
    }
}
