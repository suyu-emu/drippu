\# drippu Roadmap

\*\*drippu\*\* (also known as “suyu experimental”) is the experimental edition of the suyu Nintendo Switch emulator and AArch64 native recompiler. It exists as a high-velocity sandbox for emulator core work, recompiler research, UI experiments, platform support, and cross-project integrations that are too unstable or too speculative for the main suyu line.

This document describes the current high-level direction, the major planned workstreams, and the process by which stable results are expected to flow back into suyu. Priorities and timelines will shift; treat this as living guidance rather than a fixed schedule.

\---

\## Guiding Principles

\- \*\*Experimental by design\*\* — Rough edges, rapid iteration, and breaking changes are expected.

\- \*\*Recompiler-first research\*\* — The AArch64 static/hybrid recompiler path is a primary focus area.

\- \*\*Upstream contribution\*\* — Features that reach sufficient stability are candidates for promotion back to suyu main.

\- \*\*Cross-project collaboration\*\* — Active exploration of consolidation with related projects (notably Ruzu) and integration of complementary ecosystems (Nextendo, NX-Optimiser).

\- \*\*User-provided credentials only\*\* — All online / NSO-related work requires the user to supply their own Switch hardware credentials and keys; drippu does not distribute or circumvent Nintendo TPMs.

\---

\## Current High-Priority Workstreams

\### 1. Experimental GPU-side implementation into the recompiler

\*\*Status:\*\* Planned / early research

\*\*Goal:\*\* Move selected GPU-related work (shader translation, command-buffer handling, or memory-management paths) into or alongside the recompiler pipeline so that recompiled titles can benefit from tighter host-side GPU integration.

\*\*Scope considerations:\*\*

\- Identify which GPU subsystems are currently pure HLE and which can be partially or fully lifted into recompiled code or host-side acceleration.

\- Prototype GPU-side hooks or custom code-generation paths that preserve correctness while reducing translation overhead.

\- Maintain fallback to the existing HLE GPU backend so that titles outside the experimental path continue to function.

\- Establish clear correctness and performance metrics before any promotion attempt.

\*\*Risks \& notes:\*\*

\- High complexity; expect multiple experimental branches.

\- Compatibility baseline tooling (see `docs/recompiler-compatibility-baseline.md`) will be extended to cover GPU-related workloads once prototypes exist.

\---

\### 2. Consolidation / merger with Ruzu

\*\*Status:\*\* Planned exploratory track

\*\*Goal:\*\* Evaluate and, where beneficial, consolidate or merge relevant technology, architecture, or code from the Ruzu project (the experimental Rust reimplementation of the yuzu/Eden lineage) into drippu, or establish a durable interoperability / dual-maintenance strategy.

\*\*Areas of interest:\*\*

\- Structural parity and lessons from the large-scale C++ → Rust port.

\- Any mature subsystems (CPU backends, VFS, configuration, front-ends) that can improve drippu’s maintainability or performance.

\- Shared testing infrastructure and compatibility data.

\- Decision points: full merge, selective cherry-picks, dual-core maintenance, or formal collaboration agreement.

\*\*Process:\*\*

\- Technical comparison of architectures and ownership models.

\- Prototype integration of high-value components.

\- Community and maintainer discussion before any irreversible structural change.

\---

\### 3. Full implementation of Nextendo

\*\*Status:\*\* Planned

\*\*Goal:\*\* Deliver a complete, first-class client-side implementation of the Nextendo Network (community online service) inside drippu so that supported titles can use Nextendo account, friends, matchmaking, presence, and peer-to-peer play without external host-file or SSL-bypass tooling.

\*\*Expected capabilities:\*\*

\- Built-in hostname redirection / DNS-MITM for Nextendo servers.

\- Account sign-in flow (browser-based OAuth / PKCE style, credentials never stored by the emulator beyond the user’s local profile).

\- Support for currently documented Nextendo titles (and future expansions) with the same transparency model used by other Nextendo clients.

\- Optional environment overrides for server endpoints (restricted to safe values).

\- Clear UI affordances under a Nextendo / Network menu.

\*\*Notes:\*\*

\- Nextendo is an independent community project; drippu’s implementation will remain optional and opt-in.

\- No Nintendo account or official NSO servers are involved.

\---

\### 4. Full implementation of NX-Optimiser

\*\*Status:\*\* Planned

\*\*Goal:\*\* Integrate full support for the NX-Optimiser / UltraCam ecosystem so that users can discover, apply, and manage performance and quality mods (dynamic framerate, resolution scaling, free camera, cheats, etc.) directly from within drippu or via a tightly coupled workflow.

\*\*Scope:\*\*

\- Detection of NX-Optimiser-compatible mod packages.

\- Correct placement of `exefs` / `romfs` (and related) content into the appropriate per-title mod directories for both HLE and recompiler modes.

\- UI or launcher integration that mirrors the “Legacy” / Ryujinx-style paths already supported by NX-Optimiser.

\- Documentation of recommended presets and known interactions with drippu’s own graphics and CPU settings.

\*\*Notes:\*\*

\- NX-Optimiser remains an external project; drippu will focus on seamless interoperation rather than re-implementing the mod manager itself unless a stronger integration proves necessary.

\---

\### 5. Monthly (or repeated) stability evaluations and upstream promotion

\*\*Status:\*\* Process to be formalized

\*\*Goal:\*\* Establish a recurring cadence (initially monthly) of formal evaluations that identify features, fixes, or subsystems which have become stable enough in drippu to be proposed for inclusion in suyu main.

\*\*Evaluation criteria (indicative):\*\*

\- Sustained correctness on the recompiler compatibility baseline and expanded game/test suites.

\- Absence of major regressions on Windows, Linux, macOS, and Android.

\- Performance neutral or positive relative to the previous baseline.

\- Clean separation from drippu-only experimental flags.

\- Adequate documentation and test coverage.

\*\*Process outline:\*\*

1\. Maintainers compile a candidate list from recent drippu work.

2\. Targeted testing and review (CI + manual).

3\. Preparation of clean, branded, well-documented patches or PRs against suyu.

4\. Community feedback window.

5\. Merge or defer with recorded rationale.

This is intended to be the primary mechanism by which drippu’s experimental investment returns value to the broader suyu user base.

\---

\### 6. NSO Service implementation (user-supplied hardware credentials)

\*\*Status:\*\* Planned / research

\*\*Goal:\*\* Implement the necessary service layers so that users who possess legitimate Switch hardware credentials can interact with Nintendo Switch Online (NSO) functionality from within drippu where technically and legally appropriate.

\*\*Key constraints:\*\*

\- Users must supply their own hardware-derived credentials / keys; drippu will never ship or generate Nintendo secrets.

\- Strict adherence to the project’s existing legal posture (user-provided keys, no TPM circumvention, interoperability focus).

\- Clear UI and documentation that credentials remain under the user’s control and are stored only locally if at all.

\- Prefer minimal, well-scoped service implementations that can later be audited and, if desired, upstreamed.

\*\*Risk \& review:\*\*

\- This workstream will receive extra legal and security review before any public-facing code lands.

\- Features that cannot be implemented cleanly under the project’s constraints will be deferred or rejected.

\---

\## Supporting / Ongoing Work

In addition to the six priority items above, drippu continues to track and incrementally advance:

\- Eden improvement migration and backlog (see `docs/EDEN\_IMPROVEMENTS\_BACKLOG.md` and `docs/EDEN\_MERGE\_PLAN.md`).

\- Recompiler compatibility baseline expansion and harness improvements.

\- Platform support (Windows, Linux, macOS via MoltenVK, Android).

\- Build-system, CI, and dependency hygiene.

\- UI / branding consistency (user-facing “drippu”, internal targets may still carry historical names).

\- Documentation, contributor guidelines, and policy documents under `docs/`.

\---

\## How to Follow Progress

\- Watch the \[drippu repository](https://github.com/suyu-emu/drippu) and its issues / discussions.

\- Review release notes and the recompiler-compatibility baseline artifacts published by CI.

\- Participate in the suyu / drippu community channels for design discussions and evaluation feedback.

\- For upstream promotion candidates, follow the corresponding pull requests or issues on the main suyu line once they are opened.

\---

\## Document Status

\- \*\*Created:\*\* September 2026

\- \*\*Focus:\*\* Captures the six primary planned workstreams supplied by maintainers, placed in the broader context of drippu’s experimental charter.

\- \*\*Next review:\*\* After the first formal monthly stability evaluation or when any of the major workstreams reaches a significant milestone.

This roadmap is intentionally high-level. Detailed design documents, issue trackers, and experimental branches will expand each item as work progresses.

