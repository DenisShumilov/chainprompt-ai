# Risk Register

## 1. Purpose
This document records the active risks that should influence work priority right now.

If a risk does not change planning, QA emphasis, phase-gate judgment, or release honesty, it does not belong here.

## 2. Severity Scale
- Critical: blocks trust, runtime safety, or realistic product progress
- High: serious weakness that should affect near-term planning
- Medium: meaningful weakness worth tracking, but not the top priority

## 3. Active Risks
### R-001 - Legacy Path Dependency in C++ Runtime
- Category: Repository / Path / Bootstrap
- Severity: Critical
- Status: Open
- Description: parts of the C++ runtime still depend on build-time or platform-specific path assumptions.
- Why it matters: the repository is not fully self-contained while runtime behavior can still depend on the configure-time repo location or fixed OS paths.
- Roadmap impact: blocks Phase 1 exit and weakens later Phase 5 productization credibility.
- Known examples:
  - `plugin/src/Scanner/WavesScanner.cpp` hardcodes the standard Windows VST3 directory instead of using JUCE default search locations
  - the data directory is baked into the binary at configure time (`CHAINPROMPT_DATA_DIR` in `plugin/CMakeLists.txt`), so moving the repo requires a rebuild
- Mitigation direction:
  - use host-discovered or config-driven runtime paths
  - use stable documented runtime paths
- QA emphasis: verify the repo still works when legacy locations are hidden or renamed

### R-002 - Backend Still Developer-Operated
- Category: Backend / IPC
- Severity: Critical
- Status: Open
- Description: backend startup still depends on manual developer setup.
- Why it matters: this blocks broader trust and keeps the product in internal-dev mode.
- Roadmap impact: blocks honest Phase 1 bootability and remains a major Phase 5 productization concern.
- Mitigation direction:
  - package or auto-launch the backend cleanly
  - make failure states understandable

### R-003 - Trust-Critical UX Is Incomplete
- Category: Trust / Product Integrity
- Severity: Critical
- Status: Open
- Description: the product core is strong, but trust UX around preview, apply, undo, and protected-state visibility is still incomplete.
- Why it matters: product identity depends on bounded, understandable mutation, not just correct backend logic.
- Roadmap impact: blocks Phase 2 exit and prevents an honest Phase 3 MVP vertical slice.
- Mitigation direction:
  - finish the safe apply flow
  - surface protected state and degraded behavior clearly

### R-004 - State / Restore Regression Risk
- Category: State / Restore
- Severity: Critical
- Status: Open
- Description: restore, slot identity, automation continuity, and graph rebuild remain high-risk change areas.
- Why it matters: silent restore regressions directly damage user work.
- Roadmap impact: blocks Phase 1 exit and stays gate-critical for Phases 2 and 3.
- Mitigation direction:
  - keep restore-sensitive changes narrow
  - require explicit QA around reload and continuity

### R-005 - Scan / Inventory / Metadata Fragility
- Category: Scan / Inventory / Metadata
- Severity: High
- Status: Open
- Description: inventory authority is stronger than before, but scan, rescan, metadata enrichment, and capability coverage remain fragile.
- Why it matters: weak inventory or metadata handling undermines host truth and capability-aware behavior.
- Roadmap impact: constrains honest Phase 4 plugin intelligence and Phase 8 multi-vendor expansion.
- Mitigation direction:
  - preserve fail-closed inventory behavior
  - improve scan and rescan stability
  - keep degraded behavior visible

### R-006 - Release-Level Overstatement
- Category: Release / Packaging
- Severity: High
- Status: Open
- Description: there is still a risk of describing a strong prototype as if it were a packaged product.
- Why it matters: overstating maturity damages trust and planning discipline.
- Roadmap impact: blocks honest Phase 10 release-candidate language until earlier gates are actually closed.
- Mitigation direction:
  - keep release language honest
  - treat `docs/future/RELEASE_POLICY.md` as future posture, not present reality

## 4. Current Phase Gate Pressure
- Phase 1 is blocked primarily by R-001, R-002, and R-004.
- Phase 2 is blocked primarily by R-003 plus any unresolved R-004 fallout.
- Phase 4 and Phase 8 remain constrained by R-005.
- Phase 10 honesty remains constrained by R-006.

## 5. Current Top Priorities
1. R-001 Legacy path dependency
2. R-002 Backend still developer-operated
3. R-004 State / restore regression risk
4. R-003 Trust-critical UX incomplete

These should normally outrank broad roadmap expansion while Phase 1 is still active.

## 6. Planning Rule
If a change touches:
- startup
- path/bootstrap
- restore
- graph rebuild
- inventory authority
- trust-critical UX

then this register should be consulted explicitly during planning, self-review, and phase-gate judgment.
