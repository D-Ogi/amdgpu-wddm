using System;
using AmdgpuWddmControl;
static partial class UnitTests
{
    static void UmaTests()
    {
        var s = new UmaState { ActiveValid = true, ReadValid = true, WriteAllowed = true, ActiveBytes = 8ul << 30, RequestedMiB = 8192 };
        Check(UmaSetting.CanSet(s, 12288), "UMA allows measured 12 GiB choice");
        Check(!UmaSetting.CanSet(s, 8192), "UMA unchanged request does not write");
        Check(!UmaSetting.CanSet(s, 14336) && !UmaSetting.CanSet(s, 0), "UMA rejects unoffered sizes");
        Check(!UmaSetting.Pending(s), "UMA active eight equals requested eight");
        s.RequestedMiB = 12288;
        Check(UmaSetting.Pending(s) && s.ActiveBytes == (8ul << 30), "UMA request never becomes active before restart");
        Check(!UmaSetting.CanRestore(s), "UMA no backup cannot restore");
        s.BackupAvailable = true; s.PreviousMiB=8192;
        Check(UmaSetting.CanRestore(s), "UMA bounded driver backup can restore");
        s.WriteAllowed = false;
        Check(!UmaSetting.CanSet(s, 8192) && !UmaSetting.CanRestore(s), "UMA unsupported board is read only");
        s.WriteAllowed = true; s.ReadValid = false;
        Check(!UmaSetting.CanSet(s, 8192) && !UmaSetting.CanRestore(s) && !UmaSetting.Pending(s), "UMA invalid block refuses mutation and pending claim");
        var reply = new byte[128];
        Action<int,uint> word = (offset,value) => Array.Copy(BitConverter.GetBytes(value), 0, reply, offset, 4);
        word(0,KmdReply.Magic); word(4,32); word(16,1); word(24,1); word(28,1);
        Array.Copy(BitConverter.GetBytes(8ul<<30),0,reply,32,8);
        var unavailable = UmaSetting.Parse(reply,32);
        Check(unavailable.ActiveValid && !unavailable.ReadValid && !UmaSetting.Pending(unavailable), "UMA unavailable transport exposes active only");
        Check(!UmaSetting.CanSet(unavailable,12288) && !UmaSetting.CanRestore(unavailable), "UMA unavailable transport cannot write");
        Check(UmaSetting.Request(0,0,null).Length==48, "UMA exact query ABI length");
        bool refused=false;try{UmaSetting.Request(1,12288,unavailable);}catch(ArgumentException){refused=true;}
        Check(refused,"UMA native request builder rejects unavailable mutation");
        s.ReadValid=true;s.RequestedMiB=8192;s.ObservedBlock[27]=0x7a;
        var request=UmaSetting.Request(1,12288,s);
        Check(BitConverter.ToUInt32(request,0)==48 && BitConverter.ToUInt32(request,4)==1 &&
              BitConverter.ToUInt32(request,8)==12288 && request[43]==0x7a &&
              BitConverter.ToUInt32(request,12)==0 && BitConverter.ToUInt32(request,44)==0,"UMA mutation bounded expected block and zero reserved fields");
        word(16,2);refused=false;try{UmaSetting.Parse(reply,32);}catch(FormatException){refused=true;}
        Check(refused,"UMA incompatible reply ABI rejected");
        var done=new UmaState {ReadValid=true,Operation=2,RequestedMiB=8192,ResultCode=0,BackupAvailable=false};
        Check(UmaSetting.Verified(done,2,8192),"UMA verified restore can consume backup");
        done.ResultCode=-5;Check(!UmaSetting.Verified(done,2,8192),"UMA rolled-back write is failure");
        done.ResultCode=1;Check(UmaSetting.Verified(done,2,8192),"UMA verified no-change is success");
        Check(!UmaSetting.Verified(done,1,8192) && !UmaSetting.Verified(done,2,12288),"UMA wrong operation or target cannot claim success");
        done.ReadValid=false;Check(!UmaSetting.Verified(done,2,8192),"UMA no readback cannot claim success");
        s.Reason=1;Check(!UmaSetting.CanSet(s,12288) && !UmaSetting.CanRestore(s),"UMA unavailable reason overrides contradictory write flags");
        var token=UmaSetting.ConfirmationToken(s);
        Check(UmaSetting.Confirmed(s,token),"UMA unchanged confirmed state accepted");
        s.ObservedBlock[0]++;Check(!UmaSetting.Confirmed(s,token),"UMA changed CMOS after confirmation refused");s.ObservedBlock[0]--;
        s.PreviousMiB=12288;Check(!UmaSetting.Confirmed(s,token),"UMA changed restore backup after confirmation refused");
        s.Reason=0;s.PreviousMiB=4096;Check(!UmaSetting.CanRestore(s),"UMA unoffered restore size refused");
        word(16,1);word(8,1);refused=false;try{UmaSetting.Parse(reply,32);}catch(FormatException){refused=true;}Check(refused,"UMA failed header status rejected");
        word(8,0);word(12,0xc0000001);refused=false;try{UmaSetting.Parse(reply,32);}catch(FormatException){refused=true;}Check(refused,"UMA failed NTSTATUS rejected");
        word(12,0);reply[127]=1;refused=false;try{UmaSetting.Parse(reply,32);}catch(FormatException){refused=true;}Check(refused,"UMA reserved tail rejected");
        reply[127]=0;word(24,7);refused=false;try{UmaSetting.Parse(reply,32);}catch(FormatException){refused=true;}Check(refused,"UMA inconsistent write reason rejected");
        done.ReadValid=true;done.Reason=1;Check(!UmaSetting.Verified(done,2,8192),"UMA unavailable result never verified");
        Check(!UmaSetting.CanSet(null, 8192) && !UmaSetting.CanRestore(null), "UMA missing driver refuses mutation");
    }
}
