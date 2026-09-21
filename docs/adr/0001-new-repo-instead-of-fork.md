# ADR 0001: New repository instead of a fork of the existing driver

Date: 2026-09-21. Status: accepted.

## Context

`Keshas-dev/AMD-BC-250-Windows-Driver` exists, is active, and contains working SMU and PSP code. It could be forked.

## Decision

Start a new repository. Treat the predecessor as a donor of verified pieces (Apache-2.0, attribution in `THIRD-PARTY.md`) and as reference material kept outside this repo.

## Reasons

- Its architecture (WDM IOCTL driver + KMDOD) cannot become a render driver by patching; a WDDM miniport is a different program.
- Its kernel driver is an 8000-line monolith with hand-written offsets throughout and registry switches that skip crashing init steps.
- Its knowledge base (`AGENTS.md`, `docs/`) mixes correct and refuted claims without marking which is which. Any agent or person working inside a fork inherits those premises.
- It embeds AMD firmware blobs in an Apache-licensed source header.

## Consequences

- We owe upstream a clear bug report about the addressing error once E01 confirms it on hardware (draft at the end of `predecessor-analysis.md`).
- Anything we borrow is re-verified under our evidence rules before it counts as working.
