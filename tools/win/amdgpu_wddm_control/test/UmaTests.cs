using System;
using System.IO;
using AmdgpuWddmControl;
static partial class UnitTests
{
    static byte[] UmaBlock(uint mib)
    {
        var b = new byte[28];
        Array.Copy(BitConverter.GetBytes(0x42435041u), b, 4);
        b[26] = (byte)mib; b[27] = (byte)(mib >> 8);
        ushort sum = (ushort)(b[26] + b[27]);
        Array.Copy(BitConverter.GetBytes(sum), 0, b, 4, 2);
        return b;
    }
    static void UmaTests(string root)
    {
        var s = new UmaState { Supported=true, ProviderId=1, AllowedFirstMiB=8192, AllowedSecondMiB=12288, NeedsRestart=1, ActiveValid = true, ReadValid = true, WriteAllowed = true, ActiveBytes = 8ul << 30, RequestedMiB = 8192 };
        s.ObservedBlock=UmaBlock(8192);
        s.Supported=false;
        Check(!UmaSetting.Visible(s) && !UmaSetting.CanSet(s,12288),"Missing supported flag hides valid-looking provider and forbids action");
        s.Supported=true;
        Check(UmaSetting.CanSet(s, 12288), "UMA allows measured 12 GiB choice");
        Check(!UmaSetting.CanSet(s, 8192), "UMA unchanged request does not write");
        Check(!UmaSetting.CanSet(s, 14336) && !UmaSetting.CanSet(s, 0), "UMA rejects unoffered sizes");
        Check(!UmaSetting.Pending(s), "UMA active eight equals requested eight");
        s.RequestedMiB = 12288; s.ObservedBlock = UmaBlock(12288);
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
        word(0,KmdReply.Magic); word(4,32); word(16,2); word(24,17); word(80,1);word(84,8192);word(88,12288);word(92,1); word(28,1);
        Array.Copy(BitConverter.GetBytes(8ul<<30),0,reply,32,8);
        var unavailable = UmaSetting.Parse(reply,32);
        Check(unavailable.ActiveValid && !unavailable.ReadValid && !UmaSetting.Pending(unavailable), "UMA unavailable transport exposes active only");
        Check(!UmaSetting.CanSet(unavailable,12288) && !UmaSetting.CanRestore(unavailable), "UMA unavailable transport cannot write");
        Check(UmaSetting.Request(0,0,null).Length==48, "UMA exact query ABI length");
        bool refused=false;try{UmaSetting.Request(1,12288,unavailable);}catch(ArgumentException){refused=true;}
        Check(refused,"UMA native request builder rejects unavailable mutation");
        s.ReadValid=true;s.RequestedMiB=8192;s.ObservedBlock=UmaBlock(8192);
        var request=UmaSetting.Request(1,12288,s);
        Check(BitConverter.ToUInt32(request,0)==48 && BitConverter.ToUInt32(request,4)==1 &&
              BitConverter.ToUInt32(request,8)==12288 && request[43]==0x20 &&
              BitConverter.ToUInt32(request,12)==0 && BitConverter.ToUInt32(request,44)==0,"UMA mutation bounded expected block and zero reserved fields");
        word(16,1);refused=false;try{UmaSetting.Parse(reply,32);}catch(FormatException){refused=true;}
        Check(refused,"UMA incompatible reply ABI rejected");
        var done=new UmaState { Supported=true, ProviderId=1, AllowedFirstMiB=8192, AllowedSecondMiB=12288, NeedsRestart=1,ReadValid=true,Operation=2,RequestedMiB=8192,ResultCode=0,BackupAvailable=false};
        done.ObservedBlock=UmaBlock(8192);
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
        word(16,2);word(8,1);refused=false;try{UmaSetting.Parse(reply,32);}catch(FormatException){refused=true;}Check(refused,"UMA failed header status rejected");
        word(8,0);word(12,0xc0000001);refused=false;try{UmaSetting.Parse(reply,32);}catch(FormatException){refused=true;}Check(refused,"UMA failed NTSTATUS rejected");
        word(12,0);reply[127]=1;refused=false;try{UmaSetting.Parse(reply,32);}catch(FormatException){refused=true;}Check(refused,"UMA reserved tail rejected");
        reply[127]=0;word(24,7);refused=false;try{UmaSetting.Parse(reply,32);}catch(FormatException){refused=true;}Check(refused,"UMA inconsistent write reason rejected");
        done.ReadValid=true;done.Reason=1;Check(!UmaSetting.Verified(done,2,8192),"UMA unavailable result never verified");
        Array.Clear(reply,0,reply.Length);word(0,KmdReply.Magic);word(4,32);word(16,2);word(28,2);
        var foreign=UmaSetting.Parse(reply,32);
        Check(!UmaSetting.Visible(foreign) && !UmaSetting.CanSet(foreign,12288) && !UmaSetting.CanRestore(foreign),"Foreign board hides reservation card and cannot act");
        word(20,1);refused=false;try{UmaSetting.Parse(reply,32);}catch(FormatException){refused=true;}
        Check(refused,"Unsupported DONE mutation reply rejected");word(20,0);
        word(28,3);Check(!UmaSetting.Visible(UmaSetting.Parse(reply,32)),"Unknown firmware hides reservation card");
        word(80,1);refused=false;try{UmaSetting.Parse(reply,32);}catch(FormatException){refused=true;}Check(refused,"Unsupported flag and provider mismatch rejected");
        word(24,16);word(84,8192);word(88,12288);word(92,1);word(28,1);word(80,2);
        refused=false;try{UmaSetting.Parse(reply,32);}catch(FormatException){refused=true;}Check(refused,"Unknown provider rejected");
        word(80,1);word(84,4096);refused=false;try{UmaSetting.Parse(reply,32);}catch(FormatException){refused=true;}Check(refused,"Unrecognized board choices rejected");
        word(84,8192);word(92,0);refused=false;try{UmaSetting.Parse(reply,32);}catch(FormatException){refused=true;}Check(refused,"Missing restart capability rejected");
        s.PreviousMiB=8192;token=UmaSetting.ConfirmationToken(s);s.ProviderId=2;
        Check(!UmaSetting.Confirmed(s,token) && !UmaSetting.CanSet(s,12288),"Provider identity bound to confirmation and mutation");s.ProviderId=1;
        token=UmaSetting.ConfirmationToken(s);s.AllowedFirstMiB=4096;Check(!UmaSetting.Confirmed(s,token),"Allowed values bound to confirmation");s.AllowedFirstMiB=8192;
        token=UmaSetting.ConfirmationToken(s);s.NeedsRestart=0;Check(!UmaSetting.Confirmed(s,token),"Restart capability bound to confirmation");
        s.NeedsRestart=1;s.Reason=6;
        Check(!UmaSetting.CanSet(s,12288) && !UmaSetting.CanRestore(s),"Unknown state locks both writes");
        done.ReadValid=true;done.Reason=0;done.ResultCode=0;
        done.ObservedBlock[4]++;
        Check(!UmaSetting.Verified(done,2,8192),"Bad checksum cannot report write success");
        done.ObservedBlock=UmaBlock(12288);
        Check(!UmaSetting.Verified(done,2,8192),"Correct checksum with wrong observed target refused");
        var app=Path.Combine(root,"tools/win/amdgpu_wddm_control/src");
        var graphics=File.ReadAllText(Path.Combine(app,"MainForm.Games.cs"));
        Check(graphics.Contains("if (UmaSetting.Visible(_uma)) p.Controls.Add(BuildUmaCard(width));") &&
              graphics.Contains("else p.Controls.Add(BuildReadOnlyMemoryCard(width));"),"GUI hides board control and selects read-only fallback by capability");
        var card=File.ReadAllText(Path.Combine(app,"MainForm.Uma.cs"));
        Check(card.Contains("_vram.Dedicated") && card.Contains("uma.fallback.readonly"),"Other devices show existing segment memory statistics");
        Check(card.Contains("if (code == 0) OfferRestart();"),"Restart offer follows verified success only");
        var program=File.ReadAllText(Path.Combine(app,"Program.cs"));
        Check(program.Contains("--bc250-board-memory-action") && !program.Contains("--uma-action"),"Only board-specific public action verb accepted");
        var native=File.ReadAllText(Path.Combine(app,"Native.cs"));
        Check(native.Contains("Bc250BoardMemory") && !native.Contains("Bc250Uma") && native.Contains("BoardMemoryQuery()"),"Public native wrappers identify board memory contract");
        Check(!UmaSetting.CanSet(null, 8192) && !UmaSetting.CanRestore(null), "UMA missing driver refuses mutation");
    }
}
