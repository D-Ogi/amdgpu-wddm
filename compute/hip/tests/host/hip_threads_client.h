// hip_threads_client.h - the multithreaded HIP client of layer 2 (M16 route B).
//
// Why it exists: the owner's instruction of 2026-09-29 is that multithreading is tested with our
// own clients before a real application is asked to exercise it. The first build of layer 2 held
// one process lock over its waits, so a thread that waited for the device stopped every other
// thread of the process. This client measures exactly that: one thread waits for the device
// while the others launch kernels and allocate memory, and it counts the work the others finish
// inside that wait.
//
// It is one source with two consumers, so that the host test and the lab program measure the
// same thing:
//   * compute/hip/tests/host/test_hip_threads.cpp, against the mock bc250hsa backend, with a
//     negative control build that holds the lock over its waits again (BC250_HIP_WAIT_UNDER_LOCK);
//   * compute/hip/samples/threads.hip, a real HIP program that clang compiles, which runs
//     against the mock build of amdhip64.dll on the development PC and against the product DLL
//     on the lab (design section 6, step 3).
//
// The launch itself is the one thing the two cannot share: the host test registers a committed
// code object through the registration interface and launches with hipLaunchKernel, and the
// clang-built program writes `kernel<<<grid, block, 0, stream>>>(...)`. The client therefore
// takes the launch as a function pointer, and owns everything else: the threads, the window, the
// measurement and the verdict.
//
// Three phases:
//   A. One thread launches and synchronizes, which is a long wait. The others allocate and
//      launch, and a call that returns only after the wait ended does not count. A build that
//      waits with the lock held therefore measures exactly zero here, and a build that does not
//      measures as much work as the window allows.
//   B. Every thread launches and synchronizes, several times. The wall time is compared with the
//      cost of one wait times the number of waits, which is what a serialised runtime takes.
//   C. A stream and an event are destroyed by another thread while a waiter waits on them. The
//      waiter must finish normally, which is what the reference counts of the runtime are for.

#ifndef BC250_HIP_THREADS_CLIENT_H
#define BC250_HIP_THREADS_CLIENT_H

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "hip/hip_runtime.h"

namespace bc250hipthreads {

const int kMaxThreads = 8;

// Launches one small kernel on this stream, writing into this buffer. It is called from several
// threads at once, so it keeps every argument on its own stack.
typedef hipError_t (*LaunchFn)(hipStream_t stream, void* buffer, void* ctx);

struct Options {
    int      threads;          // the waiter and the workers together, 2 to kMaxThreads
    int      iterations;       // launches and synchronizes per thread in phase B
    int      destroy_rounds;   // rounds of phase C
    int      window_ops_max;   // the cap on the operations one worker counts in phase A
    int      settle_ms;        // the wait after the window opens, before a worker counts
    size_t   buffer_bytes;     // the device buffer of each thread
    size_t   worker_alloc_bytes;  // what a worker allocates per operation in phase A
    int      negative_control; // 1: this build waits with the process lock held
    int      verbose;
    LaunchFn launch;
    void*    launch_ctx;
};

struct Result {
    int    api_failures;
    int    verdict_failures;
    int    ops_in_window[kMaxThreads];
    int    ops_in_window_total;
    double window_ms;
    double one_wait_ms;
    double parallel_ms;
    double serial_estimate_ms;
};

inline Options default_options(LaunchFn launch, void* ctx) {
    Options options;
    options.threads = 4;
    options.iterations = 4;
    options.destroy_rounds = 6;
    // The kernel argument pool of layer 2 holds 64 buffers. A cap keeps one worker from taking
    // all of them and leaving the others to wait for a buffer, which is a wait for the device
    // and would read as if the lock had blocked them.
    options.window_ops_max = 16;
    options.settle_ms = 20;
    options.buffer_bytes = 65536;
    options.worker_alloc_bytes = 4096;
    options.negative_control = 0;
    options.verbose = 1;
    options.launch = launch;
    options.launch_ctx = ctx;
    return options;
}

inline double now_ms() {
    const std::chrono::steady_clock::duration since =
        std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration<double, std::milli>(since).count();
}

inline void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// Runs the three phases. It returns 0 when every HIP call answered hipSuccess and every verdict
// held, and a non-zero number otherwise. The caller owns no HIP object when this returns.
inline int run(const Options& options, Result* result) {
    Result out;
    out.api_failures = 0;
    out.verdict_failures = 0;
    out.ops_in_window_total = 0;
    out.window_ms = 0.0;
    out.one_wait_ms = 0.0;
    out.parallel_ms = 0.0;
    out.serial_estimate_ms = 0.0;
    for (int i = 0; i < kMaxThreads; ++i) {
        out.ops_in_window[i] = 0;
    }

    int threads = options.threads;
    if (threads < 2) {
        threads = 2;
    }
    if (threads > kMaxThreads) {
        threads = kMaxThreads;
    }

    std::atomic<int> api_failures(0);
    std::vector<hipStream_t> streams(static_cast<size_t>(threads), nullptr);
    std::vector<void*> buffers(static_cast<size_t>(threads), nullptr);

    for (int i = 0; i < threads; ++i) {
        if (hipStreamCreateWithFlags(&streams[static_cast<size_t>(i)], hipStreamNonBlocking) !=
            hipSuccess) {
            std::printf("hipthreads: hipStreamCreateWithFlags failed for thread %d\n", i);
            out.api_failures++;
        }
        if (hipMalloc(&buffers[static_cast<size_t>(i)], options.buffer_bytes) != hipSuccess) {
            std::printf("hipthreads: hipMalloc failed for thread %d\n", i);
            out.api_failures++;
        }
    }
    if (out.api_failures != 0) {
        *result = out;
        return out.api_failures;
    }

    // -----------------------------------------------------------------------------------------
    // Phase A: one thread waits for the device, the others work.
    // -----------------------------------------------------------------------------------------
    // 0 before the wait, 1 while the waiter is inside it, 2 after it.
    std::atomic<int> window(0);
    std::vector<int> ops(static_cast<size_t>(threads), 0);
    double window_ms = 0.0;

    std::thread waiter([&]() {
        if (options.launch(streams[0], buffers[0], options.launch_ctx) != hipSuccess) {
            api_failures++;
        }
        const double start = now_ms();
        window.store(1);
        if (hipStreamSynchronize(streams[0]) != hipSuccess) {
            api_failures++;
        }
        window_ms = now_ms() - start;
        window.store(2);
    });

    std::vector<std::thread> workers;
    for (int index = 1; index < threads; ++index) {
        workers.push_back(std::thread([&, index]() {
            std::vector<void*> mine;
            while (window.load() == 0) {
                std::this_thread::yield();
            }
            // The settle time makes the measurement fair to the negative control: the waiter is
            // certainly inside its wait by then, and a worker that got the lock first has given
            // it back.
            sleep_ms(options.settle_ms);
            while (window.load() == 1 && ops[static_cast<size_t>(index)] < options.window_ops_max) {
                void* block = nullptr;
                const hipError_t allocated = hipMalloc(&block, options.worker_alloc_bytes);
                hipError_t launched = allocated;
                if (allocated == hipSuccess) {
                    mine.push_back(block);
                    launched = options.launch(streams[static_cast<size_t>(index)],
                                              buffers[static_cast<size_t>(index)],
                                              options.launch_ctx);
                }
                if (allocated != hipSuccess || launched != hipSuccess) {
                    api_failures++;
                    break;
                }
                // Only work that finished while the other thread was still waiting counts. A
                // call that returned after the window closed is not evidence of anything.
                if (window.load() == 1) {
                    ops[static_cast<size_t>(index)]++;
                }
            }
            while (window.load() == 1) {
                sleep_ms(1);
            }
            // hipFree waits for the whole device, so it belongs after the window.
            for (size_t i = 0; i < mine.size(); ++i) {
                if (hipFree(mine[i]) != hipSuccess) {
                    api_failures++;
                }
            }
        }));
    }
    waiter.join();
    for (size_t i = 0; i < workers.size(); ++i) {
        workers[i].join();
    }
    workers.clear();

    out.window_ms = window_ms;
    for (int i = 0; i < threads; ++i) {
        out.ops_in_window[i] = ops[static_cast<size_t>(i)];
        out.ops_in_window_total += ops[static_cast<size_t>(i)];
    }

    if (options.verbose) {
        std::printf("hipthreads: phase A, the wait of thread 0 took %.1f ms\n", out.window_ms);
        for (int i = 1; i < threads; ++i) {
            std::printf("hipthreads:   thread %d finished %d operations inside that wait\n", i,
                        out.ops_in_window[i]);
        }
        std::printf("hipthreads:   %d operations in all\n", out.ops_in_window_total);
    }

    if (options.negative_control) {
        // The control build holds the process lock over its wait, so no other thread can reach
        // the runtime at all while thread 0 waits.
        //
        // One straggler is allowed, and this is why. The lock goes back inside the wait, before
        // the wait returns to the waiter thread, and the window closes one statement later. A
        // worker that is blocked on the lock at that moment therefore gets its malloc and its
        // launch through and still reads the window as open. MEASURED 2026-10-09: 1 operation in
        // one run of several, and 0 in the next three. The verdict keeps its force either way:
        // the real build finishes 48 operations in the same window.
        if (out.ops_in_window_total > 1) {
            std::printf("hipthreads: FAIL the control build let %d operations through a held "
                        "lock\n",
                        out.ops_in_window_total);
            out.verdict_failures++;
        }
    } else {
        // The verdict is the total and not a count per thread. One worker may spend the whole
        // window inside a wait of its own: the kernel argument pool is shared, and a worker that
        // takes the oldest buffer waits for its dispatch to retire, which with a long hold time
        // is the whole window. That is a wait for the device and not a wait for the lock, and
        // the total answers the question either way.
        int working = 0;
        for (int i = 1; i < threads; ++i) {
            if (out.ops_in_window[i] > 0) {
                working++;
            }
        }
        if (working == 0 || out.ops_in_window_total < threads - 1) {
            std::printf("hipthreads: FAIL the other threads finished %d operations in all while "
                        "thread 0 waited, and %d of %d of them finished anything\n",
                        out.ops_in_window_total, working, threads - 1);
            out.verdict_failures++;
        }
    }

    // -----------------------------------------------------------------------------------------
    // Phase B: every thread launches and synchronizes, and the wall time is compared with a
    // serialised runtime's.
    // -----------------------------------------------------------------------------------------
    {
        const double start = now_ms();
        if (options.launch(streams[0], buffers[0], options.launch_ctx) != hipSuccess) {
            out.api_failures++;
        }
        if (hipStreamSynchronize(streams[0]) != hipSuccess) {
            out.api_failures++;
        }
        out.one_wait_ms = now_ms() - start;
    }

    const double parallel_start = now_ms();
    for (int index = 0; index < threads; ++index) {
        workers.push_back(std::thread([&, index]() {
            for (int i = 0; i < options.iterations; ++i) {
                if (options.launch(streams[static_cast<size_t>(index)],
                                   buffers[static_cast<size_t>(index)],
                                   options.launch_ctx) != hipSuccess) {
                    api_failures++;
                    return;
                }
                if (hipStreamSynchronize(streams[static_cast<size_t>(index)]) != hipSuccess) {
                    api_failures++;
                    return;
                }
            }
        }));
    }
    for (size_t i = 0; i < workers.size(); ++i) {
        workers[i].join();
    }
    workers.clear();
    out.parallel_ms = now_ms() - parallel_start;
    out.serial_estimate_ms =
        out.one_wait_ms * static_cast<double>(threads) * static_cast<double>(options.iterations);

    if (options.verbose) {
        std::printf("hipthreads: phase B, one wait %.1f ms, %d threads x %d waits took %.1f ms, "
                    "serialised that is %.1f ms\n",
                    out.one_wait_ms, threads, options.iterations, out.parallel_ms,
                    out.serial_estimate_ms);
    }
    // The comparison means something only when a wait really waits. Against a backend that
    // retires a dispatch at once, both numbers are noise.
    if (out.one_wait_ms >= 20.0) {
        if (options.negative_control) {
            if (out.parallel_ms < 0.6 * out.serial_estimate_ms) {
                std::printf("hipthreads: FAIL the control build was faster than a serialised "
                            "runtime\n");
                out.verdict_failures++;
            }
        } else if (out.parallel_ms > 0.8 * out.serial_estimate_ms) {
            std::printf("hipthreads: FAIL %d threads took as long as one thread would have\n",
                        threads);
            out.verdict_failures++;
        }
    }

    // -----------------------------------------------------------------------------------------
    // Phase C: another thread destroys the stream and the event that a waiter waits on.
    // -----------------------------------------------------------------------------------------
    for (int round = 0; round < options.destroy_rounds; ++round) {
        hipStream_t stream = nullptr;
        hipEvent_t  event = nullptr;
        if (hipStreamCreateWithFlags(&stream, hipStreamNonBlocking) != hipSuccess ||
            hipEventCreate(&event) != hipSuccess) {
            out.api_failures++;
            break;
        }
        if (options.launch(stream, buffers[0], options.launch_ctx) != hipSuccess ||
            hipEventRecord(event, stream) != hipSuccess) {
            out.api_failures++;
            (void)hipEventDestroy(event);
            (void)hipStreamDestroy(stream);
            break;
        }
        std::atomic<int> inside(0);
        std::thread sync_thread([&]() {
            inside.store(1);
            if (hipEventSynchronize(event) != hipSuccess) {
                api_failures++;
            }
        });
        while (inside.load() == 0) {
            std::this_thread::yield();
        }
        sleep_ms(2);
        // The handles die here, under the waiter. The objects may not: the runtime holds them
        // with a reference count until the waiter lets go.
        if (hipEventDestroy(event) != hipSuccess) {
            out.api_failures++;
        }
        if (hipStreamDestroy(stream) != hipSuccess) {
            out.api_failures++;
        }
        sync_thread.join();
    }
    if (options.verbose) {
        std::printf("hipthreads: phase C, %d rounds of a stream and an event destroyed inside a "
                    "wait\n",
                    options.destroy_rounds);
    }

    // -----------------------------------------------------------------------------------------
    for (int i = 0; i < threads; ++i) {
        if (buffers[static_cast<size_t>(i)] != nullptr &&
            hipFree(buffers[static_cast<size_t>(i)]) != hipSuccess) {
            out.api_failures++;
        }
        if (streams[static_cast<size_t>(i)] != nullptr &&
            hipStreamDestroy(streams[static_cast<size_t>(i)]) != hipSuccess) {
            out.api_failures++;
        }
    }

    out.api_failures += api_failures.load();
    if (options.verbose) {
        std::printf("hipthreads: %d API failures, %d verdicts failed\n", out.api_failures,
                    out.verdict_failures);
    }
    *result = out;
    return out.api_failures + out.verdict_failures;
}

}  // namespace bc250hipthreads

#endif  // BC250_HIP_THREADS_CLIENT_H
