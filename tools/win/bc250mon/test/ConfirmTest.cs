using System;
using System.ComponentModel;
using System.IO;
using Microsoft.Win32;
static class ConfirmModel
{
    public static bool Missing, WriteFails;
    public static int FlushError, Writes, Flushes, Closes, Cached, Durable;
    public static void Reset() { Missing=WriteFails=false; FlushError=Writes=Flushes=Closes=0; Cached=Durable=2; }
}
sealed class FakeKey : IDisposable
{
    public bool Closed;
    public FakeKey Handle { get { return this; } }
    public FakeKey OpenSubKey(string path,bool writable) { return ConfirmModel.Missing ? null : new FakeKey(); }
    public FakeKey CreateSubKey(string path) { return new FakeKey(); }
    public void SetValue(string name,int value,RegistryValueKind kind)
    { ConfirmModel.Writes++; if(ConfirmModel.WriteFails)throw new IOException("write denied"); ConfirmModel.Cached=value; }
    public void Dispose() { Closed=true; ConfirmModel.Closes++; }
}
sealed class RegistryUnderTest
{
    readonly FakeKey _hive=new FakeKey();
    const string _subPath="fixture", Path="fixture";
    static int RegFlushKey(FakeKey key)
    {
        if(key.Closed)throw new Exception("flush after close");
        ConfirmModel.Flushes++;
        if(ConfirmModel.FlushError==0)ConfirmModel.Durable=ConfirmModel.Cached;
        return ConfirmModel.FlushError;
    }
    // ACTUAL_CONFIRM_METHOD
}
static class ConfirmTest
{
    static int checks;
    static void Check(bool condition,string name){checks++;if(!condition)throw new Exception(name);}
    static int Main()
    {
        try {
            var registry=new RegistryUnderTest();
            ConfirmModel.Reset();registry.Confirm();
            Check(ConfirmModel.Cached==0&&ConfirmModel.Durable==0&&ConfirmModel.Flushes==1&&ConfirmModel.Closes==2,"successful persistent confirmation");
            ConfirmModel.Reset();ConfirmModel.WriteFails=true;bool rejected=false;
            try{registry.Confirm();}catch(IOException){rejected=true;}
            Check(rejected&&ConfirmModel.Flushes==0&&ConfirmModel.Durable==2&&ConfirmModel.Closes==2,"write failure cannot confirm");
            ConfirmModel.Reset();ConfirmModel.FlushError=1017;rejected=false;
            try{registry.Confirm();}catch(Win32Exception e){rejected=e.NativeErrorCode==1017;}
            Check(rejected&&ConfirmModel.Cached==0&&ConfirmModel.Durable==2&&ConfirmModel.Closes==2,"cached zero is not durable success");
            ConfirmModel.Reset();ConfirmModel.Missing=true;rejected=false;
            try{registry.Confirm();}catch(InvalidOperationException){rejected=true;}
            Check(rejected&&ConfirmModel.Writes==0&&ConfirmModel.Flushes==0,"missing service not created");
            Console.WriteLine("Monitor confirmation: {0} checks passed",checks);return 0;
        }catch(Exception e){Console.Error.WriteLine(e);return 1;}
    }
}
