# ChainPrompt AI — Architecture (Sprint 14 Baseline)

> Last updated: 2026-03-10 — Universal Scanner-to-Catalog Spine
> Sprint numbers refer to internal development iterations; earlier sprint notes are not part of this repository.

## System Overview

Two-process architecture: JUCE 8 plugin (C++) + Python Flask backend.

```
DAW Host
  |
  ChainPrompt AI (VST3/Standalone)
  |   - AudioProcessorGraph (serial insert chain, stereo-only)
  |   - ChainGraphRebuilder (topology mutations, bypass, wiring)
  |   - AutomationPoolRouter (stable {slotId,paramIndex}→pool mapping)
  |   - AIIntentTracker (snapshot/detect user parameter touches)
  |   - ChainStateSerializer (DAW save/load, undo capture)
  |   - StudioTelemetryBridge (background ping + payload assembly)
  |   - LLMCoordinator (HTTP → Python)
  |   - GlobalPluginCache (vendor-neutral VST3 scanner + differential rescan + manifest v2)
  |   - DynamicSlotParameter[1024] (automation pool)
  |   - SlotStateOps.h (slot reconciliation helpers)
  |
  |   GUI Layer (Sprint 12: decomposed editor)
  |   - ChainPromptAudioProcessorEditor (composition root, ~440 lines)
  |   - SidebarPresenter (chain slot widgets, add-plugin menu, reorder/delete)
  |   - HostedEditorController (native plugin editor lifetime + viewport)
  |   - RescanWorkflowController (rescan lifecycle: scan → diff → notify Python)
  |   - UIRefreshController (timer polling: capture state, AI thinking, undo/redo)
  |
  HTTP (localhost:5000)
  |
  Python Backend (Flask/Waitress) — Sprint 14: manifest-first catalog
      TRANSPORT LAYER:
      - app.py               → Thin HTTP routes (~200 lines)

      ORCHESTRATION:
      - chat_pipeline.py     → Chat/Build pipeline (early fail-closed gate)

      DOMAIN SERVICES:
      - unified_agent.py     → Stage 1: LLM routing + selection
      - chain_diff.py        → Diff engine: current vs desired + actions
      - stage2_engineer.py   → Stage 2: parameter generation coordinator
      - stage2_context.py    → Stage 2 context: prompts, dictionaries, RAG
      - iron_dome.py         → Safety filters

      DATA AUTHORITIES:
      - inventory_manifest.py → Installed plugin manifest (fail-closed gate + FX eligibility)
      - plugin_catalog.py     → Manifest-first catalog, optional master DB enrichment
      - studio_memory.py      → Thread-safe SQLite: cross-track memory (slot-centric)

      INFRASTRUCTURE:
      - gemini_client.py     → Gemini API wrapper with fallback
      - config.py            → API keys, schemas, paths

      SHIMS (backward compat, deprecated):
      - db.py                → Re-exports from inventory_manifest + plugin_catalog
      - studio_db.py         → Re-exports from studio_memory
```

## Manifest-First Catalog Authority (Sprint 14)

Sprint 14 inverted the catalog trust model. The installed plugin manifest is now the
**base catalog authority**, while vendor-specific databases (`data/catalogs/Waves_Master_DB.json`,
`data/metadata/Waves_Meta_GroundTruth.json`, `data/catalogs/Waves_HighRes_DB.json`) are optional enrichment overlays.

### Before (Sprint 13)
- Master DB (`data/catalogs/Waves_Master_DB.json`) was the backbone — loaded unconditionally at startup
- Manifest only filtered the master DB — non-Waves plugins were invisible to the LLM
- Instrument exclusion used Waves-specific category heuristics (`Fx|Surround`, `Instrument|Waves`)
- Inventory check happened AFTER the LLM call — wasted tokens on builds that would be rejected

### After (Sprint 14)
- Manifest is the catalog authority — all installed VST3 plugins appear in the catalog
- Master DB loads optionally — enriches with parameter data, descriptions, tags
- Manifest-only plugins get basic catalog entries (name, manufacturer, category from manifest)
- Instrument/surround exclusion uses vendor-neutral manifest fields (`isInstrument`, `numOutputChannels`)
- Inventory check happens BEFORE the LLM call — zero tokens wasted

### Data Flow: Catalog Assembly

```
installed_plugins.json (C++ manifest v2)
  ↓
inventory_manifest.py: load UIDs + plugin info + FX eligibility sets
  ↓
plugin_catalog.py:
  1. Load data/catalogs/Waves_Master_DB.json (optional enrichment → plugins_by_uid, _all_variants)
  2. Load Meta GroundTruth (optional enrichment → descriptions, tags)
  3. Inject manifest-only plugins into _all_variants (vendor-neutral)
  4. Build _uid_format_map (mono/stereo swap)
  5. Build _NORMALIZED_VARIANTS (fuzzy name resolution)
  6. Load metadata envelopes (HighRes parameter maps)
  7. Build filtered catalogs (chain-eligible + installed + format-aware)
  ↓
unified_agent.py: cached system prompt + catalog → LLM
```

## Early Fail-Closed Build Gate (Sprint 14)

**Critical property**: no LLM tokens are spent before host truth is validated.

```
User prompt → chat_pipeline.run_chat_pipeline()
  → Context assembly (metrics, history, preferences)
  → is_inventory_available()? ─── NO → return ⚠️ "scan your plugins first"
  │                                      (zero LLM tokens spent)
  YES
  ↓
  → unified_agent (Stage 1 LLM call)
  → route == "chat"? → return reply
  → route == "build"? → validate_uids → diff → Stage 2 → merge → return preset
```

| State | `is_inventory_available()` | Pipeline behavior |
|-------|---------------------------|-------------------|
| Manifest loaded | `True` | Full pipeline (chat + build) |
| Manifest missing | `False` | Blocked immediately, zero LLM tokens |
| Manifest corrupt | `False` | Blocked immediately, zero LLM tokens |

Gate location: `chat_pipeline.py`, before `unified_agent()` call.
Regression test: `server/tests/test_chat_pipeline.py` (3 tests).

## Universal FX Eligibility Filter (Sprint 14)

Plugin eligibility for the LLM catalog is now vendor-neutral, using manifest metadata
instead of Waves-specific category heuristics.

| Filter | Old (Sprint 13) | New (Sprint 14) |
|--------|-----------------|-----------------|
| Instruments | `_SKIP_CATEGORIES = {"Instrument|Waves", "Instrument|Synth"}` | `isInstrument == True` from manifest |
| Surround | `_SKIP_WORDS = {"5.0", "5.1", "7.0", ...}` | `numOutputChannels > 2` from manifest |
| Platform-specific | `_SKIP_WORDS = {"SoundGrid", "DiGiGrid"}` | Retained as pre-filter for master DB entries |

Functions in `inventory_manifest.py`:
- `is_fx_eligible(base_name)` → False for instruments and surround plugins
- Uses cached `_installed_instruments` and `_installed_surround` sets (built at manifest load)

## Metadata Hot-Reload (Sprint 14)

The `/api/rescan` endpoint now performs a complete hot-reload of all data layers
without requiring a backend restart:

```
POST /api/rescan
  → reload_installed_manifest()     — manifest UIDs, capability tiers
  → reload_metadata_envelopes()     — HighRes parameter maps (NEW in Sprint 14)
  → _rebuild_cached_catalogs()      — re-inject manifest plugins, format maps, catalogs
  → rebuild_cached_prompts()        — Stage 1 system prompt + catalog
  → rebuild_param_cache()           — Stage 2 per-UID parameter dictionaries
```

Before Sprint 14, `reload_metadata_envelopes()` was only called at import time.
Fresh extractor output placed into `data/catalogs/Waves_HighRes_DB.json` (or any provider file)
was invisible until restart. Now it's picked up by `/api/rescan`.

## Differential Rescan Contract (Sprint 14)

### C++ Side: GlobalPluginCache

Sprint 14 upgraded `requestRescan()` from "nuke everything and full scan" to true
differential scanning:

| Aspect | Old (Sprint 13) | New (Sprint 14) |
|--------|-----------------|-----------------|
| knownPluginList | Cleared before rescan | Preserved (scanner skips known unchanged files) |
| Cache file (PluginCache.xml) | Deleted before rescan | Preserved (scan history retained) |
| Dead-man's pedal | Deleted | Deleted (retry crashed plugins) |
| Scan speed | Full: scans ALL .vst3 files | Differential: skips unchanged files |
| Plugin removal | Not detected | Pruned: file-existence check after scan |
| Version updates | Not detected | Detected: pre/post version comparison |

### RescanDiff Contract

```cpp
struct RescanDiff {
    StringArray added;      // new plugins discovered
    StringArray removed;    // plugins whose .vst3 files were deleted
    StringArray updated;    // plugins whose version string changed
    int totalInstalled;     // total after scan
    String scanMode;        // "full_vst3" or "differential"
};
```

The diff is computed by comparing pre-scan snapshots (UIDs, names, versions) with
post-scan state. The UI displays all three categories (added/removed/updated).

### Manifest scanMode Field

The `installed_plugins.json` manifest now reports the actual scan mode:
- `"full_vst3"` — first scan (no prior knownPluginList)
- `"differential"` — subsequent rescans (scanner skipped known unchanged files)

### Python Side: Enriched Rescan Response

`/api/rescan` returns an enriched summary including:
- `scan_mode`: from manifest ("full_vst3" or "differential")
- `extraction_backlog`: count of plugins needing metadata extraction
- `metadata_backlog`: full breakdown (ready, needs_extract, stale, by_provider)

## Processor Submodules (Sprint 11)

Sprint 11 decomposed the 1615-line `ChainPromptAudioProcessor` god-file into
5 focused modules. The processor is now a thin coordinator (~800 lines) that
delegates to these modules:

| Module | Pattern | Owns | Responsibilities |
|--------|---------|------|------------------|
| `ChainStateSerializer` | Namespace (free functions) | Nothing (stateless) | DAW save/load, undo state capture |
| `AIIntentTracker` | Header-only class | aiSnapshot[16] | Snapshot AI params, detect user touches |
| `StudioTelemetryBridge` | Header-only class | PingTimer | Background ping, payload assembly, session scope |
| `AutomationPoolRouter` | Header-only class | Registry map | Stable automation remap across topology changes |
| `ChainGraphRebuilder` | Class with .cpp | Rebuild state machine | Topology apply, node recycling, wiring, bypass |

### Dependency Pattern

- **ChainGraphRebuilder** is a `friend` of the processor (needs access to private graph state)
- All other modules take explicit parameters — no circular dependencies
- `AudioMetrics` extracted to own header for clean cross-module access

### Contract Changes

- **Sidechain bus DELETED** from processor constructor and `isBusesLayoutSupported`
- **Stereo-only graph policy** made explicit: all hosted plugins get stereo layout forced
- **Session scope**: explicit settable token (default: PID-based). Future Master Mode
  can override `projectScope` with a real project hash without touching the bridge

## Editor Submodules (Sprint 12)

Sprint 12 decomposed the 903-line editor god-file into a composition root plus
4 focused controllers. The editor is now ~440 lines and owns only visual components
and callback wiring.

| Module | Pattern | Owns | Responsibilities |
|--------|---------|------|------------------|
| `SidebarPresenter` | .h/.cpp class | chainSlots OwnedArray | Slot widget creation, refresh, delete, reorder, add-plugin menu |
| `HostedEditorController` | Header-only class | hostedEditor unique_ptr | Native plugin editor show/clear, viewport management |
| `RescanWorkflowController` | .h/.cpp class | Rescan listener ID | Start rescan, format diff message, notify Python, trigger sidebar refresh |
| `UIRefreshController` | Header-only class | Button references | Timer polling: capture state, AI thinking, undo/redo, rescan button |

## Installed Plugin Inventory

The inventory boundary is the **trust boundary** between what the host can instantiate
and what the AI is allowed to plan. It is vendor-neutral, fail-closed, manifest-first,
and differential-scan-ready.

### Manifest Contract (v2)

`installed_plugins.json` written by C++ after every scan, read by Python as the single
source of truth. Located at `%APPDATA%/ChainPrompt/installed_plugins.json`.

Fields per plugin: `name`, `uid`, `category`, `manufacturer`, `format`, `pluginFile`,
`isInstrument`, `numInputChannels`, `numOutputChannels`, `version`.

Manifest-level fields: `version` (2), `timestamp`, `scanMode` ("full_vst3" | "differential"),
`plugins[]`, `failedFiles[]`.

### Fail-Closed Inventory Gate

**Critical safety property**: the AI cannot plan chains without authoritative host truth.
The gate is checked BEFORE any LLM call (zero token waste).

Gate functions in `inventory_manifest.py`:
- `is_inventory_available()` → checked at top of pipeline, before unified_agent
- `is_uid_installed(uid)` → `False` when no manifest
- `validate_uids(uids)` → all UIDs invalid when no manifest
- `is_fx_eligible(base_name)` → vendor-neutral instrument/surround filter

### Plugin Capability Tiers

| Tier | Meaning | Catalog Badge | Stage 2 Behavior |
|------|---------|---------------|------------------|
| **configurable** | Installed + has HighRes parameter maps | ★★★/★★/★ | Full LLM parameter generation |
| **hostable** | Installed but no parameter maps | `[HOST ONLY]` | Factory defaults (empty params) |
| **unavailable** | Not in manifest | Hidden from catalog | Rejected at fail-fast gate |

Functions: `get_plugin_capability(uid)`, `get_base_name_capability(base_name, uid_format_map)`

## Slot-State Contract

Every plugin slot carries authoritative state that round-trips through the system:

| Field | Type | Meaning |
|-------|------|---------|
| `uid` | int | Plugin uniqueId (from JUCE scan) |
| `slot_id` | int | **Authoritative** monotonic identity (survives reorder, persist in DAW state) |
| `bypassed` | bool | Disconnected from signal chain |
| `user_touched` | bool | User modified params since last AI action |
| `user_owned` | bool | User manually added this plugin |
| `action` | string | Transport-only: keep/insert/bypass/enable/reconfigure |

### Bypass Contract (Tri-State)

| Value of `desired_bypassed_positions` | Meaning | Behavior |
|---------------------------------------|---------|----------|
| `None` | LLM didn't mention bypass | Preserve current bypass state (no changes) |
| `set()` (empty) | LLM explicitly returned `routing.bypassed: []` | Enable all currently bypassed slots |
| `{1, 3}` | LLM specified positions | Bypass pos 1,3; enable others |

## Data Flow

### Chat/Build Flow
```
User prompt + chain snapshot (rich entries with slot_id + intent flags)
  → C++ LLMCoordinator → HTTP POST /api/chat
  → app.py (transport) → chat_pipeline.run_chat_pipeline()
  → Early fail-closed gate: is_inventory_available() — blocks if no manifest (zero LLM tokens)
  → unified_agent (Stage 1 LLM: routing.main + routing.bypassed)
  → Fail-fast: validate_uids — reject unavailable plugins
  → chain_diff (compute_chain_diff: current vs desired + tri-state bypass → actions)
  → stage2_engineer (capability-aware: configurable → LLM, host-only → factory defaults)
  → iron_dome (safety filter)
  → build_merged_preset (merge retained params + Stage 2 + actions)
  → JSON response {reply, preset: {plugins: [{uid, slot_id, params, action}]}}
```

### Ping Flow (every 15s)
```
C++ PingTimer → detectUserTouches() → HTTP POST /api/ping
  → studio_memory.upsert_mix_state (thread-safe SQLite, rich slot entries)
```

### Rescan Flow
```
User clicks Rescan → GlobalPluginCache.requestRescan()
  → Differential scan (keeps knownPluginList, skips unchanged files)
  → Prune removed plugins (file-existence check)
  → Export manifest v2 (scanMode = "differential" or "full_vst3")
  → RescanDiff (added/removed/updated + scanMode)
  → RescanWorkflowController → display diff in chat
  → POST /api/rescan → Python hot-reload:
      reload manifest → reload metadata → rebuild catalogs → rebuild prompts → rebuild param cache
```

## API Endpoints

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/api/chat` | POST | Main prompt → chat_pipeline → response |
| `/api/ping` | POST | Background metrics + chain state (rich slot entries) |
| `/api/tracks` | GET | List all tracked instances (`?scope=&fresh=1`) |
| `/api/diag` | GET | Slot-centric diagnostics (`?scope=&detail=1`) |
| `/api/rescan` | POST | Hot-reload manifest + metadata + catalogs + prompts |
| `/health` | GET | Server status + structured inventory object |

## Key Constraints

- **Audio-thread discipline**: the plugin's own DSP, metrics, and capture code in processBlock is allocation-free and lock-free (preallocated buffers, atomics, AbstractFifo). Known exception under audit: applying FIFO presets and forwarding pool automation calls `setValueNotifyingHost` on hosted plugin parameters from the audio thread, which takes JUCE's parameter-listener lock and runs listener code. Flagged in README limitations and the validation plan.
- **Lock-free FIFO**: ParamPreset (POD-only) transfers LLM→audio thread
- **Message thread**: All topology changes, undo/redo, state save/load
- **Flat chain only**: No sends, no sidechain routing
- **Sends/Sidechain FULLY DELETED**: No bus, no structs, no graph code, no serialization
- **Stereo-only graph**: All hosted plugins get stereo layout forced
- **ModalDismissTimer DISABLED**: Do not re-enable without GUI management
- **Slot identity is authoritative**: position is for display only, slot_id is for identity
- **Inventory fail-closed**: No manifest = no pipeline (zero LLM tokens before host truth)
- **Manifest-first catalog**: Master DB is enrichment, manifest is authority
- **FX eligibility vendor-neutral**: isInstrument + numOutputChannels, not Waves categories
- **Metadata hot-reloadable**: /api/rescan reloads all data layers without restart
- **Differential rescan**: Scanner skips known unchanged files, prunes removed, detects updates
- **Studio Memory thread-safe**: All SQLite ops under threading.Lock
- **Rescan multi-listener**: Never use single-callback pattern for scan notifications

## File Map

### C++ Core
- `PresetData.h` — POD structs: ChainPreset, PluginConfig, ParamPreset, slotAction[]
- `AudioMetrics.h` — Audio analysis metrics struct (extracted from processor)
- `SlotStateOps.h` — Slot reconciliation helpers (header-only, pure functions)
- `LLMCoordinator.h/cpp` — HTTP client, JSON parse, SlotActions constants
- `ChainPromptAudioProcessor.h/cpp` — **Thin coordinator** (~800 lines)
- `ChainGraphRebuilder.h/cpp` — Topology mutations, node recycling, bypass wiring
- `ChainStateSerializer.h` — DAW save/load + undo state capture
- `AIIntentTracker.h` — AI parameter snapshot + user-touch detection
- `AutomationPoolRouter.h` — Stable {slotId, paramIndex} → pool slot registry
- `StudioTelemetryBridge.h` — Background ping, payload assembly, session scope
- `DynamicSlotParameter.h` — Automation pool slots with stable identity
- `GlobalPluginCache.h` — Vendor-neutral VST3 scanner (differential rescan + multi-listener)
- `AudioCaptureManager.h` — Smart Wait audio capture
- `OfflineAudioAnalyzer.h/cpp` — 31-band spectrum, MD-TSER, OE-HER, BW-ICC

### C++ GUI
- `ChainPromptAudioProcessorEditor.h/cpp` — Composition root (~440 lines)
- `SidebarPresenter.h/cpp` — Chain slot widgets, add-plugin menu
- `HostedEditorController.h` — Native plugin editor lifetime
- `RescanWorkflowController.h/cpp` — Rescan lifecycle (displays added/removed/updated diff)
- `UIRefreshController.h` — Timer polling

### Python Backend (Sprint 14: manifest-first catalog)
- `config.py` — API keys, schemas, paths, MANIFEST_VERSION, metadata provider registry
- `app.py` — **Thin HTTP transport** (~200 lines)
- `chat_pipeline.py` — Chat/Build orchestration (early fail-closed gate)
- `inventory_manifest.py` — Manifest authority (fail-closed + FX eligibility + capability tiers)
- `plugin_catalog.py` — Manifest-first catalog, optional master DB enrichment, metadata envelopes
- `unified_agent.py` — Stage 1 LLM (routing + HOST ONLY awareness)
- `chain_diff.py` — Diff engine (tri-state bypass actions), build_merged_preset
- `stage2_engineer.py` — Stage 2 LLM coordinator (capability-aware, parallel batching)
- `stage2_context.py` — Stage 2 context assembly (prompts, dictionaries, RAG)
- `iron_dome.py` — Safety: value translation, anti-bloat, param validation
- `studio_memory.py` — **Thread-safe** SQLite gateway (Lock-serialized)
- `gemini_client.py` — Gemini API wrapper with fallback
- `db.py` — Backward-compat shim → inventory_manifest + plugin_catalog
- `studio_db.py` — Backward-compat shim → studio_memory

### Tests
- `server/tests/test_chat_pipeline.py` — 3 tests: early fail-closed gate (zero LLM tokens)
- `server/tests/test_chain_diff.py` — 20 tests: identity, add/remove, reorder, duplicates, bypass, merge
- `server/tests/test_inventory_manifest.py` — 6 tests: fail-closed gate, UID validation, FX eligibility, capability tiers
- `server/tests/test_iron_dome.py` — 26 tests: value translation, safety filter, anti-bloat
