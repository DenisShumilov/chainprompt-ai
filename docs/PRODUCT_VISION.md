# Product Vision

## 1. Product Identity
ChainPrompt AI is a trust-first AI copilot for serial insert chains inside a DAW.

Its job is not open-ended autonomous mixing. Its job is bounded chain mutation against a real host, a real plugin inventory, measured signal context when available, and durable user-owned state.

The product should be understood as a safe execution layer for chain assistance, not as "chat with plugins."

## 2. One-Sentence Definition
ChainPrompt AI safely builds and mutates serial DAW insert chains while preserving host truth, audio truth, and user intent.

## 3. Purpose
The product exists to remove friction from insert-chain work without making the session unsafe.

It should help users:
- choose from real installed tools
- assemble sensible serial chains
- generate useful starting settings
- revise chains faster
- preserve manual decisions

The problem it solves is execution trust, not idea generation alone.

## 4. Core Truth Boundaries
Three forms of truth define the product:

- Host Truth: only real installed plugins, real chain state, and real host capability are authoritative.
- Audio Truth: measured signal context should inform decisions when capture and analysis are available.
- User Intent Truth: manual edits, user-added slots, and explicit bypass choices are protected state.

The model may assist reasoning, but it must not override those truths.

## 5. Product Principles
- Safety over surprise.
- Controlled mutation over unrestricted generation.
- Real inventory over hallucinated tools.
- User intent must survive AI actions.
- Capability-aware honesty over fake confidence.
- Saved work must survive backend loss.
- Degraded behavior must be visible, not silent.

## 6. Current Product Boundary
Current scope is intentionally narrow:

- serial insert chains only
- current track only
- local plugin runtime plus local backend orchestration
- bounded AI-assisted chain mutation
- offline audio analysis when explicitly requested

Out of scope for the current phase:
- sends and returns
- sidechain routing
- unconstrained session-wide autonomy
- master-bus autonomy
- "one-click full mix" behavior

## 7. Target Users
Primary users:
- producers
- mix engineers
- sound designers

They want faster chain building and safer iteration without surrendering control.

## 8. Success Criteria
The product is moving in the right direction when:
- planning only uses real installed plugins
- manual user work is preserved
- saved sessions remain recoverable
- degraded modes are visible and honest
- the single-track serial workflow becomes meaningfully faster without becoming less safe

## 9. Current Strategic Direction
The next milestone is a trustworthy MVP for single-track serial insert assistance.

That means:
- hardening path and bootstrap behavior
- reducing developer-only setup
- finishing the trust-critical UX
- preserving restore and mutation safety

Broader authority should come only after that narrower contract is reliable.
