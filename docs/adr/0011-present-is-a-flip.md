# ADR 0011: present is a flip, the CPU blit is a diagnostic

Date: 2026-09-22. Status: **accepted** (split out of ADR 0010 point 3 at the owner's suggestion).

## Context

Under the full table the screen is black (facts M71). E20 is about to put a picture there by copying the presented
surface into the firmware's framebuffer on the CPU inside `DxgkDdiPresent`. That copy reads uncached VRAM and writes
uncached VRAM, a full 1920 x 1200 surface at a time, on the thread that presents. It proves that the surfaces are
where we think they are. It is not how a game gets a frame onto the glass.

## Decision

1. The CPU blit stays behind the registry gate `EnablePresentBlit` (default 0) for as long as it exists, and is
   removed once point 2 works. No tuning is spent on it.
2. The present path is a flip: `DxgkDdiSetVidPnSourceAddress` moves the DCN scanout to the presented surface
   (primary surface address of the HUBP the firmware lit, written inside the OTG's master update lock), and the
   vertical-sync notification comes from the display interrupt through the IH ring, replacing the timer.
3. Order of work: a read-only control of which HUBP and OTG are live, from Windows, through the gated MMIO escape
   with offsets generated from `dcn_2_0_1_offset.h`; then a Linux trace of one flip
   (`docs/linux-session-wishlist.md`); only then the first write, which by then is a register of known meaning.
4. Windowed presents that need a copy get one from an engine (CP `PACKET3_DMA_DATA` first, SDMA after ADR 0013),
   never from the CPU.

## Consequences

- The firmware framebuffer stops being the scanout once a flip has happened; returning to the display-only table or
  stopping the adapter must put the firmware's address back. That restore is part of the first flip build, not a
  follow-up.
- Until the interrupt is wired, frame pacing numbers mean nothing (ADR 0010, consequences).
