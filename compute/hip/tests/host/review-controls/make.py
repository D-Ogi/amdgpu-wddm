"""The two controls of the review of bde0ff88, applied to the host tests of any commit: the
review's needles and inserted text, written next to the frozen sources as two new test files.

review_copy_lost.c: test_copy_lifecycle.c with a loss block in case_unretired_copy_excludes. A
known loss is terminal even when the copy's fence value is finite, so bc250hsa_copy_unretired must
answer BC250HSA_EDEVICELOST before and after the fence reaches the copy, and with no copy
unretired. bde0ff88 answers BC250HSA_OK there and fails.

review_hip_routes.cpp: test_hip_mock.cpp with the host-processor routes that section 15 did not
reach in bde0ff88 (host to host, 2D, peer, GPU fill without a CPU fallback, synchronous memset),
all while a GPU copy is unretired.

With --fixed-count the one expectation that counts late retirements is the one of a read that
retires nothing on a lost device (1). The review expected 2 and so counted the retirement that
bde0ff88 made on the lost device, which is the defect itself.

    python -I make.py <copy of compute/hip/tests/host> [--fixed-count]
"""
import sys
from pathlib import Path

H = Path(sys.argv[1])
fixed_count = '--fixed-count' in sys.argv[2:]
t = (H / 'test_copy_lifecycle.c').read_text()
needle = '''    fake_retire_all();
    g_hold = 0;
    log_clear();
    CHECK(bc250hsa_copy_unretired(&d, &pending) == BC250HSA_OK && pending == 0u);'''
replacement = '''    /* Review: a known loss is terminal even if its final fence value is finite. */
    d.device_lost = 1;
    CHECK(bc250hsa_copy_unretired(&d, &pending) == BC250HSA_EDEVICELOST);
    fake_retire_all();
    {
        bc250hsa_status lost_query = bc250hsa_copy_unretired(&d, &pending);
        printf("REVIEW lost=1 finite-retired status=%d pending=%llu\\n",
               (int)lost_query, (unsigned long long)pending);
        CHECK(lost_query == BC250HSA_EDEVICELOST);
        lost_query = bc250hsa_copy_unretired(&d, &pending);
        printf("REVIEW lost=1 no-pending status=%d\\n", (int)lost_query);
        CHECK(lost_query == BC250HSA_EDEVICELOST);
    }
    /* Only restore fixture state for the original checks; product loss is irreversible. */
    d.device_lost = 0;
    d.copy_unretired = r.fence_value;
    g_hold = 0;
    log_clear();
    CHECK(bc250hsa_copy_unretired(&d, &pending) == BC250HSA_OK && pending == 0u);'''
assert t.count(needle) == 1, 'the loss needle is not in test_copy_lifecycle.c exactly once'
t = t.replace(needle, replacement)
old = 'CHECK(counters(&c1) && c1.gpu_copy_late_retirements - c0.gpu_copy_late_retirements == 1u);'
assert t.count(old) == 1, 'the late-retirement count is not in test_copy_lifecycle.c exactly once'
if not fixed_count:
    t = t.replace(old, old.replace('== 1u', '== 2u'))
(H / 'review_copy_lost.c').write_text(t)

t = (H / 'test_hip_mock.cpp').read_text()
needle = '''            // The late retirement: the copy's bytes land and its value retires.
            bc250hsa_mock_retire_unretired();'''
replacement = '''            // Review coverage of remaining exported routes, all before genuine retirement.
            check(hipMemcpyAsync(host.data(), payload.data(), small, hipMemcpyHostToHost,
                                 stream) == want, "review: host-to-host remains excluded");
            check(host[0] == 0xEE && host[small - 1] == 0xEE,
                  "review: host-to-host moves no byte");
            check(hipMemcpy2DAsync(host.data(), 128, dst, 128, 64, 2,
                                  hipMemcpyDeviceToHost, stream) == want,
                  "review: 2D refuses before its first row");
            check(host[0] == 0xEE && host[128] == 0xEE,
                  "review: neither 2D row changed");
            check(hipMemcpyPeerAsync(other, 0, dst, 0, small, stream) == want,
                  "review: peer wrapper remains excluded");
            bc250hip::state().gpu_fill_mapped = true;
            check(hipMemsetAsync(dst, 0x31, big, stream) == want,
                  "review: GPU fill refusal cannot fall back");
            bc250hip::state().gpu_fill_mapped = false;
            check(hipMemset(dst, 0x31, small) != hipSuccess,
                  "review: synchronous memset refuses at wait");
            (void)hipGetLastError();

            // The late retirement: the copy's bytes land and its value retires.
            bc250hsa_mock_retire_unretired();'''
assert t.count(needle) == 1, 'the route needle is not in test_hip_mock.cpp exactly once'
t = t.replace(needle, replacement)
(H / 'review_hip_routes.cpp').write_text(t)
print('controls written to %s (late retirements expected: %s)' % (H, '1' if fixed_count else '2'))
