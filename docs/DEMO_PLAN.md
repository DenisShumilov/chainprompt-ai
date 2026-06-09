# Demo Plan

This is the recommended live-demo path for a 60-90 second walkthrough.

The current slide-based explainer can support context, but the hiring/outreach demo should mostly use live screen recording: backend terminal, `/health`, plugin or standalone UI, prompt, payload/diff, manual bypass/tweak, revision, and `/api/diag`.

## Demo Goal

Show that ChainPrompt AI is not just a chat UI. It is a local, host-aware prototype that uses plugin inventory, health diagnostics, and bounded chain planning.

## Scenario

**Prompt:** "Build a clean modern vocal chain using installed plugins."

The strongest demo moment is not the first generated chain. It is the revision step where a user manually tweaks or bypasses a slot, asks for a change, and the system preserves or explicitly reports tracked user state.

## Screen Flow

### 1. Opening

Show:

- README first screen or plugin window.
- Project running locally.

Voice:

"This is ChainPrompt AI, a working developer prototype for AI-assisted insert-chain building inside a DAW."

### 2. Problem

Show:

- Empty or simple chain.
- Backend `/health` endpoint or plugin diagnostic status.

Voice:

"Generic AI can suggest a vocal chain, but it usually does not know what is installed, what the current chain contains, or which settings the user already changed."

### 3. Inventory and Health

Show:

- Backend running on `127.0.0.1:5000`.
- `/health`.
- Plugin rescan flow if available.

Voice:

"This prototype uses local plugin inventory as a planning boundary. If inventory is unavailable, the backend fails closed instead of asking the model to guess."

### 4. Build Request

Show:

- User prompt.
- AI response.
- Chain payload or chain UI update.

Voice:

"The request goes through staged orchestration: intent routing, UID validation, chain diffing, targeted parameter generation, and safety filtering."

### 5. Preserve User Intent

Show:

- Manually bypass or tweak a slot.
- Ask for a revision.
- Show preservation note or unchanged user-controlled slot.

Voice:

"The goal is bounded mutation, not autonomous mixing. User-owned edits and bypass state should survive AI revisions."

### 6. Diagnostics

Show:

- `/api/diag` or plugin diagnostic status.

Voice:

"The prototype exposes health and slot diagnostics because degraded states should be visible in a real creative workflow."

### 7. Closing

Show:

- README, case study, and repository.

Voice:

"This is a portfolio prototype, not a finished product. It explores how AI assistance inside DAWs can be scaffolded by host truth, validation, diagnostics, and user-state preservation."

## Captions

- "Portfolio prototype, not production-ready"
- "Local plugin inventory"
- "Fail-closed if inventory is missing"
- "Chain diff instead of overwrite"
- "User edits preserved where tracked"
- "Diagnostics visible"

## Final CTA

"Open to feedback from music tech product and engineering teams."
