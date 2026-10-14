// Runs the actual GUI state wrapper, backup reader, Apply/rollback and CU setter
// against the native volatile HKCU fixture. No form or device is opened.
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Threading;
using AmdgpuWddmControl;

static class GuiBatchTest
{
    [DllImport("bc250control.dll", ExactSpelling=true, CharSet=CharSet.Unicode)]
    static extern uint AmdgpuTestSetup(string package);
    [DllImport("bc250control.dll", ExactSpelling=true)] static extern uint AmdgpuTestCleanup();
    [DllImport("bc250control.dll", ExactSpelling=true)] static extern uint AmdgpuTestFreeze(uint on);
    [DllImport("bc250control.dll", ExactSpelling=true, CharSet=CharSet.Unicode)]
    static extern uint AmdgpuTestNumber(string name,uint value);
    static int checks;
    static void Check(bool value,string what) { checks++;if(!value)throw new Exception(what); }
    static object Call(string name,params object[] args)
    { return typeof(RecoveryRunner).GetMethod(name,BindingFlags.Static|BindingFlags.NonPublic).Invoke(null,args); }
    sealed class Freezer : IDisposable
    {
        readonly ManualResetEvent entered=new ManualResetEvent(false),done=new ManualResetEvent(false);
        readonly Thread thread;
        uint result=uint.MaxValue;
        public Freezer()
        {
            thread=new Thread(()=>{entered.Set();result=AmdgpuTestFreeze(1);done.Set();});thread.Start();
            Check(entered.WaitOne(5000),"freeze thread starts");
            Check(!done.WaitOne(50),"installer freeze waits for the GUI batch");
        }
        public void Dispose()
        {
            Check(done.WaitOne(5000),"freeze finishes after batch disposal");
            thread.Join();Check(result==0,"freeze succeeds");entered.Dispose();done.Dispose();
        }
    }
    static void ApplyTest(bool fail)
    {
        Freezer freezer=null;
        try
        {
            using(var batch=DriverStateBatch.Begin())
            {
                var backup=new BackupRecord();
                foreach(string name in new[]{"CpuTune","DpmMaxMHz"})
                    backup.Values.Add((BackupValue)Call("Saved",DpmSettings.RegistryPath,name,false,batch));
                var writes=new List<RegWrite>{RegWrite.Dword(DpmSettings.RegistryPath,"CpuTune",0),
                    RegWrite.Dword(DpmSettings.RegistryPath,"DpmMaxMHz",fail?99999u:1500u)};
                freezer=new Freezer();
                Check((bool)Call("Apply",writes,backup,batch)==!fail,"actual Apply result");
                using(var state=batch.Open(effective:false))
                {
                    Check(fail?state.GetValue("CpuTune")==null:(int)state.GetValue("CpuTune")==0,"CpuTune raw rollback/write");
                    Check(fail?state.GetValue("DpmMaxMHz")==null:(int)state.GetValue("DpmMaxMHz")==1500,"DPM raw rollback/write");
                }
            }
        }
        finally { if(freezer!=null)freezer.Dispose(); }
        bool refused=false;
        try { using(var unexpected=DriverStateBatch.Begin()) {} } catch(Win32Exception e) {refused=e.NativeErrorCode==21;}
        Check(refused,"frozen batch rejected before backup");
        Check(AmdgpuTestFreeze(0)==0,"cancel fixture freeze");
    }
    static int Main(string[] args)
    {
        bool created=false;
        try
        {
            Check(args.Length==1,"package argument");Check(AmdgpuTestSetup(args[0])==0,"new isolated fixture");created=true;
            ApplyTest(true);ApplyTest(false);
            Check(AmdgpuTestNumber("CuMode",40)==0 && AmdgpuTestNumber("CuDisableWgp",1)==0 &&
                  AmdgpuTestNumber("CuModeConfirmed",40)==0,"prepare multi-step CU group");
            Freezer freezer=null;
            try
            {
                using(var api=new Win32RegApi(CuMode.ParametersPath))
                {
                    var registry=new CheckedCuRegistry(api);
                    var plan=CuMode.Plan(CuMode.ReadAll(registry),CuMode.Stock);
                    Check(!plan.Refused,"stock CU plan");freezer=new Freezer();
                    Check(CuMode.Execute(plan,registry).Completed,"actual CU sequence completes under one batch");
                    Check(registry.Read("CuMode")==24 && registry.Read("CuDisableWgp")==null && registry.Read("CuModeConfirmed")==null,"CU readback complete");
                }
            }
            finally { if(freezer!=null)freezer.Dispose(); }
            Check(AmdgpuTestFreeze(0)==0,"cancel CU freeze");
            var batch=DriverStateBatch.Begin();DriverStateKey retained=batch.Open();bool wrongThread=false;
            var foreign=new Thread(()=>{try{retained.GetValue("CpuTune");}catch(InvalidOperationException){wrongThread=true;}});
            foreign.Start();foreign.Join();Check(wrongThread,"managed batch rejects another thread");batch.Dispose();
            bool closed=false;try{retained.GetValue("CpuTune");}catch(ObjectDisposedException){closed=true;}
            Check(closed,"child cannot use disposed native batch");retained.Dispose();
            Console.WriteLine(checks+" GUI batch checks, 0 failures");return 0;
        }
        catch(Exception error){Console.WriteLine(error);return 1;}
        finally{if(created && AmdgpuTestCleanup()!=0)throw new Exception("fixture cleanup failed");}
    }
}
