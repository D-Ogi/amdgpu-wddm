using System;
using System.Runtime.InteropServices;
using Bc250Mon;
static class FakeEnvironment { public static int TickCount; }
sealed class FakeClock { public long ElapsedMilliseconds; }
enum Level { Info, Warn, Error, Good }
sealed class State { public string Last; public void Log(string name, Level level, string text) { Last=text; } }
sealed class KmdSnapshot { public int? LastStage; }
static class KmdStages { public const int FirstPresentDone=61; public static string Describe(int stage) { return stage.ToString(); } }
sealed class FakeRegistry { public int Confirms; public void Confirm() { Confirms++; } }
static class FakeDriver
{
    public static StartHealthSnapshot Sample;
    public static bool Missing, FailConfirm, ChangeEpochOnConfirm;
    public static int Reads, Confirms;
    public static ulong ExpectedGeneration, ExpectedEpoch;
    public static void Reset() { Missing=FailConfirm=ChangeEpochOnConfirm=false; Reads=Confirms=0; }
    public static StartHealthSnapshot ReadStartHealth()
    {
        Reads++;
        if(Missing)throw new EntryPointNotFoundException("old DLL");
        Driver.ValidateStartHealthReply(0,Sample,0,0,0);return Sample;
    }
    public static StartHealthSnapshot ConfirmStartHealth(ulong generation, ulong epoch)
    {
        Confirms++;ExpectedGeneration=generation;ExpectedEpoch=epoch;
        var reply=Sample;reply.Op=1;reply.Flags|=8;if(ChangeEpochOnConfirm)reply.Epoch++;
        Driver.ValidateStartHealthReply(FailConfirm?unchecked((int)0xC0000185):0,reply,1,generation,epoch);
        return reply;
    }
}
sealed class ProviderUnderTest
{
    readonly FakeClock _healthClock=new FakeClock();
    readonly StartConfirmationPolicy _healthPolicy=new StartConfirmationPolicy();
    public readonly FakeRegistry _kmd=new FakeRegistry();
    readonly State _state=new State();
    string _lastError,_lastHealthError;
    bool _retryFullConfirmation;
    int _pendingStarts=-1,_pendingSince;
    const int ConfirmAfterSeconds=60;
    string Name { get { return "test"; } }
    int UptimeSeconds { get { return (int)(_healthClock.ElapsedMilliseconds/1000); } }
    public string Tick(long now, int stage, int starts)
    {
        _healthClock.ElapsedMilliseconds=now;FakeEnvironment.TickCount=(int)now;
        return Confirm(_state,new KmdSnapshot { LastStage=stage },starts);
    }
    // ACTUAL_PROVIDER_METHODS
}
static class StartConfirmationTest
{
    static int checks;
    static void Check(bool value,string reason) { checks++;if(!value)throw new Exception(reason); }
    static StartHealthSnapshot Good(long now,ulong count,ulong generation=11,ulong epoch=2)
    { return new StartHealthSnapshot { Magic=0x30353242,Command=21,AbiVersion=1,Flags=7,Generation=generation,Epoch=epoch,Completed=count,LastCompletionAgeMs=10,ReadyAgeMs=(ulong)(now+60000) }; }
    static void Rejected(StartHealthSnapshot s,uint op,ulong generation,ulong epoch,string reason)
    { bool threw=false;try { Driver.ValidateStartHealthReply(0,s,op,generation,epoch); }catch(InvalidOperationException){threw=true;}Check(threw,reason); }
    static int Main()
    {
        try {
            Check(Marshal.SizeOf(typeof(StartHealthSnapshot))==96,"ABI96");
            Check(Marshal.OffsetOf(typeof(StartHealthSnapshot),"Generation").ToInt32()==32,"generation offset");
            Check(Marshal.OffsetOf(typeof(StartHealthSnapshot),"ExpectedEpoch").ToInt32()==80,"epoch offset");
            var p=new StartConfirmationPolicy();
            for(int i=0;i<=12;i++)Check(p.Observe(i*5000,Good(i*5000,(ulong)i+1))==(i==12),"60s continuous progress");
            p.Reset();for(int i=0;i<30;i++)Check(!p.Observe(i*5000,Good(i*5000,1)),"same completed surface cannot confirm");
            p.Reset();for(int i=0;i<12;i++)p.Observe(i*5000,Good(i*5000,(ulong)i+1));
            Check(!p.Observe(60000,Good(60000,13,12,2))&&p.ObservedMilliseconds==0,"new generation, same registry counter");
            for(int i=1;i<12;i++)Check(!p.Observe(60000+i*5000,Good(60000+i*5000,(ulong)i+13,12,2)),"new generation needs full interval");
            Check(p.Observe(120000,Good(120000,25,12,2)),"new generation completes at60s");
            Check(!p.Observe(125000,Good(125000,26,12,3))&&p.ObservedMilliseconds==0,"visibility epoch restarts");
            Check(!p.Observe(145001,Good(145001,27,12,3))&&p.ObservedMilliseconds==0,"long poll gap restarts");
            Check(!p.Observe(150000,Good(150000,1,12,3))&&p.ObservedMilliseconds==0,"counter regression");
            p.Reset();for(int i=0;i<=4;i++)Check(p.Observe(i*15000,Good(i*15000,(ulong)i+1))==(i==4),"15s gap allowed");
            var unhealthy=Good(65000,6);unhealthy.LastCompletionAgeMs=15001;
            Check(!p.Observe(65000,unhealthy)&&p.ObservedMilliseconds==0,"stale completion");
            for(uint flags=0;flags<7;flags++){var bad=Good(0,1);bad.Flags=flags;Check(!p.Observe(0,bad),"required flags");}
            unhealthy=Good(0,0);Check(!p.Observe(0,unhealthy),"no completed presentation");
            p.Observe(0,Good(0,1));p.Reset();Check(!p.Observe(60000,Good(60000,2)),"missing sample resets");
            Check(!p.Observe(59000,Good(59000,3))&&p.ObservedMilliseconds==0,"clock regression resets");
            p.Reset();for(int i=0;i<=12;i++){var young=Good(i*5000,(ulong)i+1);young.ReadyAgeMs=50000;Check(!p.Observe(i*5000,young),"ready age below60s");}
            p.Reset();p.Observe(0,Good(0,1));var ageRegression=Good(5000,2);ageRegression.ReadyAgeMs=1;
            Check(!p.Observe(5000,ageRegression)&&p.ObservedMilliseconds==0,"ready age regression");
            var reply=Good(0,1);reply.AbiVersion=2;Rejected(reply,0,0,0,"unknown ABI");
            reply=Good(0,1);reply.Status=2;Rejected(reply,0,0,0,"typed refusal");
            reply=Good(0,1);reply.NtStatus=0xC0000185;Rejected(reply,0,0,0,"checked flush error");
            reply=Good(0,1);reply.Op=1;Rejected(reply,1,11,2,"no confirmed flag");
            reply.Flags=15;Rejected(reply,1,12,2,"generation changed before confirm");Rejected(reply,1,11,3,"epoch changed before confirm");
            Driver.ValidateStartHealthReply(0,reply,1,11,2);Check(true,"exact confirmed identity");
            bool failedTransport=false;try { Driver.ValidateStartHealthReply(unchecked((int)0xC0000185),reply,1,11,2); }catch(InvalidOperationException){failedTransport=true;}
            Check(failedTransport,"transport failure cannot confirm despite successful payload");

            var provider=new ProviderUnderTest();FakeDriver.Reset();
            for(int i=0;i<=12;i++){FakeDriver.Sample=Good(i*5000,(ulong)i+1);provider.Tick(i*5000,50,1);Check(FakeDriver.Confirms==(i==12?1:0),"stage50 not shortcut");}
            Check(provider._kmd.Confirms==0&&FakeDriver.ExpectedGeneration==11&&FakeDriver.ExpectedEpoch==2,"full path never writes registry directly");
            provider=new ProviderUnderTest();FakeDriver.Reset();FakeDriver.Missing=true;
            for(int i=0;i<=15;i++)provider.Tick(i*5000,61,1);
            Check(FakeDriver.Confirms==0&&provider._kmd.Confirms==0,"old DLL cannot use stage61 fallback");
            provider=new ProviderUnderTest();FakeDriver.Reset();FakeDriver.Sample=Good(0,1);FakeDriver.Sample.AbiVersion=0;
            provider.Tick(0,61,1);provider.Tick(120000,61,1);
            Check(provider._kmd.Confirms==0,"old ABI cannot use DDO fallback");
            provider=new ProviderUnderTest();FakeDriver.Reset();FakeDriver.Sample=Good(0,1);FakeDriver.Sample.Flags=0;
            provider.Tick(0,61,1);provider.Tick(60000,61,1);
            Check(provider._kmd.Confirms==1&&FakeDriver.Confirms==0,"positive DDO retains stage61 policy");
            provider=new ProviderUnderTest();FakeDriver.Reset();FakeDriver.ChangeEpochOnConfirm=true;
            string last="";
            for(int i=0;i<=12;i++){FakeDriver.Sample=Good(i*5000,(ulong)i+1);last=provider.Tick(i*5000,50,1);}
            Check(last.StartsWith("waiting:") && FakeDriver.Confirms==1 && provider._kmd.Confirms==0,"epoch change immediately before confirm refused");
            provider=new ProviderUnderTest();FakeDriver.Reset();FakeDriver.FailConfirm=true;
            for(int i=0;i<=12;i++){FakeDriver.Sample=Good(i*5000,(ulong)i+1);provider.Tick(i*5000,50,1);}
            Check(FakeDriver.Confirms==1&&provider._kmd.Confirms==0,"failed confirm no success fallback");
            FakeDriver.FailConfirm=false;
            for(int i=13;i<=25;i++){FakeDriver.Sample=Good(i*5000,(ulong)i+1);provider.Tick(i*5000,50,0);}
            Check(FakeDriver.Confirms==2,"cached registry zero after failed flush retains typed retry");
            Console.WriteLine("Start confirmation policy/provider: {0} checks passed",checks);return 0;
        } catch(Exception e) {Console.Error.WriteLine(e);return 1;}
    }
}
