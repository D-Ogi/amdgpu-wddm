Addendum to README.txt, 2026-09-21 (the README itself is not edited after its commit).

README.txt says that it is not known whether the sp5100_tco watchdog reset the machine. The owner has answered it:
the watchdog did NOT reset the board; the owner switched it off and on by hand. The streamed kernel log ends at
about 17:43:30 local time and Windows reports a boot time of 17:47:57, so the hung machine had roughly four minutes,
four times the watchdog's 60 s, and stayed hung.

Why it did not fire is not measured. Two readings, neither tested: the feeding loop in user space was still running
(the kernel kept logging USB timeouts for seconds after amdgpu went silent, so the machine did not stop all at once),
or the hang took the watchdog's own path to a reset with it. Either way the consequence is the same: a watchdog fed
from a shell loop is no protection against this hang. A feeder that stops when the GPU step stops answering (tied to
the step's own progress, not to a timer loop) is the variant worth trying next; until then the reset button has a
name and it is the owner's. Umiesz liczyć, licz na siebie (if you can count, count on yourself).
