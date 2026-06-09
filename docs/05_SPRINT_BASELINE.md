# Sprint Baseline

## 1. Purpose
This file is the short operational snapshot of the project.

It should answer:
1. what is strong enough to build on
2. what is still risky
3. what kind of phase-scoped cycle should come next
4. what the Analyst should read first

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
- another developer can bring the system up from canon without relying on legacy machine rituals

This means:
- Phase 2 trust UX work is still blocked by the current stabilization gate
- Phase 5 productization work is still future-only unless it directly removes a Phase 1 blocker

## 6. Recommended Cycle Mode
Current default:

**Phase 1 stabilization cycle**

Choose roadmap expansion only when Phase 1 exit criteria are no longer the dominant risk.

## 7. Nearest Correct Cycles
- remove remaining legacy Desktop or legacy-root path assumptions in the C++ runtime and scanner
- ~~make backend unavailable state explicit in the plugin/server contract without jumping early into full auto-launch~~ (DONE: cycle-002)
- ~~define and document one canonical manual startup and health-check path~~ (DONE: cycle-002)
- add regression checks around startup, rescan, restore, and backend reachability for the new bring-up path
- verify that a non-author developer can boot and diagnose the system from canon alone

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

## 10. Analyst Reading Order
Default mandatory reading order for the Analyst:
1. `docs/04_REPOSITORY_CANON.md`
2. `docs/03_EXECUTION_ROADMAP.md`
3. `docs/05_SPRINT_BASELINE.md`
4. `docs/12_RISK_REGISTER.md`
5. `docs/02_ARCHITECTURE.md`
6. `docs/01_PRODUCT_VISION.md`

Optional:
- `docs/06_AGENT_OPERATING_MODEL.md`
- `plugin/docs/02_ARCHITECTURE.md`

Do not read by default:
- `docs/reference/*`
- `docs/future/*`

## 11. Practical Summary
The project has a strong core and a cleaner documentation surface than before, but the roadmap is still in Phase 1.

The right default remains:

**close the current gate first, then widen scope deliberately**
