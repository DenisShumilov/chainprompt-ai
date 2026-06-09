# Execution Roadmap

## 1. Purpose
This document turns the approved roadmap into the canonical phase and gate model for ChainPrompt AI.

It answers:
- which phase is active now
- what is blocked until later
- what a single work increment is allowed to do
- what evidence is required to move to the next phase

## 2. Roadmap Authority
Use the roadmap together with the rest of the core docs:

- `docs/PRODUCT_VISION.md` defines product truth and the trust-first product boundary.
- `docs/ARCHITECTURE.md` defines the current technical nucleus and real runtime shape.
- `docs/EXECUTION_ROADMAP.md` defines phase order, active phase, gate criteria, and what a valid work increment looks like.
- `docs/SPRINT_BASELINE.md` is the short operational snapshot of the current phase.
- `docs/RISK_REGISTER.md` records the active risks that can block a phase gate.

This file is the single roadmap document for the repository.

## 3. Current Roadmap Status
- Phase 0 `Product Truth Lock` is locked enough to operate from the core docs.
- Active phase: Phase 1 `Prototype Stabilization`.
- Next phase: Phase 2 `Trust UX Foundation`.
- Later phases remain blocked until the current phase exits cleanly.

Why Phase 1 is active:
- the repo still has legacy machine-specific runtime assumptions
- backend bring-up is still developer-operated (now with explicit diagnostics and a canonical manual startup path)
- the trust-first promise is stronger in backend primitives than in user-visible flow
- the product still behaves like a strong prototype before it behaves like a self-contained product loop

While Phase 1 is active, the following stay future-only by default:
- preview / confirm / rollback UX completion beyond what is needed for Phase 1 diagnostics
- MVP vertical-slice claims
- plugin intelligence expansion beyond the current support truth
- auto-launch and packaging work that belongs to Phase 5
- evaluation-system buildout
- beginner-first UX
- multi-vendor expansion
- bounded automation modes

## 4. Phase Gate Rules
- Phase order is strict. Do not start a later phase because it looks attractive.
- One work increment equals one narrow, reviewable change inside the active phase.
- A valid increment must close one explicit gap in the active phase exit criteria, or fix a regression that blocks the active phase.
- Planning work should stay limited to:
  - active-phase work
  - bugfix work
  - direct prerequisite work needed to verify an active-phase exit criterion
- Implementation work should stay limited to the approved scope inside the active phase.
- Bugfix work is allowed at any time, but it does not advance the roadmap unless it closes a named exit criterion.
- A phase gate passes only when:
  - every exit criterion in this file is satisfied
  - blocking risks in `docs/RISK_REGISTER.md` are reduced enough for honest forward movement
  - QA or validation evidence exists for the changed behavior
  - the maintainer signs off on the phase-status change
- Current phase status is updated only by editing this file and `docs/SPRINT_BASELINE.md` together.

## 5. MVP Honesty Gate
The project must not be described as a product-facing MVP until all of these are true:

- one-track workflow is complete and stable
- startup is reproducible without hidden developer rituals
- the AI action flow is visible and understandable
- preview / approve / rollback trust UX exists
- confidence and degraded behavior are explicit
- supported plugin behavior is honest about limits
- recovery from AI changes is safe
- the system works as a product loop, not only as a developer-operated prototype

Before that point, the honest label remains: strong internal prototype.

## 6. Phase Model
### Phase 0 - Product Truth Lock
- Purpose: lock product truth, MVP boundary, and trust boundary.
- Why it exists: prevent scope fantasy and fake-product language.
- In scope: product identity, one-track MVP contract, AI vs human roles, explicit non-goals.
- Out of scope: shipping UX, packaging, multi-vendor promises, autonomy features.
- Key deliverables: stable product vision, approved MVP honesty contract, approved roadmap structure.
- Entry criteria: prototype exists but product truth is still soft or contradictory.
- Exit criteria: core docs clearly define product boundary, trust contract, and non-goals.
- Dependency: none.
- Typical increment: truth-lock doc updates or scope cuts, not runtime feature work.
- Main risks: narrative drift, overstated product claims, undefined MVP boundary.

### Phase 1 - Prototype Stabilization
- Purpose: make the current technical nucleus reproducible, stable, and honestly bootable.
- Why it exists: a strong prototype is still fragile if it depends on hidden machine rituals or unstable lifecycle behavior.
- In scope: path/bootstrap hardening, stable manual bring-up, backend reachability diagnostics, scan/startup/restore hardening, reproducibility checks.
- Out of scope: preview/confirm UX, confidence UX, auto-launch packaging, beginner UX, multi-vendor promises.
- Key deliverables: no known machine-specific runtime path assumptions in the canonical startup flow, a documented bring-up contract, explicit backend failure states, regression-safe startup and restore behavior.
- Entry criteria: product truth is locked and the technical nucleus already exists.
- Exit criteria: a non-author developer can boot the system from the documentation without hidden path rituals; critical startup/scan/restore flows are stable enough to build on; unresolved blockers no longer force "developer-operated only" as the startup model.
- Dependency: Phase 0.
- Typical increment: one hardening increment, one startup contract improvement, or one regression-proofing increment tied to path, startup, scan, or restore.
- Main risks: legacy path dependencies, lifecycle regressions, hidden bootstrap assumptions, manual backend fragility.

### Phase 2 - Trust UX Foundation
- Purpose: make AI mutation visible, reviewable, and reversible.
- Why it exists: the product promise is trust-first assistance, not silent automation.
- In scope: preview, confirm, rollback, confidence visibility, protected-state visibility, safe apply semantics, clear separation of chat reset vs chain reset.
- Out of scope: broad onboarding, semi-autonomy, packaging, full product polish.
- Key deliverables: visible proposal before apply, user approval before destructive changes by default, rollback path, explicit uncertainty and degraded-state cues.
- Entry criteria: Phase 1 is stable enough that UX work is not sitting on broken startup assumptions.
- Exit criteria: AI changes are reviewable before apply; low-confidence behavior is visible; rollback works; the trust contract is observable in the UI.
- Dependency: Phase 1.
- Typical increment: one trust-critical user action path closed end to end, or one safety/visibility increment with QA coverage.
- Main risks: silent mutation, user confusion, apply-time regressions, hidden uncertainty.

### Phase 3 - MVP Vertical Slice
- Purpose: deliver the first honest end-to-end one-track workflow.
- Why it exists: the project becomes product-shaped only when one complete loop works.
- In scope: intent -> analysis -> proposal -> approval -> apply -> iterate for one bounded track flow.
- Out of scope: multi-track intelligence, mastering, broad plugin universality, onboarding intelligence, packaging polish.
- Key deliverables: one stable track workflow that uses real installed plugins, audio analysis, proposal review, safe apply, and revision loop.
- Entry criteria: Phase 2 trust UX exists in a real, testable form.
- Exit criteria: one-track end-to-end loop is coherent and stable under the approved MVP contract, even if broader productization work remains.
- Dependency: Phase 2.
- Typical increment: one completed user-loop segment or one integration-gap closure inside the vertical slice.
- Main risks: partial workflow masquerading as MVP, hidden operator intervention, brittle end-to-end coupling.

### Phase 4 - Plugin Intelligence V1
- Purpose: make plugin support honest, capability-aware, and controllable.
- Why it exists: vendor-neutral ambition becomes credible only when support levels are explicit.
- In scope: confidence model, plugin support tiers, metadata/control honesty, capability-aware behavior.
- Out of scope: broad vendor rollout without proof, autonomous behavior, beginner UX polish.
- Key deliverables: explicit support tiers, honest host-only vs configurable behavior, metadata visibility, safe degraded planning.
- Entry criteria: Phase 3 proves one real user loop already works.
- Exit criteria: plugin support limits are visible and enforced; confidence logic influences behavior honestly; unsupported cases degrade clearly.
- Dependency: Phase 3.
- Typical increment: one support-tier rule, one capability surface, or one metadata/control contract increment.
- Main risks: fake universality, hidden degraded behavior, vendor bias disguised as general support.

### Phase 5 - Productization Layer
- Purpose: turn the prototype loop into a self-contained local product flow.
- Why it exists: a useful workflow still fails the product bar if startup and lifecycle remain developer-operated.
- In scope: auto-launch, service lifecycle, local orchestration, stable config, packaging basics, clean startup/shutdown.
- Out of scope: new product surface area, vendor expansion, autonomy.
- Key deliverables: stable backend launch model, clear service health/error UX, config discipline, packaging basics for repeatable setup.
- Entry criteria: the vertical slice exists and trust UX is already real.
- Exit criteria: the one-track workflow starts and runs without hidden operator rituals; local service lifecycle is stable; setup is repeatable enough for honest MVP exposure.
- Dependency: Phases 1 through 4.
- Typical increment: one startup/lifecycle increment, one config hardening increment, or one packaging-basics step tied to MVP readiness.
- Main risks: background-service flakiness, startup race conditions, config drift, packaging overreach.

### Phase 6 - Evaluation System
- Purpose: create regression truth for what counts as a strong result.
- Why it exists: without evaluation, "works well" becomes wishful thinking.
- In scope: benchmarks, validation cases, regression suites, quality gates, comparison baselines.
- Out of scope: new UX layers, autonomy, unsupported product claims.
- Key deliverables: representative test corpus, benchmark criteria, repeatable quality checks, regression reporting.
- Entry criteria: there is a stable product loop worth measuring.
- Exit criteria: the team can detect quality regressions and defend "strong result" with evidence instead of anecdotes.
- Dependency: Phase 5.
- Typical increment: one benchmark slice, one regression harness increment, or one validation rule tied to user-visible quality.
- Main risks: subjective success criteria, untracked regressions, overclaiming result quality.

### Phase 7 - Beginner-First UX Intelligence
- Purpose: make the product usable by a non-expert without hiding control boundaries.
- Why it exists: beginner support should clarify intent, not replace trust.
- In scope: intent clarification, guided phrasing, simple questions, novice-safe copy and flow.
- Out of scope: magic auto-mixing, hidden destructive automation, broad educational content.
- Key deliverables: guided prompting, ambiguity handling, simpler questions, novice-safe defaults that preserve review and approval.
- Entry criteria: trust, product loop, and evaluation foundations already exist.
- Exit criteria: a non-expert can reach a useful first result through guided clarification without losing visibility or approval control.
- Dependency: Phase 6.
- Typical increment: one clarification path, one guided-question pattern, or one beginner-friction removal tied to the active workflow.
- Main risks: over-simplification, hidden autonomy, confusing the user about what the system can actually do.

### Phase 8 - Honest Multi-Vendor Expansion
- Purpose: expand beyond the current Waves-heavy reality without pretending universal support.
- Why it exists: broader support must be earned with proof, not branding.
- In scope: proven vendor coverage, metadata/control support expansion, honest capability reporting across vendors.
- Out of scope: blanket universality claims, rushed UI complexity, unsupported automation modes.
- Key deliverables: validated vendor additions, updated support tiers, cross-vendor regression checks, honest catalog behavior.
- Entry criteria: Phase 4 support model and Phase 6 evaluation system are both real.
- Exit criteria: broader support is demonstrably working, clearly bounded, and measured; vendor-neutrality is confirmed in behavior, not only aspiration.
- Dependency: Phases 4 and 6.
- Typical increment: one vendor-support increment with evidence, one capability-contract update, or one regression-backed expansion step.
- Main risks: fake universality, metadata gaps, support claims outrunning validation.

### Phase 9 - Bounded Automation Mode
- Purpose: allow bounded "do it for me" behavior under explicit user-approved limits.
- Why it exists: autonomy without trust, evaluation, and product boundaries would be unsafe.
- In scope: bounded autonomy settings, approval boundaries, safe automation envelopes, explicit undo/control limits.
- Out of scope: open-ended autonomous mixing or hidden destructive behavior.
- Key deliverables: autonomy rules, permission boundaries, clear user controls, recovery path.
- Entry criteria: trust UX, productization, evaluation, and honest support boundaries are already mature.
- Exit criteria: the system can act with limited autonomy inside explicit guardrails, with visible boundaries and safe rollback.
- Dependency: Phases 2, 5, 6, and 8.
- Typical increment: one bounded automation capability or one guardrail-contract increment.
- Main risks: overreach, broken trust, unclear authority boundaries, unsafe silent behavior.

### Phase 10 - Product-Grade Release Candidate
- Purpose: reach the first honest release candidate.
- Why it exists: the goal is a real product bar, not a forever-prototype with impressive internals.
- In scope: final hardening, release honesty, packaging completion, RC validation, support boundary clarity.
- Out of scope: new major product scope.
- Key deliverables: honest RC checklist, validated one-track MVP loop, stable startup, trust UX, evaluation evidence, support disclaimers.
- Entry criteria: phases 1 through 9 are complete enough that only release-grade hardening remains.
- Exit criteria: the product can honestly be called a release candidate without hiding prototype caveats.
- Dependency: Phases 1 through 9.
- Typical increment: one RC blocker removed, one release-validation increment, or one honesty-gap closure.
- Main risks: premature release claims, packaging instability, unclosed trust or quality gaps.
