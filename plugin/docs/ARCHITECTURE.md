# ChainPrompt AI — Plugin Architecture Notes

This file describes how the plugin side works under the hood, so a developer can quickly understand the structure and avoid duplicating existing decisions.

## 1. 5-Layer Pattern (Architectural Boundaries)
The plugin is split into strict layers (no direct cross-layer calls):
1. **Presentation Layer (GUI):** `ChainPromptAudioProcessorEditor` (composition root), `SidebarPresenter`, `HostedEditorController`, `RescanWorkflowController`, `UIRefreshController`.
2. **Application Layer (Core):** `ChainPromptAudioProcessor` (thin coordinator), `ChainStateSerializer` (DAW save/load), `AIIntentTracker`, `AutomationPoolRouter`, `StudioTelemetryBridge`.
3. **IPC / Async Layer:** `LLMCoordinator` (HTTP client thread), `BackendHealthChecker`, `GlobalPluginCache` scanner thread.
4. **State Handoff:** `AbstractFifo` carrying POD-only `ParamPreset` structs (LLM thread → audio thread); atomics for metrics and flags. No strings or JSON cross the FIFO.
5. **DSP Core:** `AudioProcessorGraph` hosting plugin instances (vendor-neutral VST3), `PerceptualAutoGain`, metric taps, crossfade ramp.

## 2. Signal Flow
`Audio In` → `dry metrics / capture taps` → `Plugin Nodes (AudioProcessorGraph)` → `post metrics / capture taps` → `PerceptualAutoGain` → `crossfade ramp` → `Audio Out`

## 3. Communication Rules (Golden Rules)
- **UI → Processor:** Direct calls on the message thread (topology, bypass, undo); no parameter attachments are used.
- **Processor → UI:** Atomics polled by `UIRefreshController` at 15 Hz (capture state, AI thinking, undo/redo availability); no string transfer on the audio thread.
- **LLM Thread → DSP:** Worker thread makes HTTP API call → parses JSON on the worker thread → pushes a POD `ParamPreset` struct into `AbstractFifo` → `processBlock` pops it and applies parameter values only. All topology changes happen on the message thread, never in `processBlock`.
- **Parameters:** A preallocated pool of 1024 `DynamicSlotParameter` instances registered via `addParameter` (no APVTS); `AutomationPoolRouter` keeps a stable `{slotId, paramIndex}` → pool-slot mapping across topology changes.

## 4. Metadata Control Plane (Sprint 11)

Universal hosting and universal controllability are separate but connected layers:

### 4.1 Two-Truth Architecture
- **Inventory Truth** (C++ → `installed_plugins.json` manifest): what is physically installed and can be instantiated. Vendor-neutral VST3 scan via `GlobalPluginCache.h`.
- **Metadata Truth** (Provider envelopes → `highres_by_uid` cache): what can be AI-configured. Each metadata provider writes a file following the envelope contract.

### 4.2 Provider-Neutral Envelope Contract
Every metadata extractor outputs JSON matching:
```json
{
  "provider": "<provider_id>",
  "provider_version": <int>,
  "scan_timestamp": "<ISO-8601>",
  "plugins": [
    {
      "name": "...",
      "uid": <int>,
      "manufacturer": "...",
      "category": "...",
      "parameters": [
        {
          "index": <int>,
          "name": "...",
          "type": "continuous" | "discrete",
          "unit": "...",
          "default": <float>,
          "map": [1001 values] | {"label": <norm_float>}
        }
      ]
    }
  ]
}
```

Currently registered providers (in `config.py:KNOWN_PROVIDERS`):
- `waves_truescan` — 1001-point VST3 parameter dump for Waves plugins

### 4.3 Metadata Lifecycle States
Each installed UID has a status (in `config.py:MetadataStatus`):
- **ready** — Has parameter maps from a provider (Stage 2 can configure)
- **needs_extract** — Installed but no metadata (host-only, factory defaults)
- **stale** — Has metadata but `scan_timestamp` older than `METADATA_STALE_HOURS`
- **failed** — Extraction was attempted but failed

### 4.4 Capability Tiers (inventory_manifest.py)
- **configurable** — installed + metadata ready/stale → full LLM parameter tuning
- **hostable** — installed + needs_extract/failed → instantiate with factory defaults
- **unavailable** — not in manifest → rejected

### 4.5 Pipeline Integration
1. `plugin_catalog.py:reload_metadata_envelopes()` loads ALL provider files at startup and on `/api/rescan`
2. `inventory_manifest.py:recompute_metadata_status()` classifies every installed UID
3. `inventory_manifest.py:get_metadata_backlog()` returns actionable state for diagnostics
4. `stage2_engineer.py` uses `get_uid_metadata(uid)` accessor — provider-agnostic
5. Catalog and prompts rank plugins by capability, not by vendor
6. `/api/rescan` reloads manifest + recomputes backlog + rebuilds caches

### 4.6 Adding a New Metadata Provider
1. Add entry to `config.py:KNOWN_PROVIDERS` and `METADATA_DB_PATHS`
2. Write extractor that outputs envelope contract JSON
3. `plugin_catalog.py:reload_metadata_envelopes()` automatically picks it up
4. No changes needed in Stage 2 or Unified Agent — they're provider-agnostic

## 5. Inventory Gate (FAIL-CLOSED)
- No manifest → no plugins available (BUILD route blocked)
- Manifest is written by C++ `GlobalPluginCache.h` after vendor-neutral VST3 scan
- `inventory_manifest.py` refuses to allow-all when manifest is missing — this is by design
- There is intentionally no allow-all fallback

## 6. Slot-State Contract
- `PresetData.h:ChainPreset` carries per-slot identity (`slotId`), ownership (`userOwned`), bypass state, and transient actions
- `slotAction[16]`: 0=keep, 1=insert, 2=bypass, 3=enable, 4=reconfigure
- Diff engine (`chain_diff.py`) preserves slot identity across rebuilds
- User-touched slots are protected from Stage 2 re-tuning
- Bypassed slots are kept in chain but skipped by Stage 2

## 7. Key Constants
- `kMaxPluginsInChain = 16`, `kMaxParamsPerPlugin = 128` (PresetData.h)
- `kAutomationPoolSize = 1024`
- `MANIFEST_VERSION = 2` (config.py, must match C++)
- `METADATA_STALE_HOURS = 720` (30 days)
