#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include "counter-abi.h"

using CounterReader = int (*)(bc250hipCounters *);
struct DecodeProfile {
    const char *phase;
    int tokens;
    CounterReader reader;
    bc250hipCounters before{}, after{};
    LARGE_INTEGER start{}, frequency{};
    int before_status = -1;
    bool finished = false;

    DecodeProfile(const char *label, int count, CounterReader test_reader = nullptr)
        : phase(label), tokens(count), reader(test_reader) {
        if (!reader) {
            HMODULE module = GetModuleHandleW(L"amdhip64.dll");
            if (module) reader = reinterpret_cast<CounterReader>(GetProcAddress(module, "bc250hipGetCounters"));
        }
        before.struct_bytes = sizeof(before);
        if (reader) before_status = reader(&before);
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);
    }
    void finish(int decode_status) {
        if (finished) return;
        finished = true;
        LARGE_INTEGER end{};
        QueryPerformanceCounter(&end);
        after.struct_bytes = sizeof(after);
        const int after_status = reader ? reader(&after) : -1;
        const bool valid = before_status == 0 && after_status == 0 &&
            after.dispatches >= before.dispatches && after.submissions >= before.submissions &&
            after.batches >= before.batches && after.dispatches_batched >= before.dispatches_batched &&
            after.waits >= before.waits && after.waits_fast >= before.waits_fast;
        fprintf(stderr, "{\"event\":\"bc250-decode-profile\",\"pid\":%lu,\"phase\":\"%s\",\"tokens\":%d,\"decode_status\":%d,\"qpc_start\":%lld,\"qpc_end\":%lld,\"qpc_hz\":%lld,\"wall_ms\":%.6f,\"counter_before_status\":%d,\"counter_after_status\":%d,\"counter_valid\":%s",
            GetCurrentProcessId(), phase, tokens, decode_status, start.QuadPart, end.QuadPart,
            frequency.QuadPart, 1000.0 * (end.QuadPart-start.QuadPart)/frequency.QuadPart,
            before_status, after_status, valid ? "true" : "false");
        if (valid) {
            fprintf(stderr, ",\"dispatches\":%llu,\"submissions\":%llu,\"batches\":%llu,\"dispatches_batched\":%llu,\"waits\":%llu,\"waits_fast\":%llu",
                after.dispatches-before.dispatches, after.submissions-before.submissions,
                after.batches-before.batches, after.dispatches_batched-before.dispatches_batched,
                after.waits-before.waits, after.waits_fast-before.waits_fast);
        }
        fprintf(stderr, "}\n");
        fflush(stderr);
    }
    ~DecodeProfile() { finish(-999); }
};
