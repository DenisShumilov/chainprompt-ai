# Validation Plan

This document lists the validation work needed before ChainPrompt AI should be treated as more than a developer-operated prototype.

## Current Verified Scope

- Backend unit tests for chain diff, chat pipeline gating, and safety filtering.
- Local backend health endpoint.
- Manual startup path documented in `README.md`.
- Local plugin inventory and rescan flow represented in architecture docs.

## Test Command

```bash
python -m pytest server/tests
```

Last observed local result on 2026-06-09:

```text
49 passed
```

## Audio Plugin Validation Needed

- Run pluginval for VST3 builds.
- Validate AU target on macOS when available.
- Test variable sample rates.
- Test variable buffer sizes.
- Test offline render or bounce behavior.
- Test plugin scan failure behavior.
- Confirm HTTP, scanning, file I/O, and model calls are off the real-time audio callback.
- Review host notification behavior such as parameter updates from runtime queues.

## Host Compatibility Matrix

Use this table for each tested host:

| Host | Version | OS | Format | Loads | Scan Works | Chat Works | Apply Works | Save/Reopen | Known Issues |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| REAPER | TBD | Windows | VST3 | TBD | TBD | TBD | TBD | TBD | TBD |
| Studio One / Fender Studio Pro | TBD | Windows | VST3 | TBD | TBD | TBD | TBD | TBD | TBD |
| Ableton Live | TBD | Windows | VST3 | TBD | TBD | TBD | TBD | TBD | TBD |
| Logic Pro | TBD | macOS | AU | TBD | TBD | TBD | TBD | TBD | TBD |

## State and Restore Tests

- Save and reopen a session with an AI-built chain.
- Save and reopen after manual parameter changes.
- Save and reopen after bypass changes.
- Duplicate the same plugin in multiple slots.
- Reorder plugins and verify slot identity.
- Remove a plugin from the system and reopen.
- Rescan after reopen.
- Undo/redo after chain mutation.
- Verify automation continuity where applicable.

## AI Workflow Tests

- Missing inventory should block build requests.
- Corrupt inventory should block build requests.
- Unsupported plugins should be reported honestly.
- Non-FX plugins should be excluded or clearly marked unsupported.
- User-touched slots should not be unnecessarily re-tuned.
- Bypassed slots should preserve bypass state unless explicitly changed.
- Excessive or duplicate chains should be trimmed by safety logic.

## Privacy and Security Tests

- Confirm `.env` is ignored.
- Confirm no API keys appear in logs, screenshots, or history.
- Review debug logging for prompt snippets, model output snippets, track names, and audio paths.
- Restrict or remove `audioPath` before broader testing.
- Keep backend bound to localhost unless authentication and transport security are added.

## Release Readiness Gate

Do not describe the project as product-ready until:

- setup is reproducible on a clean machine
- plugin validation is documented
- host matrix has real results
- preview/approve/rollback UX exists
- backend lifecycle is packaged or clearly automated
- privacy behavior is user-facing and configurable
- support tiers are visible in the UI and docs
