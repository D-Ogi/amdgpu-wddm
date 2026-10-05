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
        public static string Output = "vidpn flip open: 12 hardware flips\nblit gate closed, 0 blits\nnode 0 hardware: 9 submitted, 9 completed\nnode 1 (paging, open): 17 hardware submitted, 17 completed";
        public static int Run(string path,string args,int timeout,out string output,out string error)
        {
            if(path!=CliPath || args!="log summary" || timeout!=5000)throw new Exception("unexpected subprocess");
            Calls++;output=Output;error="";return Exit;
        }
    }
}

static class GraphicsSummaryTest
{
    static int checks, failures;
    static void Check(bool value,string name){checks++;if(!value){failures++;Console.WriteLine("FAIL "+name);}}
    static string Value(Panel p,string key){var row=p.Rows.FirstOrDefault(r=>r.Label==key);return row==null?null:row.Value;}
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
            Check(Value(panel,"KMD counters")==null,"default not paused");
            File.WriteAllText(marker,"");
            panel=Poll(provider);
            Check(KmdInfoProvider.Calls==1,"pause skips subprocess");
            Check(Value(panel,"HW flips")=="12" && Value(panel,"Paging")=="SDMA 17/17","cached counters retained");
            Check(Value(panel,"KMD counters").StartsWith("paused; snapshot "),"cached snapshot visibly stale");
            panel=Poll(provider);Check(KmdInfoProvider.Calls==1,"successive pause does not call CLI");
            var cold=new GraphicsPipelineProvider(dir);panel=Poll(cold);
            Check(KmdInfoProvider.Calls==1,"cold paused instance also skips CLI");
            Check(Value(panel,"KMD counters")=="paused; no cached snapshot" && Value(panel,"HW flips")==null,"cold pause makes no runtime claim");
            File.Delete(marker);KmdInfoProvider.Output=KmdInfoProvider.Output.Replace("12 hardware", "99 hardware");
            panel=Poll(provider);Check(KmdInfoProvider.Calls==2&&Value(panel,"HW flips")=="99","remove marker resumes fresh poll");
            Check(Value(panel,"KMD counters")==null,"fresh poll clears paused indication");
            KmdInfoProvider.Exit=1;panel=Poll(provider);
            Check(KmdInfoProvider.Calls==3&&Value(panel,"KMD live")=="summary unavailable","normal failure still visible");
            File.WriteAllText(marker,"");panel=Poll(provider);
            Check(KmdInfoProvider.Calls==3&&Value(panel,"HW flips")=="99","pause after failure uses last successful cache");
            Check(Value(panel,"KMD counters").StartsWith("paused; snapshot "),"failure cache remains explicitly stale");
            File.Delete(marker);
        }
        finally {if(File.Exists(marker))File.Delete(marker);Directory.Delete(dir);}
        Console.WriteLine("graphics summary pause: {0} checks, {1} failures",checks,failures);
        return failures==0?0:1;
    }
}
