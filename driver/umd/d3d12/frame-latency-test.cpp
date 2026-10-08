// SPDX-License-Identifier: MIT
// Host gate of the D3D12 shell's MaxFrameLatency gate (frame-latency.h): the ring, the poll loop and the
// counters. No GPU and no runtime: a substitute progress source answers the gate.
#include "frame-latency.h"
#include <cstdio>
#include <cstring>
#include <share.h>
#include <string>
#include <vector>

namespace {
// The witness lines must reach the AMDGPU_WDDM_LOG sink of the shell, not the debugger alone: a lab trial reads
// that file and has no debugger. stdio-log.h reads the switch once, on its first use, so main() sets it first.
constexpr char kLogName[]="frame-latency-test-witness.log";
int failures=0;
void check(bool ok,const char* what) {
    std::printf("%s %s\n",ok?"PASS":"FAIL",what);
    if(!ok)++failures;
}

// A substitute of the device progress source (device-progress.h). Every snapshot carries one mark whose value
// counts up, so a test can name the frame a gate asked about.
struct FakeProgress {
    unsigned snapshots{};
    unsigned asked{};                       // retired() calls
    uint64_t last_gate{};                   // the mark of the snapshot the last gate asked about
    std::vector<uint64_t> gates;            // the mark of every gate, in order
    unsigned false_answers{};               // answer false this many times, then true
    bool answer{true};
    bool complete{true};
    uint64_t next_mark{1};
    static void snapshot(void* owner,native12::ProgressSnapshot* out) noexcept {
        auto& self=*static_cast<FakeProgress*>(owner);
        ++self.snapshots;
        *out={};
        out->complete=self.complete;
        out->count=1;
        out->marks[0]={7,self.next_mark++};
    }
    static bool retired(void* owner,const native12::ProgressSnapshot* snapshot) noexcept {
        auto& self=*static_cast<FakeProgress*>(owner);
        ++self.asked;
        self.last_gate=snapshot->marks[0].value;
        if(self.asked==1 || self.gates.empty() || self.gates.back()!=self.last_gate)
            self.gates.push_back(self.last_gate);
        if(self.false_answers){--self.false_answers;return false;}
        return self.answer;
    }
    native12::ProgressSource source() noexcept {return {this,snapshot,retired};}
};

void test_slot() {
    using native12::latency_slot;
    check(latency_slot(0,0)==0,"latency_slot: no setting names slot 0");
    // Latency 1: every frame reads the slot the frame before it wrote.
    check(latency_slot(0,1)==0 && latency_slot(1,1)==0 && latency_slot(9,1)==0,"latency_slot: latency 1 uses one slot");
    // Latency k: the slot frame N reads is the one frame N-k wrote, and no frame between them touched it.
    for(uint32_t k=1;k<=native12::kMaxFrameLatencySlots;++k){
        bool ok=true;
        for(uint64_t n=k;n<40;++n){
            if(latency_slot(n,k)!=latency_slot(n-k,k))ok=false;
            for(uint64_t between=n-k+1;between<n;++between)
                if(latency_slot(between,k)==latency_slot(n,k))ok=false;
        }
        char what[96];std::snprintf(what,sizeof(what),"latency_slot: latency %u reads exactly frame N-%u",k,k);
        check(ok,what);
    }
}

void test_wait_loop() {
    using native12::latency_wait;
    // Retired at the first check: one poll, no clock read and no idle call. This is the application that keeps
    // its own frames in flight below the setting, the DXGI frame-latency waitable among them.
    unsigned clock=0,idles=0;
    auto now=[&]() noexcept {++clock;return uint64_t{0};};
    auto idle=[&](uint64_t) noexcept {++idles;};
    native12::LatencyWait out=latency_wait([]() noexcept {return true;},now,idle,1000);
    check(out.polls==1u && out.retired && clock==0u && idles==0u,"latency_wait: a retired gate costs one check");

    // Retired at the fourth check: four polls, three idle calls.
    clock=0;idles=0;
    unsigned left=3;
    out=latency_wait([&]() noexcept {if(left){--left;return false;}return true;},now,idle,1000);
    check(out.polls==4u && out.retired && idles==3u,"latency_wait: polls until the gate is retired");

    // Never retired: the loop ends on the budget, says so, and makes no claim of retirement.
    uint64_t fake_now=0;
    clock=0;idles=0;
    auto ticking=[&]() noexcept {++clock;fake_now+=100;return fake_now;};
    out=latency_wait([]() noexcept {return false;},ticking,idle,1000);
    check(!out.retired && out.polls>1u,"latency_wait: an unretired gate ends on the budget");
    check(fake_now>=1000u && idles==out.polls-1u,"latency_wait: one idle call between two checks");

    // A budget of zero still makes the second check, so the loop can never exit on the first idle alone.
    fake_now=0;idles=0;
    out=latency_wait([]() noexcept {return false;},ticking,idle,0);
    check(out.polls==2u && !out.retired,"latency_wait: budget 0 makes one more check");
}

void test_ring() {
    // No setting: the source is never asked, which is the behaviour of the releases before this change.
    {
        FakeProgress fake;native12::FrameLatency gate;
        for(int i=0;i<8;++i)gate.frame(fake.source(),0);
        check(fake.snapshots==0u && fake.asked==0u && gate.frames()==0u,"FrameLatency: no setting takes nothing");
    }
    // A source that cannot answer (no engine): the same.
    {
        native12::FrameLatency gate;native12::ProgressSource empty{};
        gate.frame(empty,2);
        check(gate.frames()==0u,"FrameLatency: an unusable source takes nothing");
    }
    // Latency 1: frame 0 has nothing behind it, frame 1 asks about frame 0, frame 2 about frame 1.
    {
        FakeProgress fake;native12::FrameLatency gate;
        gate.frame(fake.source(),1);
        check(fake.snapshots==1u && fake.asked==0u,"FrameLatency: the first frame waits for nothing");
        gate.frame(fake.source(),1);
        check(fake.asked==1u && fake.last_gate==1u,"FrameLatency: latency 1 gates on the frame before");
        gate.frame(fake.source(),1);
        check(fake.asked==2u && fake.last_gate==2u,"FrameLatency: the gate follows the frames");
        check(gate.frames()==3u && gate.waits()==0u && gate.timeouts()==0u && gate.skipped()==0u,
              "FrameLatency: a retired gate counts no wait");
    }
    // Latency 3: the first three frames pass, then frame 3 gates on frame 0, frame 4 on frame 1 and so on.
    {
        FakeProgress fake;native12::FrameLatency gate;
        for(int i=0;i<7;++i)gate.frame(fake.source(),3);
        const std::vector<uint64_t> want{1,2,3,4};
        check(fake.gates==want,"FrameLatency: latency 3 gates on frame N-3 in order");
        check(gate.frames()==7u && fake.snapshots==7u,"FrameLatency: one snapshot per present");
    }
    // A value above the setting's range is clamped to the ring, and still gates in order.
    {
        FakeProgress fake;native12::FrameLatency gate;
        for(int i=0;i<6;++i)gate.frame(fake.source(),9);
        const std::vector<uint64_t> want{1,2,3};
        check(fake.gates==want,"FrameLatency: a latency above the range is clamped to the ring");
    }
    // A latency that changes starts a new ring, so a gate never reads a frame of the old modulus.
    {
        FakeProgress fake;native12::FrameLatency gate;
        for(int i=0;i<4;++i)gate.frame(fake.source(),3);   // frame 3 gates on frame 0 (mark 1)
        check(fake.gates==std::vector<uint64_t>{1},"FrameLatency: the ring gates before the change");
        gate.frame(fake.source(),1);                       // a new ring: nothing behind this frame
        check(fake.asked==1u,"FrameLatency: a changed latency starts a new ring");
        gate.frame(fake.source(),1);                       // now the frame before it, which took mark 5
        const std::vector<uint64_t> want{1,5};
        check(fake.gates==want,"FrameLatency: the new ring gates on its own frames");
    }
    // A gate that is not retired yet makes one wait and then goes on.
    {
        FakeProgress fake;fake.false_answers=3;
        native12::FrameLatency gate;
        gate.frame(fake.source(),1);
        gate.frame(fake.source(),1,20000);
        check(gate.waits()==1u && gate.timeouts()==0u && gate.polls()>=4u,"FrameLatency: a busy gate waits once");
    }
    // A gate that never retires ends on the budget, counts a timeout and lets the frame through.
    {
        FakeProgress fake;fake.answer=false;
        native12::FrameLatency gate;
        gate.frame(fake.source(),1);
        const uint64_t before=native12::latency_now_us();
        gate.frame(fake.source(),1,4000);
        const uint64_t spent=native12::latency_now_us()-before;
        check(gate.timeouts()==1u && gate.waits()==1u,"FrameLatency: an unretired gate counts a timeout");
        // The given budget, not the one second default: the loop checks the clock after every poll, so the only
        // overshoot is one sleep. 500 ms leaves room for a loaded machine and still fails a run that waited the
        // default budget.
        check(spent<500000u,"FrameLatency: the timeout keeps the present inside the budget it was given");
        check(!gate.stopped(),"FrameLatency: one timeout does not stop the gate");
        std::printf("note: the 4 ms budget took %llu us\n",static_cast<unsigned long long>(spent));
    }
    // A fence that never retires: the gate gives up kFrameLatencyGiveUps times and then stops for this device,
    // instead of spending a budget in every Present for as long as the application runs.
    {
        FakeProgress fake;fake.answer=false;
        native12::FrameLatency gate;
        for(int i=0;i<40 && !gate.stopped();++i)gate.frame(fake.source(),1,1000);
        check(gate.stopped() && gate.timeouts()==native12::kFrameLatencyGiveUps,
              "FrameLatency: the gate stops after the given number of frames gave up");
        const unsigned asked=fake.asked,snapshots=fake.snapshots;
        for(int i=0;i<5;++i)gate.frame(fake.source(),1,1000);
        check(fake.asked==asked && fake.snapshots==snapshots,
              "FrameLatency: a stopped gate takes no snapshot and asks nothing");
    }
    // An incomplete snapshot proves nothing, so it is not a gate: no wait, and the frame is counted as skipped.
    {
        FakeProgress fake;fake.complete=false;fake.answer=false;
        native12::FrameLatency gate;
        gate.frame(fake.source(),1);
        gate.frame(fake.source(),1,4000);
        check(fake.asked==0u && gate.skipped()==1u && gate.waits()==0u,
              "FrameLatency: an incomplete snapshot is no gate");
    }
    // The real clock moves forward and never goes back.
    {
        const uint64_t a=native12::latency_now_us();
        const uint64_t b=native12::latency_now_us();
        check(b>=a && a>0,"latency_now_us: a monotonic microsecond clock");
    }
}

// Every witness line of the gate, read back from the log sink of the shell. A lab trial reads that file and has
// no debugger. The notes are bounded to the first event of each kind in the process, so the tests above produced
// exactly one of each.
void test_witness_lines() {
    std::string text;
    // _fsopen, not fopen_s: the shell's sink still holds this file open for append, and fopen_s asks for
    // exclusive access.
    FILE* const file=_fsopen(kLogName,"rb",_SH_DENYNO);
    check(file!=nullptr,"witness: the log file of this process opens for reading");
    if(file){
        char buffer[4096];
        for(size_t read=0;(read=std::fread(buffer,1,sizeof(buffer),file))!=0;)text.append(buffer,read);
        std::fclose(file);
    }
    const auto holds=[&](const char* what){return text.find(what)!=std::string::npos;};
    check(holds("the present waited after"),"witness: the wait reaches the log sink");
    check(holds("the present gave up after"),"witness: the timeout reaches the log sink");
    check(holds("the present proved nothing"),"witness: an unproven gate reaches the log sink");
    check(holds("the gate stopped"),"witness: the stop reaches the log sink");
    check(holds("BC250 MaxFrameLatency=1:"),"witness: every line names the setting and its value");
    // Bounded: one line of each of the four kinds, whatever the number of events.
    size_t lines=0;
    for(size_t at=text.find("BC250 MaxFrameLatency=");at!=std::string::npos;at=text.find("BC250 MaxFrameLatency=",at+1))
        ++lines;
    check(lines==4,"witness: four lines in the whole process, one of each kind");
    if(lines!=4)std::printf("note: the log holds %zu lines:\n%s",lines,text.c_str());
}
}

int main() {
    // Before anything logs: stdio-log.h reads AMDGPU_WDDM_LOG once, on the first line it prints.
    DeleteFileA(kLogName);
    const std::string sink=std::string("file:")+kLogName;
    if(!SetEnvironmentVariableA("AMDGPU_WDDM_LOG",sink.c_str()))
        check(false,"witness: the log switch is set for this process");
    test_slot();
    test_wait_loop();
    test_ring();
    test_witness_lines();
    std::printf("%s frame-latency: %d failures\n",failures?"FAIL":"PASS",failures);
    return failures?1:0;
}
