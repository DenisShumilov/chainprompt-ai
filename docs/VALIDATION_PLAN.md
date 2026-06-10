# Validation Plan

This document lists the validation work needed before ChainPrompt AI should be treated as more than a developer-operated prototype.

## Current Verified Scope

- Backend unit tests for chain diff, chat pipeline gating, inventory/UID validation, and safety filtering.
- Local backend health endpoint (verified booting from a fresh clone + venv).
- Manual startup path documented in `README.md`.
- Local plugin inventory and rescan flow represented in architecture docs.
- Windows Release build of the VST3/Standalone targets from the documented CMake steps (2026-06-10).
- pluginval 1.0.4 (Windows x64, VST3, strictness level 5): all tests pass, clean exit.
- pluginval strictness level 10: all 25 test sections pass and the run reports SUCCESS; with GUI tests enabled the validator child process then exits nonzero at teardown (reproducible; a third-party control plugin exits cleanly on the same machine; with `--skip-gui-tests` the exit is clean). Bisection with `--disabled-tests` narrowed it down: each of the two editor-opening tests ("Editor", "Open editor whilst processing") reproduces the nonzero exit on its own, while runs without any editor open exit cleanly — the residue comes from the editor open/close cycle itself. No crash is recorded in Windows Error Reporting, so this is exit-time residue rather than an unhandled exception. On the audit list.
- Real-manifest inventory flow verified end-to-end on the dev machine (2026-06-10): Standalone full VST3 scan (731 plugins, 728 Waves "configurable", 3 host-only, 0 failed files) → manifest v2 export → `POST /api/rescan` hot-reload → `/health` reports `inventory.available: true`. The fail-closed gate opens with real host truth; LLM steps were not exercised in this environment (no API key).

## Test Command

```bash
python -m pytest server/tests
```

Last observed local result on 2026-06-09:

```text
55 passed
```

## Audio Plugin Validation Needed

- Resolve the strictness-10 GUI-stress teardown exit (see Current Verified Scope) and re-run pluginval.
- Validate AU target on macOS when available.
- Test variable sample rates.
- Test variable buffer sizes.
- Test offline render or bounce behavior.
- Test plugin scan failure behavior.
- Confirm HTTP, scanning, file I/O, and model calls are off the real-time audio callback.
- Review host notification behavior such as parameter updates from runtime queues (known: FIFO preset application calls `setValueNotifyingHost` on hosted parameters from the audio thread).
- Add latency aggregation/reporting for hosted chains (no plugin delay compensation today).
- Validate mono insert configurations (the internal graph is currently stereo-only).
- Expected failure to document: restoring a session that references an uninstalled plugin currently rejects the whole chain; re-saving in that state writes an empty chain.

## Host Compatibility Matrix

Use this table for each tested host:

| Host | Version | OS | Format | Loads | Scan Works | Chat Works | Apply Works | Save/Reopen | Known Issues |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| REAPER | 7.73 | Windows 11 | VST3 | Yes (2026-06-10) | TBD | partial: backend-offline diagnostics render in chat | TBD | TBD | none observed at load |
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
- Review debug logging for prompt snippets, model output snippets, and track names (debug logging is opt-in via `CHAINPROMPT_DEBUG=1`).
- Confirm `audioPath` stays restricted to the plugin capture directory (`<temp>/ChainPromptAI/`).
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
