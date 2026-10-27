// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <atomic>
#include <functional>
#include <cstdio>
#include <memory>
#include <mutex>

namespace engine_ddi::gp {
enum class CompilationPolicy { Default, Full, Minimal, Background };
// Agility 1.721 public flags. The pinned SDK check accompanies the focused tests;
// the stable WDK DDI header does not yet name the additional flag bits.
inline constexpr UINT full_flag=0x8, minimal_flag=0x200, background_flag=0x400;
inline constexpr UINT compilation_mask=full_flag|minimal_flag|background_flag;
inline CompilationPolicy compilation_policy(UINT flags) noexcept {
    // FULL asks for the fully specialized result before return. Otherwise the
    // background request includes an initial minimal link; plain minimal is last.
    return flags&full_flag ? CompilationPolicy::Full : flags&background_flag ? CompilationPolicy::Background :
        flags&minimal_flag ? CompilationPolicy::Minimal : CompilationPolicy::Default;
}
inline HRESULT compilation_admission(UINT flags,bool executable,CompilationPolicy& policy) noexcept {
    if (flags & ~(compilation_mask | 0x7u)) return E_NOTIMPL;
    if (!executable && (flags & (minimal_flag | background_flag))) return E_INVALIDARG;
    policy=compilation_policy(flags);
    return S_OK;
}
// This pool exists only while program-owned jobs exist. One compiler and a
// bounded queue avoid turning a state-object burst into unbounded threads/RAM.
class SpecializationPool {
public:
    PTP_POOL pool = nullptr;
    TP_CALLBACK_ENVIRON environment{};
    std::atomic<unsigned> reservations{0};
    static constexpr unsigned capacity = 64;
    SpecializationPool() {
        pool = CreateThreadpool(nullptr);
        if (!pool) throw std::bad_alloc();
        SetThreadpoolThreadMaximum(pool, 1);
        if (!SetThreadpoolThreadMinimum(pool, 1)) { CloseThreadpool(pool); pool=nullptr; throw std::bad_alloc(); }
        InitializeThreadpoolEnvironment(&environment);
        SetThreadpoolCallbackPool(&environment, pool);
    }
    ~SpecializationPool() { DestroyThreadpoolEnvironment(&environment); CloseThreadpool(pool); }
    static std::shared_ptr<SpecializationPool> acquire() {
        static std::mutex mutex;
        static std::weak_ptr<SpecializationPool> current;
        std::lock_guard lock(mutex);
        auto pool=current.lock();
        if (!pool) { pool=std::make_shared<SpecializationPool>(); current=pool; }
        return pool;
    }
    bool reserve() noexcept {
        unsigned n=reservations.load(std::memory_order_relaxed);
        while (n<capacity) if (reservations.compare_exchange_weak(n,n+1,std::memory_order_relaxed)) return true;
        return false;
    }
};

template<class Pipeline> class BackgroundPipeline {
    std::shared_ptr<SpecializationPool> pool_;
    PTP_WORK work_=nullptr;
    std::function<HRESULT(Pipeline**)> compile_;
    std::atomic<Pipeline*> ready_{nullptr};
    std::atomic<HRESULT> status_{E_PENDING};
    std::atomic<bool> reserved_{true};
    void unreserve() noexcept {
        if (reserved_.exchange(false,std::memory_order_acq_rel)) pool_->reservations.fetch_sub(1,std::memory_order_relaxed);
    }
    static void CALLBACK run(PTP_CALLBACK_INSTANCE,void* context,PTP_WORK) noexcept {
        auto* job=static_cast<BackgroundPipeline*>(context);
        Pipeline* result=nullptr; HRESULT hr=E_FAIL;
        try { hr=job->compile_(&result); } catch (...) { hr=E_FAIL; }
        if (SUCCEEDED(hr) && result) job->ready_.store(result,std::memory_order_release);
        else { if (result) result->Release(); if (SUCCEEDED(hr)) hr=E_UNEXPECTED; }
#if defined(AMDGPU_WDDM_LAB_FLAVOUR) && AMDGPU_WDDM_LAB_FLAVOUR
        std::fprintf(stderr,"PGP compilation publish hr=%08lx ready=%u\n",static_cast<unsigned long>(hr),job->ready() != nullptr);
#endif
        job->status_.store(hr,std::memory_order_release);
        job->unreserve();
        // No self ownership: only Program destroys/waits for this job. A callback
        // can never become the last owner and wait for its own completion.
    }
public:
    BackgroundPipeline(std::shared_ptr<SpecializationPool> pool,std::function<HRESULT(Pipeline**)> compile)
        :pool_(std::move(pool)),compile_(std::move(compile)) {}
    BackgroundPipeline(const BackgroundPipeline&)=delete;
    ~BackgroundPipeline() {
        if (work_) { WaitForThreadpoolWorkCallbacks(work_,TRUE); CloseThreadpoolWork(work_); }
        unreserve();
        if (auto* ready=ready_.load(std::memory_order_acquire)) ready->Release();
    }
    static std::shared_ptr<BackgroundPipeline> start(std::function<HRESULT(Pipeline**)> compile) {
        auto pool=SpecializationPool::acquire();
        if (!pool->reserve()) return {};
        std::shared_ptr<BackgroundPipeline> job;
        try { job=std::make_shared<BackgroundPipeline>(pool,std::move(compile)); }
        catch (...) { pool->reservations.fetch_sub(1,std::memory_order_relaxed); throw; }
        job->work_=CreateThreadpoolWork(run,job.get(),&pool->environment);
        if (!job->work_) return {};
        SubmitThreadpoolWork(job->work_);
        return job;
    }
    Pipeline* ready() const noexcept { return ready_.load(std::memory_order_acquire); }
    HRESULT status() const noexcept { return status_.load(std::memory_order_acquire); }
};
}
