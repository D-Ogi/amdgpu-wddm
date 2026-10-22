// Include the actual client. Only reply validation runs; no adapter is opened.
#define main unused_client_main
#include "dcn_latch.cpp"
#undef main
static int checks,failed;
#define CHECK(x) do { ++checks; if (!(x)) { ++failed; printf("FAIL line %d: %s\n",__LINE__,#x); } } while (0)
int main()
{
    for (unsigned long arm=0;arm<Bc250LatchArmCount;++arm) {
        BC250_ESCAPE_LATCH_PROBE good={};
        good.Magic=BC250_ESCAPE_MAGIC; good.Command=BC250_ESCAPE_LATCH_PROBE_COMMAND;
        good.Version=good.ExpectedVersion=BC250_KMD_VERSION; good.AbiVersion=BC250_LATCH_PROBE_ABI;
        good.Arm=good.Result.Arm=arm; good.Result.Restored=good.Result.BoundaryObserved=1;
        good.Result.Samples=4; good.Result.WritesAttempted=1;
        good.Result.AddressA=0x100000000ull; good.Result.AddressB=0x100010000ull;
        BC250_LATCH_SAMPLE* samples[]={&good.Result.Before,&good.Result.Armed,&good.Result.Boundary,&good.Result.AfterUnlock};
        for (auto s:samples) {s->ValidMask=(1ul<<Bc250LatchWordCount)-1; s->BracketsMatch=1;}
        good.Result.Before.Physical=good.Result.AddressA; good.Result.Before.LockClear=1;
        good.Result.AfterUnlock.Physical=good.Result.AddressB; good.Result.AfterUnlock.LockClear=1;
        CHECK(complete_reply(0,good,BC250_KMD_VERSION,arm));
        CHECK(!complete_reply((NTSTATUS)0xc0000001,good,BC250_KMD_VERSION,arm));
        CHECK(!complete_reply(1,good,BC250_KMD_VERSION,arm));
        CHECK(!complete_reply(0,good,BC250_KMD_VERSION+1,arm));
        CHECK(!complete_reply(0,good,BC250_KMD_VERSION,(arm+1)%Bc250LatchArmCount));
        // Every critical scalar must fail independently, not just one bundled
        // corrupt reply. No hardware conclusions are asserted by this fixture.
#define REJECT(field,value) do {auto d=good; d.field=value; CHECK(!complete_reply(0,d,BC250_KMD_VERSION,arm));} while (0)
        REJECT(Magic,0); REJECT(Command,0); REJECT(Version,0); REJECT(ExpectedVersion,0);
        REJECT(AbiVersion,0); REJECT(Arm,99); REJECT(Result.Arm,99); REJECT(Status,2); REJECT(NtStatus,1);
        REJECT(AbiVersion,1);
        REJECT(Result.Status,-1); REJECT(Result.UnlockStatus,-1); REJECT(Result.RestoreStatus,-1); REJECT(Result.EndStatus,-1);
        REJECT(Result.Restored,0); REJECT(Result.BoundaryObserved,0); REJECT(Result.Samples,3); REJECT(Result.WritesAttempted,0);
        REJECT(Result.AddressA,0); REJECT(Result.AddressB,0); REJECT(Result.AddressB,good.Result.AddressA);
        REJECT(Result.Before.ValidMask,0); REJECT(Result.Armed.ValidMask,0); REJECT(Result.Boundary.ValidMask,0); REJECT(Result.AfterUnlock.ValidMask,0);
        REJECT(Result.Before.BracketsMatch,0); REJECT(Result.Armed.BracketsMatch,0); REJECT(Result.Boundary.BracketsMatch,0); REJECT(Result.AfterUnlock.BracketsMatch,0);
        REJECT(Result.Before.Physical,good.Result.AddressB); REJECT(Result.AfterUnlock.Physical,good.Result.AddressA);
        REJECT(Result.Before.LockClear,0); REJECT(Result.AfterUnlock.LockClear,0);
#undef REJECT
    }
    printf("Latch client: %d checks, %d failures; adapter not opened\n",checks,failed);
    return failed?1:0;
}
