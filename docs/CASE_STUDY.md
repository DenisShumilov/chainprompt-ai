# ChainPrompt AI Case Study

## Context

ChainPrompt AI is an R&D prototype exploring host-aware AI assistance for music production workflows.

The project combines a JUCE C++ audio plugin, configured VST3/AU/Standalone targets, a local Python Flask/Waitress backend, Gemini API orchestration, local plugin inventory scanning, and diagnostics endpoints. The current implementation is VST3-first and not broadly host-validated yet.

The current scope is intentionally narrow: serial insert-chain work on the current track.

Vendor coverage is Waves-first: Waves VST3 plugins get full AI parameter generation through extracted 1001-point parameter metadata, while plugins from other vendors are inventoried and hostable but load with factory defaults until metadata coverage expands.

## Problem

AI can generate chain ideas easily, but a DAW workflow needs more than a plausible answer.

A useful assistant needs to know:

- which plugins are actually installed
- whether local inventory is available
- what the current insert chain contains
- which settings the user manually changed
- when the system should refuse to act instead of guessing

The central product problem was:

**How can an AI assistant help build insert chains while staying bounded, visible, and honest about failure states?**

## Approach

I treated the model as one component inside a larger safety scaffold, not as the source of truth.

The prototype is built around three boundaries:

- **Host truth:** installed plugin inventory and current chain state are authoritative.
- **Audio truth:** measured signal context can inform decisions when available.
- **User intent truth:** manual edits, user-owned slots, and bypass state should be preserved where the current prototype can track them.

Instead of building a broad "AI mixer", ChainPrompt AI focuses on a practical wedge: proposing and revising serial insert chains on one track.

## Technical Architecture

The system uses a two-process local architecture:

- **JUCE plugin:** chain hosting, local plugin scanning, health polling, UI, and HTTP coordination.
- **Python backend:** AI orchestration, manifest loading, catalog filtering, chain diffing, diagnostics, and state.

Important modules:

- `GlobalPluginCache` for local plugin inventory.
- `LLMCoordinator` for plugin-to-backend requests.
- `BackendHealthChecker` for backend and inventory status.
- `chat_pipeline.py` for request orchestration.
- `inventory_manifest.py` for manifest-first catalog authority.
- `chain_diff.py` for current-chain vs. desired-chain mutation.
- `stage2_engineer.py` for parameter suggestions.
- `iron_dome.py` for safety filtering.

## Scaffolded AI Workflow

The useful behavior does not come from asking a model to "mix this track."

It comes from the rules and checks around the model:

- fail-closed inventory gate before LLM calls
- manifest-backed plugin selection
- UID validation
- chain diffing instead of free-form overwrite
- targeted parameter generation only where needed
- preservation attempts for user-touched and bypassed slots
- backend and inventory diagnostics
- explicit prototype limitations

This is the product thesis: AI becomes more useful inside a DAW when it is constrained by host state, validation rules, and recovery paths.

## Main Challenges

- Preventing hallucinated plugins.
- Avoiding LLM calls when inventory is unavailable.
- Preserving manual user changes.
- Keeping chain changes bounded.
- Making backend failure visible.
- Managing local state across plugin and backend processes.
- Separating configured build targets from proven host compatibility.

## What Works Now

- Backend starts locally on `127.0.0.1:5000`.
- `/health` reports backend, model, catalog, and inventory state.
- Plugin inventory can be scanned and hot-reloaded through `/api/rescan`.
- Chat/build requests go through an early fail-closed inventory gate.
- Selected plugin UIDs are validated before payload generation.
- The chain diff engine separates retained, inserted, removed, and reordered slots.
- User-touched and bypassed slots are protected from unnecessary re-tuning in the tested backend paths.
- Backend tests cover critical chain diff, safety, and orchestration behavior.

## What I Would Improve Next

- Remove remaining machine-specific path assumptions.
- Add a reproducible evaluation fixture for integration teams.
- Add preview, approve, and rollback UX.
- Package backend startup instead of requiring manual Python startup.
- Add plugin support tiers: configurable metadata, host-only, unsupported, excluded non-FX.
- Run pluginval and create a DAW compatibility matrix.
- Audit real-time callback boundaries and host notification behavior.
- Improve privacy controls around audio payloads, local paths, and logs.

## Relevance for Modern DAWs

In-DAW AI features need more than model output. They need host-aware execution, diagnostics, permission boundaries, recoverability, and trust UX.

ChainPrompt AI explores one narrow but important slice of that direction: bounded AI assistance for insert-chain work using local plugin inventory and explicit safety checks.

## My Role

I designed and built the prototype end to end:

- product concept and scope
- JUCE/C++ plugin structure
- Python backend architecture
- Gemini API integration
- local plugin inventory flow
- health diagnostics
- chain diff and safety logic
- technical documentation
- scaffolded AI workflow design
