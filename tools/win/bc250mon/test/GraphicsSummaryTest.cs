using System;
using System.IO;
using System.Linq;
using Bc250Mon;

namespace Bc250Mon
{
    public interface IProvider { string Name { get; } TimeSpan Period { get; } void Poll(State state); }
    // The real GraphicsPipelineProvider executes against this boundary. No CLI,
    // driver DLL, registry, DWM inspection or UI is invoked by these tests.
    public static class KmdInfoProvider
    {
        public const string CliPath = "stub-cli";
        public static int Calls, Exit;
        public const string Counts = "\n             escapes: 2 without adapter synchronization, 1 with HardwareAccess";
        // KMD 0.7.208 shape (BD-070): the scan-out and vidpn flip summaries are two lines each.
        // The second line of each pair is here on purpose. The provider must read the first line
        // of a pair and must not take "vidpn flip vsyncs 300 armed" for the flip line.
        public static string Output = "wddm summary: scan-out flips 0 of 4 requested candidates; admission ok/no-alloc/not-requested 0/0/4\nwddm summary: scan-out refusals format/geom/pitch/size/segment/align/gated 0/0/0/0/0/0/0\nvidpn flip open: 12 hardware flips, 0 refused\nwddm summary: vidpn flip vsyncs 300 armed, 300 acked, 0 refused, 0 completion-deferred, 0 old-buffer-reports\nblit gate closed, 0 blits\nnode 0 hardware: 9 submitted, 9 completed\nnode 1 (paging, open): 17 hardware submitted, 17 completed" + Counts;
        public static int Run(string path,string args,int timeout,out string output,out string error)
        {
            if(path!=CliPath || args!="log summary only" || timeout!=5000)throw new Exception("unexpected subprocess");
            Calls++;output=Output;error="";return Exit;
        }
    }
}

static class GraphicsSummaryTest
{
    static int checks, failures;
    static void Check(bool value,string name){checks++;if(!value){failures++;Console.WriteLine("FAIL "+name);}}
    static Row Find(Panel p,string key){return p.Rows.FirstOrDefault(r=>r.Label==key);}
    static string Value(Panel p,string key){var row=Find(p,key);return row==null?null:row.Value;}
    static Panel Poll(GraphicsPipelineProvider provider){var p=new Panel();provider.AddKernelSummary(p);return p;}
    static int Main(string[] args)
    {
        if(args.Length!=1)return 2;
        Directory.CreateDirectory(args[0]);
        string dir=Path.Combine(args[0],"summary-"+Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        string marker=Path.Combine(dir,GraphicsPipelineProvider.SummaryPauseFileName);
        try
        {
            var provider=new GraphicsPipelineProvider(dir);
            var panel=Poll(provider);
            Check(KmdInfoProvider.Calls==1,"default retains summary subprocess");
            Check(Value(panel,"HW flips")=="12" && Value(panel,"GPU compute")=="9/9 fences completed","real output parser");
            Check(Value(panel,"Scanout")=="DCN flip + hardware VSync","the vsync line of the 0.7.208 pair is not the flip line");
            Check(Value(panel,"KMD counters")==null,"default not paused");
            Check(Find(panel,"KMD poll")!=null && Find(panel,"KMD poll").Level==Level.Good &&
                  Value(panel,"KMD poll")=="1 Level Two escape, 2 without adapter synchronization","one Level Two escape is good");
            File.WriteAllText(marker,"");
            panel=Poll(provider);
            Check(KmdInfoProvider.Calls==1,"pause skips subprocess");
            Check(Value(panel,"HW flips")=="12" && Value(panel,"Paging")=="SDMA 17/17","cached counters retained");
            Check(Value(panel,"KMD counters").StartsWith("paused; snapshot "),"cached snapshot visibly stale");
            Check(Value(panel,"KMD poll")==null,"paused poll makes no escape claim");
            panel=Poll(provider);Check(KmdInfoProvider.Calls==1,"successive pause does not call CLI");
            var cold=new GraphicsPipelineProvider(dir);panel=Poll(cold);
            Check(KmdInfoProvider.Calls==1,"cold paused instance also skips CLI");
            Check(Value(panel,"KMD counters")=="paused; no cached snapshot" && Value(panel,"HW flips")==null,"cold pause makes no runtime claim");
            File.Delete(marker);KmdInfoProvider.Output=KmdInfoProvider.Output.Replace("12 hardware", "99 hardware");
            panel=Poll(provider);Check(KmdInfoProvider.Calls==2&&Value(panel,"HW flips")=="99","remove marker resumes fresh poll");
            Check(Value(panel,"KMD counters")==null,"fresh poll clears paused indication");

            // BD-054: a driver that refused the unsynchronized pages, and a CLI that predates the count line.
            string good=KmdInfoProvider.Output;
            KmdInfoProvider.Output=good.Replace("2 without adapter synchronization, 1 with","0 without adapter synchronization, 3 with");
            panel=Poll(provider);
            Check(Find(panel,"KMD poll").Level==Level.Warn && Value(panel,"KMD poll").StartsWith("3 Level Two escapes per poll"),"several Level Two escapes warn");
            Check(Value(panel,"HW flips")=="99","counters still shown beside the warning");
            KmdInfoProvider.Output=good.Replace(KmdInfoProvider.Counts,"");
            panel=Poll(provider);
            Check(Find(panel,"KMD poll").Level==Level.Warn && Value(panel,"KMD poll").StartsWith("no escape counts"),"missing count line warns");
            KmdInfoProvider.Output=good;
            KmdInfoProvider.Exit=2;panel=Poll(provider);
            Check(KmdInfoProvider.Calls==5 && Value(panel,"KMD live")=="summary unavailable: stub-cli predates \"log summary only\"","an older CLI's usage error is named");
            Check(Value(panel,"KMD poll")==null,"failed poll makes no escape claim");

            KmdInfoProvider.Exit=1;panel=Poll(provider);
            Check(KmdInfoProvider.Calls==6&&Value(panel,"KMD live")=="summary unavailable","normal failure still visible");
            File.WriteAllText(marker,"");panel=Poll(provider);
            Check(KmdInfoProvider.Calls==6&&Value(panel,"HW flips")=="99","pause after failure uses last successful cache");
            Check(Value(panel,"KMD counters").StartsWith("paused; snapshot "),"failure cache remains explicitly stale");
            File.Delete(marker);
        }
        finally {if(File.Exists(marker))File.Delete(marker);Directory.Delete(dir);}
        Console.WriteLine("graphics summary pause: {0} checks, {1} failures",checks,failures);
        return failures==0?0:1;
    }
}
