# ChainPrompt AI — Architecture Design & Memory

Цей файл описує "як і що працює під капотом", щоб розробник швидко зрозумів загальну структуру і не дублював уже наявні рішення.

## 1. 5-Layer Pattern (Architectural Boundaries)
Проєкт розділено на жорсткі шари (заборони на прямі виклики):
1. **Presentation Layer (UI):** `PluginEditor`, `PromptView`, Macro Knobs.
2. **Application Layer (Core):** `ChainPromptAudioProcessor` (APVTS, Save/Load).
3. **IPC / Async Layer:** `LLMCoordinator`, HTTP REST Client, Thread Workers.
4. **State Management:** `AbstractFifo`, Lock-Free Queues для передачі JSON/String.
5. **DSP Core:** `AudioProcessorGraph`, Plugin Instances (vendor-neutral VST3), Smoothers.

## 2. Signal Flow
`Audio In` → `Pre-Trim` → `Plugin Nodes (AudioProcessorGraph)` → `Post-Trim` → `Meters` → `Audio Out`

## 3. Communication Rules (Golden Rules)
- **UI → DSP:** Only via `APVTS` parameters (Sliders → Attachments).
- **DSP → UI:** Only via `APVTS` (Listener) or lock-free polling in `timerCallback` if transferring Strings.
- **LLM Thread → DSP:** Worker Thread makes HTTP API call → gets formatted JSON → pushes struct into `AbstractFifo` → `processBlock` pops data and updates graph/parameters → returns processing.

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

### 4.4 Capability Tiers (db.py)
- **configurable** — installed + metadata ready/stale → full LLM parameter tuning
- **hostable** — installed + needs_extract/failed → instantiate with factory defaults
- **unavailable** — not in manifest → rejected

### 4.5 Pipeline Integration
1. `db.py:_load_metadata_envelopes()` loads ALL provider files at startup
2. `db.py:_recompute_metadata_status()` classifies every installed UID
3. `db.py:get_metadata_backlog()` returns actionable state for diagnostics
4. `stage2_engineer.py` uses `get_uid_metadata(uid)` accessor — provider-agnostic
5. Catalog and prompts rank plugins by capability, not by vendor
6. `/api/rescan` reloads manifest + recomputes backlog + rebuilds caches

### 4.6 Adding a New Metadata Provider
1. Add entry to `config.py:KNOWN_PROVIDERS` and `METADATA_DB_PATHS`
2. Write extractor that outputs envelope contract JSON
3. `db.py:_load_metadata_envelopes()` automatically picks it up
4. No changes needed in Stage 2 or Unified Agent — they're provider-agnostic

## 5. Inventory Gate (FAIL-CLOSED)
- No manifest → no plugins available (BUILD route blocked)
- Manifest is written by C++ `GlobalPluginCache.h` after vendor-neutral VST3 scan
- `db.py` refuses to allow-all when manifest is missing — this is by design
- Never re-add allow-all fallback

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
