# ADR 0004: PolyForm Noncommercial 1.0.0, licensor D-Ogi

Date: 2026-09-21. Status: accepted. Supersedes ADR 0003.

## Context

The owner wants: no commercial use of the work (for example boards sold with a pirated Windows image and this driver preinstalled), mandatory credit, and as much protection as a license can give against repackaging with malware.

## Decision

- Our own code and documentation: PolyForm Noncommercial License 1.0.0 (`LICENSE.md`), a license written for software, with a patent grant and a notice mechanism.
- Attribution and provenance travel through the `Required Notice:` lines in `NOTICE`, which the license obliges every redistributor to pass on.
- Authenticity is handled outside the license: signed checksums, source-first releases, naming rule for forks (`SECURITY.md`).
- Contributions are accepted under the inbound terms in `CONTRIBUTING.md`, which let the licensor relicense later.

## Consequences

- The project is source-available, not open source in the OSI sense. Distributions and upstream projects (Mesa, Linux) cannot take our code under this license; if we ever want to upstream something, the licensor relicenses that part, which the contribution terms allow.
- Imported third-party code keeps its own license: AMD's `amdgpu` files stay MIT, anything borrowed from the predecessor stays Apache-2.0, both with their notices (`THIRD-PARTY.md`). Permissive code may be combined with ours; GPL code may not be linked into the driver.
- A license cannot stop bad actors. It gives the owner a legal basis for takedown requests against commercial or mislabelled redistribution, nothing more.
- Noncommercial use by individuals, hobbyists, researchers and educational or public institutions is explicitly permitted by the license text.
