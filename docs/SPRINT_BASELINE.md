# Sprint Baseline

## 1. Purpose
This file is the short operational snapshot of the project.

It should answer:
1. what is strong enough to build on
2. what is still risky
3. what kind of phase-scoped work should come next
4. what maintainers should read first

## 2. Current Status
- Product state: strong internal prototype with a real technical nucleus
- Active roadmap phase: Phase 1 `Prototype Stabilization`
- Next phase: Phase 2 `Trust UX Foundation` after Phase 1 exit
- Repository state: cleaned canonical structure, still stabilization-first
- Runtime shape: JUCE plugin + local Python backend + manifest-first inventory gate

## 3. Stable Enough To Build On
- serial insert-chain hosting
- manifest-first inventory authority
- two-stage LLM pipeline
- bounded diff and mutation model
- DAW state save and restore
- audio capture and offline analysis
- core backend tests for critical logic
- explicit backend health diagnostics (unreachable / no-inventory / ready)
- canonical manual startup and health-check path documented in README

## 4. Still Fragile
- legacy path assumptions in C++
- Desktop-based lookup or output behavior
- backend still developer-operated (now with explicit diagnostics)
- trust-critical UX still incomplete
- release packaging still incomplete

## 5. Current Phase Gate
Phase 1 is not complete until all of these are true:

- remaining machine-specific runtime path assumptions are removed from canonical bring-up
- backend startup and failure states are understandable without hidden local knowledge
- startup, scan, and restore behavior are stable enough to support later UX work
- another developer can bring the system up from the documentation alone without relying on legacy machine rituals

This means:
- Phase 2 trust UX work is still blocked by the current stabilization gate
- Phase 5 productization work is still future-only unless it directly removes a Phase 1 blocker

## 6. Current Focus
Current default:

**Phase 1 stabilization work**

Choose roadmap expansion only when Phase 1 exit criteria are no longer the dominant risk.

## 7. Nearest Tasks
- remove remaining legacy Desktop or legacy-root path assumptions in the C++ runtime and scanner
- ~~make backend unavailable state explicit in the plugin/server contract without jumping early into full auto-launch~~ (done)
- ~~define and document one canonical manual startup and health-check path~~ (done)
- add regression checks around startup, rescan, restore, and backend reachability for the new bring-up path
- verify that a non-author developer can boot and diagnose the system from the documentation alone

## 8. Top Immediate Problem
Backend diagnostics are now explicit and the canonical manual startup path is documented. Phase 1 exit is still blocked by remaining legacy path assumptions and developer-only bring-up verification.

## 9. Regression Watchlist
Every non-trivial sprint should watch:
- plugin scan and load
- DAW startup
- backend and IPC boot
- state restore
- parameter continuity
- graph rebuild
- machine-specific path assumptions

## 10. Maintainer Reading Order
Default recommended reading order:
1. `README.md`
2. `docs/CASE_STUDY.md`
3. `docs/DEMO_PLAN.md`
4. `docs/VALIDATION_PLAN.md`
5. `SECURITY.md`
6. `docs/EXECUTION_ROADMAP.md`
7. `docs/SPRINT_BASELINE.md`
8. `docs/RISK_REGISTER.md`
9. `docs/ARCHITECTURE.md`
10. `docs/PRODUCT_VISION.md`

Optional:
- `plugin/docs/ARCHITECTURE.md`

Do not read by default:
- `docs/future/*` (future posture only, not current behavior)

## 11. Practical Summary
The project has a strong core and a cleaner documentation surface than before, but the roadmap is still in Phase 1.

The right default remains:

**close the current gate first, then widen scope deliberately**
