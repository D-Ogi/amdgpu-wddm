// SPDX-License-Identifier: MIT
#pragma once
#include "device-progress.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
namespace native12 {
// The per-application MaxFrameLatency of this shell's Present (docs/design/per-app-graphics-settings.md).
// Before a Present returns, the shell waits until the GPU finished the frame that is N places back. The
// application then cannot queue more than N frames ahead of the GPU, which is what the setting promises.
//
// What proves that a frame finished: a device progress snapshot (device-progress.h), taken at the Present of
// that frame. The snapshot names every monitored fence of the device that the GPU has not reached yet, so
// everything that every context of the device submitted before that Present has retired once the snapshot is
// retired. The shell owns no fence of its own in the D3D12 present path, because the runtime makes the kernel
// present call, so this snapshot is the shell's fence. It covers the engine's queues and the ICD's internal
// contexts alike, and a device that is idle between frames gives an empty snapshot, which is retired at once.
//
// The gate is a ceiling and never a floor. The first check costs no wait at all, so an application that already
// keeps fewer frames in flight, with its own fences or with the frame-latency waitable object of its swap chain,
// finds the gate retired and waits for nothing. The shell adds no second wait to such an application.
//
// Nothing happens when the setting is absent (latency 0). That is the behaviour of every release up to b23.
inline constexpr uint32_t kMaxFrameLatencySlots=3;      // the setting's range is 1-3 (app-settings-core.h)
inline constexpr uint64_t kFrameLatencyBudgetUs=1000000;// the wait gives up after this, and never fails a Present
inline constexpr uint64_t kFrameLatencySpins=8;         // polls that spin before the first sleep
inline constexpr uint32_t kFrameLatencySleepUs=250;     // one sleep of the poll loop

// The ring slot of frame `frame` in a ring of `latency` slots. The slot holds frame `frame-latency`, which the
// gate reads, and then takes this frame. A ring of exactly `latency` slots is enough because the read comes
// first: with latency 3, frame 3 reads slot 0, which frame 0 wrote.
inline unsigned latency_slot(uint64_t frame,uint32_t latency) noexcept {
    return latency?static_cast<unsigned>(frame%latency):0u;
}
struct LatencyWait {
    uint64_t polls{};       // checks made, the first one included
    bool retired{};         // false: the budget ran out and the frame goes on
};
// Polls `retired` until it answers true or `budget_us` of `now` passed. `now` gives microseconds and is read
// only after the first check, so a gate that is retired already costs one check and no clock read. `idle(poll)`
// gives the processor up between checks. The caller decides what a lost budget means.
template<class Retired,class Now,class Idle>
LatencyWait latency_wait(Retired&& retired,Now&& now,Idle&& idle,uint64_t budget_us) noexcept {
    LatencyWait out{1,retired()};
    if(out.retired)return out;
    const uint64_t start=now();
    for(;;){
        idle(out.polls);
        ++out.polls;
        out.retired=retired();
        if(out.retired || now()-start>=budget_us)return out;
    }
}
// Microseconds of the performance counter, without an overflow of the tick multiplication.
inline uint64_t latency_now_us() noexcept {
    static const uint64_t frequency=[]() noexcept {
        LARGE_INTEGER f{};QueryPerformanceFrequency(&f);
        return f.QuadPart>0?static_cast<uint64_t>(f.QuadPart):1ull;
    }();
    LARGE_INTEGER n{};QueryPerformanceCounter(&n);
    const uint64_t ticks=n.QuadPart>0?static_cast<uint64_t>(n.QuadPart):0ull;
    return ticks/frequency*1000000ull+ticks%frequency*1000000ull/frequency;
}
// The wait between two checks: a short spin first, because a frame that is nearly done arrives within
// microseconds, then a sleep on a high-resolution timer. Sleep(1) is the fallback when the timer is absent.
inline void latency_idle(uint64_t poll,HANDLE timer) noexcept {
    if(poll<kFrameLatencySpins){for(unsigned i=0;i<64;++i)YieldProcessor();return;}
    if(timer){
        LARGE_INTEGER due{};due.QuadPart=-static_cast<LONGLONG>(kFrameLatencySleepUs)*10;  // 100 ns units
        if(SetWaitableTimerEx(timer,&due,0,nullptr,nullptr,nullptr,0)){
            WaitForSingleObject(timer,INFINITE);return;
        }
    }
    Sleep(1);
}
// One ring per device, as the frame rate limit keeps one clock per device. A device that presents on several
// queues shares this ring: the setting is the application's, not one swap chain's. frame() takes its lock for
// the ring alone and waits outside it, so a present of another queue is never held behind this wait. The
// caller must also be outside the device's queue domain (device-state.h, QueueDomainScope).
class FrameLatency final {
public:
    FrameLatency() noexcept=default;
    ~FrameLatency() {if(timer_)CloseHandle(timer_);}
    FrameLatency(const FrameLatency&)=delete;
    FrameLatency& operator=(const FrameLatency&)=delete;
    // One Present. latency 0 or a source that cannot answer: nothing is taken and nothing waits.
    void frame(const ProgressSource& source,uint32_t latency,
               uint64_t budget_us=kFrameLatencyBudgetUs) noexcept {
        if(!latency || !source.usable())return;
        if(latency>kMaxFrameLatencySlots)latency=kMaxFrameLatencySlots;
        // Outside the lock: the snapshot takes the progress source's own lock.
        ProgressSnapshot taken{};
        source.snapshot(source.owner,&taken);
        ProgressSnapshot gate{};
        bool gated=false;
        HANDLE timer=nullptr;
        AcquireSRWLockExclusive(&lock_);
        if(!timer_tried_){
            timer_tried_=true;
            timer_=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
        }
        timer=timer_;
        // The setting is read once per process, so the ring keeps one modulus. A caller that changes it anyway
        // starts a new ring: a slot of the old modulus holds another frame than the new one would read.
        if(latency!=latency_){latency_=latency;frame_=0;for(Slot& s:slots_)s={};}
        Slot& slot=slots_[latency_slot(frame_,latency)];
        if(slot.recorded){gate=slot.progress;gated=true;}
        slot.progress=taken;slot.recorded=true;
        ++frame_;
        ReleaseSRWLockExclusive(&lock_);
        frames_.fetch_add(1,std::memory_order_relaxed);
        if(!gated)return;                       // the first frames of the ring have nothing behind them
        if(!gate.complete){                     // more unretired fences than marks fit: it proves nothing
            skipped_.fetch_add(1,std::memory_order_relaxed);return;
        }
        const LatencyWait result=latency_wait(
            [&]() noexcept {return source.retired(source.owner,&gate);},
            latency_now_us,
            [timer](uint64_t poll) noexcept {latency_idle(poll,timer);},
            budget_us);
        polls_.fetch_add(result.polls,std::memory_order_relaxed);
        if(result.polls>1){
            waits_.fetch_add(1,std::memory_order_relaxed);
            report(false,latency,result.polls);
        }
        if(!result.retired){
            timeouts_.fetch_add(1,std::memory_order_relaxed);
            report(true,latency,result.polls);
        }
    }
    uint64_t frames() const noexcept {return frames_.load(std::memory_order_relaxed);}
    uint64_t waits() const noexcept {return waits_.load(std::memory_order_relaxed);}
    uint64_t polls() const noexcept {return polls_.load(std::memory_order_relaxed);}
    uint64_t timeouts() const noexcept {return timeouts_.load(std::memory_order_relaxed);}
    uint64_t skipped() const noexcept {return skipped_.load(std::memory_order_relaxed);}
private:
    struct Slot {ProgressSnapshot progress{};bool recorded{};};
    // The witness of the gate on the lab, bounded: the first wait and the first timeout of the process.
    void report(bool timeout,uint32_t latency,uint64_t polls) noexcept {
        static std::atomic<unsigned> waited{0},gave_up{0};
        std::atomic<unsigned>& seen=timeout?gave_up:waited;
        if(seen.fetch_add(1,std::memory_order_relaxed))return;
        char text[192];
        std::snprintf(text,sizeof(text),
            "BC250 MaxFrameLatency=%u: the present %s after %llu checks (frame %llu)\n",
            latency,timeout?"gave up":"waited",static_cast<unsigned long long>(polls),
            static_cast<unsigned long long>(frames_.load(std::memory_order_relaxed)));
        OutputDebugStringA(text);
    }
    SRWLOCK lock_=SRWLOCK_INIT;
    Slot slots_[kMaxFrameLatencySlots]{};
    uint64_t frame_{};
    uint32_t latency_{};                        // the modulus the ring holds (0: nothing recorded yet)
    HANDLE timer_{};
    bool timer_tried_{};
    std::atomic<uint64_t> frames_{0},waits_{0},polls_{0},timeouts_{0},skipped_{0};
};
}
