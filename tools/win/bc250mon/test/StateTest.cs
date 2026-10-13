using System;
using System.Collections.Generic;
using System.ComponentModel;

namespace Bc250Mon
{
    static class StateTest
    {
        sealed class FakeSession : IDriverStateSession
        {
            public Dictionary<string, DriverStateValue> Raw = new Dictionary<string, DriverStateValue>();
            public Dictionary<string, DriverStateValue> Effective = new Dictionary<string, DriverStateValue>();
            public int EffectiveReads, Writes, Disposed, WriteError;
            public IEnumerable<DriverStateSpec> Catalog { get { return new[] {
                new DriverStateSpec { Path="",Name="CuMode",Kind=0,Type=4,MaxBytes=4 },
                new DriverStateSpec { Path="",Name="DpmMode",Kind=0,Type=4,MaxBytes=4 },
                new DriverStateSpec { Path="",Name="LastStage",Kind=1,Type=4,MaxBytes=4 } }; } }
            public DriverStateValue Read(string path,string name,bool effective)
            {
                if(effective)EffectiveReads++;
                DriverStateValue value;
                return (effective?Effective:Raw).TryGetValue(path+"/"+name,out value)?value:Absent();
            }
            public void WriteDword(string path,string name,uint value)
            {
                Check(path==""&&name=="UnconfirmedStarts"&&value==0,"guard write identity");Writes++;
                if(WriteError!=0)throw new Win32Exception(WriteError);
            }
            public void Dispose(){Disposed++;}
        }
        static int checks;
        static void Check(bool value,string name){checks++;if(!value)throw new Exception(name);}
        static DriverStateValue Absent(){return new DriverStateValue{Status=1,Error=2};}
        static DriverStateValue Good(object data,uint source=1){return new DriverStateValue{Data=data,Type=data is int?4u:1u,Source=source};}
        static FakeSession Fresh()
        {
            var session=new FakeSession();
            session.Effective["/CuMode"]=Good(24,2);
            session.Raw["/DpmMode"]=Good(0);
            session.Effective["/DpmMode"]=Good(0);
            DriverState.OpenSession=write=>session;
            return session;
        }
        static void Bulk()
        {
            var session=Fresh();var values=RegistryDwords.Read();
            Check(values.Present&&values.Error==null,"state present");
            Check(values.Get("CuMode")==24&&!values.Overrides.ContainsKey("CuMode")&&values.Sources["CuMode"]=="factory","factory differs from absent override");
            Check(values.Get("DpmMode")==0&&values.Overrides["DpmMode"]==0&&values.Sources["DpmMode"]=="override","explicit zero retained");
            Check(session.EffectiveReads==2&&session.Disposed==1,"only settings resolved, handle closed");
            foreach(uint status in new uint[]{3,5,6,7,8}) {
                session=Fresh();session.Raw["/CuMode"]=new DriverStateValue{Status=status,Error=2};
                values=RegistryDwords.Read();
                Check(values.Get("CuMode")==null&&values.Errors.ContainsKey("CuMode")&&session.EffectiveReads==1,"raw error cannot authorize fallback "+status);
                session=Fresh();session.Effective["/CuMode"]=new DriverStateValue{Status=status,Error=2};
                values=RegistryDwords.Read();
                Check(values.Get("CuMode")==null&&values.Errors.ContainsKey("CuMode"),"factory error cannot authorize code default "+status);
            }
            DriverState.OpenSession=write=>{throw new Win32Exception(2);};values=RegistryDwords.Read();
            Check(!values.Present&&values.Error!=null&&values.Get("CuMode")==null,"missing root is error, never stock fallback");
        }
        static void Startup()
        {
            var registry=new KmdRegistry(null);
            Fresh();var snapshot=registry.Read();
            Check(snapshot.Installed&&!snapshot.Started&&snapshot.Error==null,"initialized state is not proof of startup");
            var session=Fresh();session.Raw["/LastStage"]=Good(91);session.Raw["/UnconfirmedStarts"]=Good(2);
            session.Raw["StartRecord/Result"]=Good("failed");session.Raw["StartRecord/Step"]=Good("test-step");
            session.Raw["StartRecord/Status"]=Good(unchecked((int)0xc0000001));session.Raw["StartRecord/Detail"]=Good("failure detail");
            snapshot=registry.Read();
            Check(snapshot.Started&&snapshot.LastStage==91&&snapshot.UnconfirmedStarts==2&&snapshot.StartResult=="failed"&&snapshot.StartDetail=="failure detail","postmortem startup record with no GPU calls");
            foreach(int error in new[]{2,5,21,1306}) {
                DriverState.OpenSession=write=>{throw new Win32Exception(error);};snapshot=registry.Read();
                Check(snapshot.Error!=null&&registry.Summary().Contains("unreadable"),"root/factory errors not uninstalled "+error);
            }
            DriverState.OpenSession=write=>{throw new Win32Exception(1060);};snapshot=registry.Read();
            Check(!snapshot.Installed&&snapshot.Error==null&&registry.Summary().Contains("not installed"),"only SCM1060 means uninstalled");
            session=Fresh();session.Raw["/LastStage"]=new DriverStateValue{Status=8,Error=2};snapshot=registry.Read();
            Check(snapshot.Error!=null&&!snapshot.Started,"incomplete startup record does not become absence");
            bool refused=false;try{new KmdRegistry("HKCU\\fake");}catch(ArgumentException){refused=true;}
            Check(refused,"no arbitrary registry fallback");
            session=Fresh();registry.Confirm();Check(session.Writes==1&&session.Disposed==1,"confirmation delegated to common store");
            foreach(int error in new[]{5,1016}) {
                session=Fresh();session.WriteError=error;refused=false;
                try{registry.Confirm();}catch(Win32Exception e){refused=e.NativeErrorCode==error;}
                Check(refused&&session.Disposed==1,"checked persistence failure visible "+error);
            }
        }
        static int Main()
        {
            try{Bulk();Startup();Console.WriteLine("Overlay shared state: {0} checks passed",checks);return 0;}
            catch(Exception e){Console.Error.WriteLine(e);return 1;}
        }
    }
}
