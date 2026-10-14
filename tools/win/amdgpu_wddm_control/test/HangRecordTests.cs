// Host tests of the hang record as a bug report prints it (src/HangRecord.cs, BD-168): LastTime carries its UTC
// instant beside the number, every other value is printed as it stands, and a LastTime that is not a system time
// is printed as it stands as well. The contract with the driver that writes the record (driver/kmd/guard.c) is
// checked here too: the subkey name and the value names of the record are the ones this report asks for.
using System;
using System.IO;
using System.Text.RegularExpressions;
using AmdgpuWddmControl;

static partial class UnitTests
{
    static void HangRecordTests(string root)
    {
        // 2026-10-14T07:00:00Z as a Windows system time, the shape the driver writes.
        long ticks = new DateTime(2026, 10, 14, 7, 0, 0, DateTimeKind.Utc).ToFileTimeUtc();
        Equal(ticks.ToString() + " (2026-10-14T07:00:00Z)", HangRecord.Value("LastTime", ticks.ToString()),
              "LastTime carries the UTC instant beside the number");
        Equal("3", HangRecord.Value("Count", "3"), "another value is printed as it stands");
        Equal(ticks.ToString(), HangRecord.Value("Count", ticks.ToString()),
              "a system time under another name is not converted");
        foreach (var bad in new[] { "0", "-1", "", "not a number", "9223372036854775807" })
            Equal(bad, HangRecord.Value("LastTime", bad), "LastTime " + (bad == "" ? "<empty>" : bad) + " is printed as it stands");

        // The driver's side of it: the subkey this report reads is the one the driver creates, and LastTime is
        // one of the values it writes. A rename on either side would otherwise leave an empty heading in every
        // report and nobody the wiser.
        string guard = File.ReadAllText(Path.Combine(root, "driver", "kmd", "guard.c"));
        Check(Regex.IsMatch(guard, "\"" + HangRecord.SubKey + "\"|L\"" + HangRecord.SubKey + "\""),
              "driver/kmd/guard.c names the subkey " + HangRecord.SubKey);
        Check(guard.Contains("LastTime"), "driver/kmd/guard.c writes LastTime");
    }
}
