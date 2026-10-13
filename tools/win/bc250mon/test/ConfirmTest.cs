using System;
using System.ComponentModel;
static class ConfirmModel
{
    public static int OpenError, WriteError, Writes, Closes;
    public static void Reset() { OpenError=WriteError=Writes=Closes=0; }
}
sealed class FakeState : IDisposable
{
    public void WriteDword(string path,string name,uint value)
    { ConfirmModel.Writes++; if(path!=""||name!="UnconfirmedStarts"||value!=0)throw new Exception("wrong write"); if(ConfirmModel.WriteError!=0)throw new Win32Exception(ConfirmModel.WriteError); }
    public void IgnoreWriteDword(string path,string name,uint value) { try{WriteDword(path,name,value);}catch(Win32Exception){} }
    public void Dispose() { ConfirmModel.Closes++; }
}
static class DriverState
{
    public static FakeState Open(bool writable)
    { if(!writable)throw new Exception("not writable"); if(ConfirmModel.OpenError!=0)throw new Win32Exception(ConfirmModel.OpenError); return new FakeState(); }
}
sealed class RegistryUnderTest
{
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
            Check(ConfirmModel.Writes==1&&ConfirmModel.Closes==1,"successful common-store confirmation");
            foreach(int error in new[]{5,1016}) {
                ConfirmModel.Reset();ConfirmModel.WriteError=error;bool rejected=false;
                try{registry.Confirm();}catch(Win32Exception e){rejected=e.NativeErrorCode==error;}
                Check(rejected&&ConfirmModel.Writes==1&&ConfirmModel.Closes==1,"write or flush failure cannot confirm");
            }
            foreach(int error in new[]{1060,2,5}) {
                ConfirmModel.Reset();ConfirmModel.OpenError=error;bool rejected=false;
                try{registry.Confirm();}catch(Win32Exception e){rejected=e.NativeErrorCode==error;}
                Check(rejected&&ConfirmModel.Writes==0&&ConfirmModel.Closes==0,"open failure cannot create service or confirm");
            }
            Console.WriteLine("Monitor confirmation: {0} checks passed",checks);return 0;
        }catch(Exception e){Console.Error.WriteLine(e);return 1;}
    }
}
