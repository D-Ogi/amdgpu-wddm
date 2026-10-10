// The OS culture must not change the machine-facing contracts of the UI.
using System;
using System.Globalization;
using System.Linq;
using System.Threading;
using AmdgpuWddmControl;

static partial class UnitTests
{
    static void LocaleTests()
    {
        var before = Thread.CurrentThread.CurrentCulture;
        var beforeUi = Thread.CurrentThread.CurrentUICulture;
        try
        {
            foreach (var name in new[] { "en-US", "pl-PL", "es-ES", "ja-JP", "ko-KR" })
            {
                Thread.CurrentThread.CurrentCulture = CultureInfo.GetCultureInfo(name);
                Thread.CurrentThread.CurrentUICulture = CultureInfo.GetCultureInfo(name);
                var curve = Enumerable.Repeat(820u, Tuner.Points).ToArray();
                var wire = string.Join(",", Enumerable.Repeat("820", Tuner.Points));
                Equal(wire, TunerPlan.CurveText(curve), name + ": voltage curve wire format");
                Check(TunerPlan.ParseCurve(wire).SequenceEqual(curve), name + ": voltage curve roundtrip");
                Check(TunerPlan.ParseCurve(wire.Replace("820", "820,0")) == null, name + ": fractional voltage refused");
                uint[] c, pct;
                Equal("40:50,60:70,85:100", FanCurves.CurveText(new uint[] { 40, 60, 85 }, new uint[] { 50, 70, 100 }), name + ": fan wire format");
                Check(FanCurves.ParseCurve("40:50,60:70,85:100", out c, out pct) && c[2] == 85 && pct[2] == 100, name + ": fan parse");
                Check(!FanCurves.ParseCurve("40,5:50,60:70,85:100", out c, out pct), name + ": fractional fan temperature refused");
                var utc = new DateTime(2026, 10, 10, 12, 34, 56, DateTimeKind.Utc);
                Equal((DateTime?)utc, Recovery.Utc("2026-10-10T12:34:56Z"), name + ": persisted UTC reading");
                Check(ReleaseVersion.Parse("0.7.216.100-tester.26").CompareTo(ReleaseVersion.Parse("0.7.216.100-tester.25")) > 0, name + ": version ordering");
            }
        }
        finally { Thread.CurrentThread.CurrentCulture = before; Thread.CurrentThread.CurrentUICulture = beforeUi; }
    }
}
