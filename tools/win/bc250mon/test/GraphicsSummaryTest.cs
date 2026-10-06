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
        public static string LastArgs;
        // The escape counts of each form of the read. A "log 0" page read costs no Level Two escape at all; a
        // requested "log summary only" costs exactly one, for the summary's own first page (C55, BD-054).
        public const string PageCounts = "\n             escapes: 3 without adapter synchronization, 0 with HardwareAccess";
        public const string SummaryCounts = "\n             escapes: 2 without adapter synchronization, 1 with HardwareAccess";
        public const string Header = "log          908 lines since this driver load, 0 lost to the wrap, 0 dropped above DISPATCH_LEVEL; ring 1024 lines of which the first 64 are kept; table FULL WDDM (the gate was open at DriverEntry)";
        // What the CLI prints for a ring line: "<sequence> <seconds>.<ms> <text>" (bc250kmd_cli Log), and every line
        // the summary itself writes begins "wddm summary:" (wddm.c WddmSummary). KMD 0.7.208 shape (BD-070): the
        // scan-out and vidpn flip summaries are two lines each. The second line of each pair is here on purpose. The
        // provider must read the first line of a pair and must not take "vidpn flip vsyncs 300 armed" for the flip line.
        public const string SummaryBlock =
            "   900     40.000 wddm summary: scan-out flips 0 of 4 requested candidates; admission ok/no-alloc/not-requested 0/0/4\n" +
            "   901     40.001 wddm summary: scan-out refusals format/geom/pitch/size/segment/align/gated 0/0/0/0/0/0/0\n" +
            "   902     40.002 wddm summary: vidpn flip open: 12 hardware flips, 0 refused\n" +
            "   903     40.003 wddm summary: vidpn flip vsyncs 300 armed, 300 acked, 0 refused, 0 completion-deferred, 0 old-buffer-reports\n" +
            "   904     40.004 wddm summary: blit gate closed, 0 blits\n" +
            "   905     40.005 wddm summary: node 0 hardware: 9 submitted, 9 completed\n" +
            "   906     40.006 wddm summary: node 1 (paging, open): 17 hardware submitted, 17 completed";
        // The newest line of the ring, an ordinary one 20 s of driver time after the summary block.
        public const string Tail = "\n   907     60.006 dcn: vsync 12345";
        public static string Output = Header + "\n" + SummaryBlock + Tail + PageCounts;
        public static int Run(string path,string args,int timeout,out string output,out string error)
        {
            if(path!=CliPath || timeout!=5000)throw new Exception("unexpected subprocess");
            if(args!=GraphicsPipelineProvider.PageArgs && args!=GraphicsPipelineProvider.SummaryArgs)
                throw new Exception("unexpected arguments \"" + args + "\"");
            Calls++;LastArgs=args;output=Output;error="";return Exit;
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
    // A ring the stub hands back: the header, the summary block, a newest line and the escape counts of one form.
    static string Ring(string counts,string tail){return KmdInfoProvider.Header+"\n"+KmdInfoProvider.SummaryBlock+tail+counts;}
    static string Ring(string counts){return Ring(counts,KmdInfoProvider.Tail);}
    static int Main(string[] args)
    {
        if(args.Length!=1)return 2;
        Directory.CreateDirectory(args[0]);
        string dir=Path.Combine(args[0],"summary-"+Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        string marker=Path.Combine(dir,GraphicsPipelineProvider.SummaryPauseFileName);
        int before;
        try
        {
            // C55: the schedule reads the ring, it does not write a summary. LOG_SUMMARY is a Level Two escape, so
            // dxgkrnl suspends the GPU scheduler for it, and polling it every 5.4 s cost a running game a 300 ms
            // stall each time and about 2 % of its frame rate (trial 41).
            Check(GraphicsPipelineProvider.PageArgs=="log 0","the scheduled read is a GET_LOG page read");
            Check(GraphicsPipelineProvider.SummaryArgs=="log summary only","the requested form is the summary's own lines");
            var provider=new GraphicsPipelineProvider(dir);
            KmdInfoProvider.Output=Ring(KmdInfoProvider.PageCounts);
            var panel=Poll(provider);
            Check(KmdInfoProvider.Calls==1 && KmdInfoProvider.LastArgs=="log 0","the schedule sends no LOG_SUMMARY");
            Check(Value(panel,"HW flips")=="12" && Value(panel,"GPU compute")=="9/9 fences completed","real output parser");
            Check(Value(panel,"Scanout")=="DCN flip + hardware VSync","the vsync line of the 0.7.208 pair is not the flip line");
            Check(Find(panel,"KMD poll")!=null && Find(panel,"KMD poll").Level==Level.Good &&
                  Value(panel,"KMD poll")=="no Level Two escape, 3 without adapter synchronization","a page read costs no Level Two escape");
            Check(Value(panel,"KMD counters")=="from the ring's last summary, 20 s before the newest log line" &&
                  Find(panel,"KMD counters").Level==Level.Info,"the counter block is dated against the newest line");
            panel=Poll(provider);
            Check(KmdInfoProvider.Calls==2 && KmdInfoProvider.LastArgs=="log 0","and the next poll of the schedule does the same");

            // The explicit operator request (the "graphics.summary" action of the overlay's table), and one request
            // is one summary: the poll after it is a page read again.
            provider.RequestSummary();
            KmdInfoProvider.Output=Ring(KmdInfoProvider.SummaryCounts,"");
            panel=Poll(provider);
            Check(KmdInfoProvider.Calls==3 && KmdInfoProvider.LastArgs=="log summary only","a requested summary sends LOG_SUMMARY");
            Check(Value(panel,"KMD poll")=="1 Level Two escape, 2 without adapter synchronization" &&
                  Find(panel,"KMD poll").Level==Level.Good,"one Level Two escape is what a requested summary may cost");
            Check(Value(panel,"KMD counters")=="summary written by this poll, on request" &&
                  Find(panel,"KMD counters").Level==Level.Good,"a requested summary is named as fresh");
            Check(Value(panel,"HW flips")=="12","a requested summary parses the same way");
            provider.RequestSummary();provider.RequestSummary();
            KmdInfoProvider.Output=Ring(KmdInfoProvider.SummaryCounts,"");
            panel=Poll(provider);Check(KmdInfoProvider.LastArgs=="log summary only","repeated requests between two polls are one summary");
            KmdInfoProvider.Output=Ring(KmdInfoProvider.PageCounts);
            panel=Poll(provider);Check(KmdInfoProvider.Calls==5 && KmdInfoProvider.LastArgs=="log 0","the request is consumed by that one poll");

            // A ring with no summary in it makes no counter claim at all.
            KmdInfoProvider.Output=KmdInfoProvider.Header+KmdInfoProvider.Tail+KmdInfoProvider.PageCounts;
            panel=Poll(provider);
            Check(Value(panel,"KMD counters")=="no summary in the ring; the graphics.summary action writes one" &&
                  Find(panel,"KMD counters").Level==Level.Warn,"no summary in the ring is said so");
            Check(Value(panel,"HW flips")==null && Value(panel,"Scanout")=="not reported","and no counter is invented");
            // A block older than ten minutes of driver time is amber, not quietly current.
            KmdInfoProvider.Output=Ring(KmdInfoProvider.PageCounts,"\n   908   1240.500 dcn: vsync 99999");
            panel=Poll(provider);
            Check(Value(panel,"KMD counters")=="from the ring's last summary, 1200 s before the newest log line" &&
                  Find(panel,"KMD counters").Level==Level.Warn,"a block older than ten minutes warns");

            KmdInfoProvider.Output=Ring(KmdInfoProvider.PageCounts);
            panel=Poll(provider);Check(Value(panel,"HW flips")=="12","a fresh page read before the pause");
            File.WriteAllText(marker,"");
            before=KmdInfoProvider.Calls;
            panel=Poll(provider);
            Check(KmdInfoProvider.Calls==before,"pause skips subprocess");
            Check(Value(panel,"HW flips")=="12" && Value(panel,"Paging")=="SDMA 17/17","cached counters retained");
            Check(Value(panel,"KMD counters").StartsWith("paused; snapshot "),"cached snapshot visibly stale");
            Check(Value(panel,"KMD poll")==null,"paused poll makes no escape claim");
            panel=Poll(provider);Check(KmdInfoProvider.Calls==before,"successive pause does not call CLI");
            // A request made while the marker exists is dropped with it, not held until the marker goes, and the
            // action is told so rather than promising a summary nothing will write.
            Check(!provider.RequestSummary(),"a request while paused is refused, not queued");
            panel=Poll(provider);
            Check(KmdInfoProvider.Calls==before,"a request while paused sends nothing");
            var cold=new GraphicsPipelineProvider(dir);panel=Poll(cold);
            Check(KmdInfoProvider.Calls==before,"cold paused instance also skips CLI");
            Check(Value(panel,"KMD counters")=="paused; no cached snapshot" && Value(panel,"HW flips")==null,"cold pause makes no runtime claim");
            File.Delete(marker);KmdInfoProvider.Output=KmdInfoProvider.Output.Replace("12 hardware","99 hardware");
            panel=Poll(provider);
            Check(KmdInfoProvider.Calls==before+1 && Value(panel,"HW flips")=="99" && KmdInfoProvider.LastArgs=="log 0","remove marker resumes a fresh page read, and the dropped request stays dropped");
            Check(Value(panel,"KMD counters").StartsWith("from the ring's last summary"),"fresh poll clears paused indication");

            // BD-054: a driver or a CLI that took the adapter lock for the pages, and a CLI with no count line.
            string good=KmdInfoProvider.Output;
            KmdInfoProvider.Output=good.Replace("3 without adapter synchronization, 0 with","0 without adapter synchronization, 3 with");
            panel=Poll(provider);
            Check(Find(panel,"KMD poll").Level==Level.Warn && Value(panel,"KMD poll")=="3 Level Two escapes, expected 0","Level Two escapes on a page read warn");
            Check(Value(panel,"HW flips")=="99","counters still shown beside the warning");
            KmdInfoProvider.Output=good.Replace(KmdInfoProvider.PageCounts,"");
            panel=Poll(provider);
            Check(Find(panel,"KMD poll").Level==Level.Warn && Value(panel,"KMD poll").StartsWith("no escape counts"),"missing count line warns");
            KmdInfoProvider.Output=good;

            before=KmdInfoProvider.Calls;
            KmdInfoProvider.Exit=2;panel=Poll(provider);
            Check(KmdInfoProvider.Calls==before+1 && Value(panel,"KMD live")=="log unavailable: stub-cli did not accept \"log 0\"","a CLI that refuses the page read is named");
            Check(Value(panel,"KMD poll")==null,"failed poll makes no escape claim");
            Check(provider.RequestSummary(),"a request outside the pause is accepted");
            panel=Poll(provider);
            Check(Value(panel,"KMD live")=="summary unavailable: stub-cli predates \"log summary only\"","a CLI from before \"only\" is still named by the requested summary");
            // A read that failed wrote no summary, so the request is not spent: the next poll asks again, and the
            // one that succeeds is the one that consumes it.
            panel=Poll(provider);
            Check(KmdInfoProvider.LastArgs=="log summary only","a failed summary read leaves the request pending");
            KmdInfoProvider.Exit=0;
            KmdInfoProvider.Output=Ring(KmdInfoProvider.SummaryCounts,"").Replace("12 hardware","99 hardware");
            panel=Poll(provider);
            Check(KmdInfoProvider.LastArgs=="log summary only" &&
                  Value(panel,"KMD counters")=="summary written by this poll, on request","the pending request writes the summary once the read works");
            KmdInfoProvider.Output=good;
            panel=Poll(provider);
            Check(KmdInfoProvider.LastArgs=="log 0","and a written summary is not asked for a second time");

            KmdInfoProvider.Exit=1;before=KmdInfoProvider.Calls;panel=Poll(provider);
            Check(KmdInfoProvider.Calls==before+1&&Value(panel,"KMD live")=="log unavailable","normal failure still visible");
            File.WriteAllText(marker,"");before=KmdInfoProvider.Calls;panel=Poll(provider);
            Check(KmdInfoProvider.Calls==before&&Value(panel,"HW flips")=="99","pause after failure uses last successful cache");
            Check(Value(panel,"KMD counters").StartsWith("paused; snapshot "),"failure cache remains explicitly stale");
            File.Delete(marker);
        }
        finally {if(File.Exists(marker))File.Delete(marker);Directory.Delete(dir);}
        Console.WriteLine("graphics summary pause: {0} checks, {1} failures",checks,failures);
        return failures==0?0:1;
    }
}
