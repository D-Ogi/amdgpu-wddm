// SPDX-License-Identifier: MIT
// The release gate of heap memory, linked against the native engine-ddi.lib (no harness macro), the way the
// shell's DLL links it. No engine, no GPU: the test builds a DeviceContext and one engine queue by hand and
// drives the state word the way submit_locked does (queue.cpp:17-32), so that the retirement decisions can be
// made at exact moments. What it proves:
//
//   1. set_release_policy's refusals and its two settings.
//   2. Two phases on (the driver's default, M15.8 F1): a submission made between the destroy and the release
//      keeps the memory until its own mark retires. This is trial 245's window: GFX job 604859 reached the
//      kernel 2.1 ms before the unmap of the memory it read.
//   3. Two phases off (release-two-phase-off, adapter106): the same sequence frees the memory while that
//      submission is still in flight.
//   4. A destroy with every queue idle: with two phases the release still waits for one second phase, so a
//      submission that races the destroy is covered; without them it runs inside the destroy.
//   5. A removed device stays stuck under both settings, and the memory is never released.
//   6. The memory of a linear primary (in_ddi) keeps its single bounded phase inside the destroy.
#include "internal.h"
#include <cstdarg>
#include <cstdio>
#include <io.h>

namespace {
int failures = 0;
void check(bool ok, const char* format, ...) {
    char text[512];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", text);
    failures += ok ? 0 : 1;
}

// The stub retirement fence. engine-ddi reads GetCompletedValue only (completed_value, context.cpp).
class Fence final : public ID3D12Fence {
public:
    UINT64 value = 0;
    ULONG refs = 1;
    unsigned reads = 0;
    void (*on_read)(Fence*) = nullptr;
    void* user = nullptr;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override {
        if (out) *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void** out) override {
        if (out) *out = nullptr;
        return E_NOTIMPL;
    }
    UINT64 STDMETHODCALLTYPE GetCompletedValue() override {
        ++reads;
        if (on_read) on_read(this);
        return value;
    }
    HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64, HANDLE) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Signal(UINT64) override { return E_NOTIMPL; }
};

struct Shell {
    unsigned freed = 0;
    unsigned errors = 0;
    HRESULT free_result = S_OK;
};
HRESULT APIENTRY free_memory(void* shell, const engine_ddi::ImportedMemory*) {
    ++static_cast<Shell*>(shell)->freed;
    return static_cast<Shell*>(shell)->free_result;
}
void APIENTRY device_error(void* shell, HRESULT) { ++static_cast<Shell*>(shell)->errors; }
void APIENTRY list_error(void*, D3D12DDI_HRTCOMMANDLIST, HRESULT) {}
BOOL APIENTRY device_lost(void*) { return FALSE; }
HRESULT APIENTRY bind_table(void*, D3D12DDI_HRTCOMMANDLIST, uint32_t) { return S_OK; }

// One device context with one DIRECT engine queue, as create_device_context and create_engine_queue leave
// them for the parts this test drives (no engine object is reachable from either).
struct Fixture {
    Shell shell;
    Fence fence;
    engine_ddi::DeviceContext context;
    engine_ddi::EngineQueue queue{};
    Fixture() {
        context.hooks = {sizeof(engine_ddi::ShellHooks), &shell, device_error, list_error, device_lost,
                         bind_table, nullptr, free_memory};
        queue.context = &context;
        queue.fence = &fence;
        queue.id = 1;
        queue.slot = 0;
        queue.type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        InitializeSRWLock(&queue.submit_lock);
        context.queues[0] = &queue;
        context.queue_mask = 1;
    }
    // submit_locked's effect on the state word: the work becomes uncovered, then one signal covers it.
    uint64_t submit() {
        const uint64_t value = (queue.state.fetch_or(1) >> 1) + 1;
        queue.state.store(value << 1);
        return value;
    }
    bool policy(uint32_t two_phase) {
        engine_ddi::ReleasePolicy p{sizeof(p), two_phase};
        return engine_ddi::set_release_policy(&context, &p) == S_OK;
    }
    // A release of heap memory, as backing_release hands it over (resources.cpp:1128-1135).
    engine_ddi::PendingRelease* node(bool in_ddi = false) {
        auto* n = new engine_ddi::PendingRelease{};
        n->payload.has_memory = true;
        n->payload.id = ++ids;
        n->payload.in_ddi = in_ddi;
        n->payload.memory.size = sizeof(engine_ddi::ImportedMemory);
        return n;
    }
    uint64_t ids = 0;
};
} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    _dup2(_fileno(stdout), _fileno(stderr));        // engine-ddi's log lines, in order with the results
    SetEnvironmentVariableA("AMDGPU_WDDM_LOG", "stderr"); // the sink is off by default (stdio-log.h)

    // 1. The policy's refusals.
    {
        engine_ddi::ReleasePolicy p{sizeof(p), 1};
        check(engine_ddi::set_release_policy(nullptr, &p) == E_INVALIDARG, "release policy: null context refused");
        engine_ddi::DeviceContext c;
        check(engine_ddi::set_release_policy(&c, nullptr) == E_INVALIDARG, "release policy: null policy refused");
        engine_ddi::ReleasePolicy bad_size{sizeof(p) + 4, 1};
        check(engine_ddi::set_release_policy(&c, &bad_size) == E_INVALIDARG, "release policy: wrong size refused");
        engine_ddi::ReleasePolicy bad_value{sizeof(p), 2};
        check(engine_ddi::set_release_policy(&c, &bad_value) == E_INVALIDARG &&
                  engine_ddi::set_release_policy(&c, &p) == S_OK,
              "release policy: two_phase 2 refused, 1 accepted");
    }

    // 2. Two phases: a submission between the destroy and the release holds the memory.
    {
        Fixture f;
        check(f.policy(1), "two phases: policy set");
        f.submit();                                             // value 1, in flight
        auto* n = f.node();
        f.context.release(n);
        const bool recorded = f.context.pending.load() == 1 && !f.shell.freed;
        const uint64_t later = f.submit();                      // value 2: after the destroy, still in flight
        f.fence.value = 1;                                      // the destroy's own phase retires
        f.context.process_retired();
        const bool held = f.context.pending.load() == 1 && !f.shell.freed;
        f.fence.value = later;                                  // the later submission retires
        f.context.process_retired();
        check(recorded && held && !f.context.pending.load() && f.shell.freed == 1 && !f.shell.errors,
              "two phases: a submit between the destroy and the release holds the memory until its mark "
              "retires (recorded %u, held %u, freed %u)",
              unsigned(recorded), unsigned(held), f.shell.freed);
    }

    // 3. One phase (release-two-phase-off): the same sequence frees it while that submission is in flight.
    {
        Fixture f;
        check(f.policy(0), "one phase: policy set");
        f.submit();
        auto* n = f.node();
        f.context.release(n);
        const bool recorded = f.context.pending.load() == 1 && !f.shell.freed;
        f.submit();                                             // value 2, in flight
        f.fence.value = 1;
        f.context.process_retired();
        check(recorded && !f.context.pending.load() && f.shell.freed == 1,
              "one phase: adapter106's behaviour, the memory goes back with a submission still in flight "
              "(recorded %u, freed %u)",
              unsigned(recorded), f.shell.freed);
    }

    // 4. A destroy with every queue idle.
    {
        Fixture f;
        check(f.policy(1), "idle destroy: policy set");
        f.fence.value = f.submit();                              // retired before the destroy
        auto* n = f.node();
        f.context.release(n);
        const bool recorded = f.context.pending.load() == 1 && !f.shell.freed;
        const uint64_t racing = f.submit();                      // the submission that races the destroy
        f.context.process_retired();
        const bool held = f.context.pending.load() == 1 && !f.shell.freed;
        f.fence.value = racing;
        f.context.process_retired();
        check(recorded && held && f.shell.freed == 1,
              "two phases: a destroy with idle queues still waits for one second phase (recorded %u, held %u, "
              "freed %u)",
              unsigned(recorded), unsigned(held), f.shell.freed);

        Fixture g;
        check(g.policy(0), "idle destroy, one phase: policy set");
        g.fence.value = g.submit();
        g.context.release(g.node());
        check(!g.context.pending.load() && g.shell.freed == 1,
              "one phase: a destroy with idle queues releases inside the destroy (freed %u)", g.shell.freed);
    }

    // 5. A removed device: stuck under both settings.
    for (uint32_t two_phase = 0; two_phase < 2; ++two_phase) {
        Fixture f;
        check(f.policy(two_phase), "removed device: policy %u set", two_phase);
        f.submit();
        f.fence.value = engine_ddi::kFenceRemoved;
        f.context.release(f.node());
        f.context.process_retired();
        check(f.context.pending.load() == 1 && !f.shell.freed,
              "removed device (two_phase %u): the release is stuck and the memory stays owned", two_phase);
    }

    // 6. The linear primary's bounded phase inside the destroy, with two phases on: the bound passes, the
    //    node is recorded at phase 0 without reporting an API error, so the next retirement releases it.
    {
        Fixture f;
        check(f.policy(1), "linear primary: policy set");
        f.context.in_ddi_bound_ms = 1;
        const uint64_t value = f.submit();
        auto* n = f.node(true);
        f.context.release(n);
        const bool late = !f.shell.errors && f.context.pending.load() == 1 && !f.shell.freed;
        f.fence.value = value;
        f.context.process_retired();
        check(late && !f.context.pending.load() && f.shell.freed == 1 && !f.shell.errors,
              "linear primary: timeout retains memory without an API error, then one retirement point (late %u, "
              "freed %u)",
              unsigned(late), f.shell.freed);
    }

    // BD-221: later work on the same queue must not move this destroy's original target.
    {
        Fixture f;
        f.context.in_ddi_bound_ms = 30;
        f.submit();
        f.fence.user = &f.queue;
        f.fence.on_read = [](Fence* fence) {
            auto* q = static_cast<engine_ddi::EngineQueue*>(fence->user);
            q->state.store(uint64_t{fence->reads + 1} << 1);
            fence->value = fence->reads - 1; // always behind new work, but original mark 1 retires
        };
        f.context.release(f.node(true));
        check(f.shell.freed == 1 && !f.shell.errors && !f.context.pending.load() && f.fence.refs == 1,
              "BD-221: destroy waits for its frozen mark, not continuing submissions (freed %u, pending %u)",
              f.shell.freed, f.context.pending.load());
    }
    // An uncovered mark cannot rely on a future application Signal. Retain safely, return promptly.
    {
        Fixture f;
        f.queue.state.store(1);
        const ULONGLONG began = GetTickCount64();
        f.context.release(f.node(true));
        check(GetTickCount64() - began < 500 && !f.shell.errors && !f.shell.freed &&
                  f.context.pending.load() == 1 && f.fence.refs == 1,
              "BD-221: unsignalled work without a future Signal retains memory without a 2 s wait or error");
        f.queue.state.store(2);
        f.fence.value = 1;
        f.context.process_retired();
        check(f.shell.freed == 1, "BD-221: subsequent proven engine signal can retire retained memory");
    }
    // Suppression is specific to a deferred in-DDI owner expiration, not arbitrary free failures.
    for (unsigned arm = 0; arm < 4; ++arm) {
        Fixture f;
        f.context.in_ddi_bound_ms = 1;
        f.context.release_two_phase = false;
        f.shell.free_result = arm == 1 ? E_OUTOFMEMORY : HRESULT_FROM_WIN32(ERROR_INVALID_OWNER);
        const bool in_ddi = arm != 2;
        const bool deferred = arm != 3;
        if (deferred) f.submit();
        f.context.release(f.node(in_ddi));
        f.fence.value = 1;
        f.context.process_retired();
        check(f.shell.freed == 1 && f.shell.errors == (arm ? 1u : 0u) &&
                  f.context.releases_owner_expired.load() == (arm ? 0u : 1u),
              "BD-221: free-result arm %u preserves unrelated device errors (errors %u)", arm, f.shell.errors);
        f.context.process_retired();
        check(f.shell.freed == 1, "BD-221: free is never retried after owner expiration or failure");
    }
    // Slot reuse while the lock is released is not proof of the original queue's retirement.
    for (unsigned completed = 0; completed < 2; ++completed) {
        Fixture f;
        f.context.in_ddi_bound_ms = 5;
        f.submit();
        f.fence.user = &f.queue;
        f.fence.on_read = [](Fence* fence) {
            if (fence->reads >= 3) static_cast<engine_ddi::EngineQueue*>(fence->user)->id = 2;
        };
        if (completed) f.fence.on_read = [](Fence* fence) {
            if (fence->reads >= 3) {
                static_cast<engine_ddi::EngineQueue*>(fence->user)->id = 2;
                fence->value = 1;
            }
        };
        f.context.release(f.node(true));
        f.fence.value = 100; // a replacement queue's large fence cannot free unproven old work
        f.context.process_retired();
        check(f.shell.freed == completed && f.context.releases.stuck() == (completed ? 0u : 1u) &&
                  f.fence.refs == 1 && !f.shell.errors,
              "BD-221: slot reuse arm %u requires original fence proof (freed %u)", completed, f.shell.freed);
    }

    std::printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
