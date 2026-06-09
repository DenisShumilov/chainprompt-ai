"""
ChainPrompt AI — Configuration: API keys, model, schemas, paths.
"""

import os

from dotenv import load_dotenv

load_dotenv()

# ---------------------------------------------------------------------------
# Gemini API
# ---------------------------------------------------------------------------
GEMINI_API_KEYS = [
    k.strip() for k in os.environ.get("GEMINI_API_KEYS", "").split(",") if k.strip()
]
if not GEMINI_API_KEYS:
    raise RuntimeError("GEMINI_API_KEYS environment variable is not set. "
                       "Set it in .env as comma-separated keys.")

GEMINI_MODEL = "gemini-3.1-flash-lite-preview"
GEMINI_FALLBACK_MODEL = "gemini-2.5-flash"

# ---------------------------------------------------------------------------
# JSON Schemas for structured output (enforced by Gemini response_schema)
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# Unified Agent schema (declarative routing — main serial chain only)
# ---------------------------------------------------------------------------
_ROUTING_SCHEMA = {
    "type": "OBJECT",
    "properties": {
        "main": {"type": "ARRAY", "items": {"type": "STRING"}},
        "bypassed": {"type": "ARRAY", "items": {"type": "STRING"}},
    },
    "required": ["main"],
}

UNIFIED_SCHEMA = {
    "type": "OBJECT",
    "properties": {
        "audio_analysis":       {"type": "STRING"},
        "route":                {"type": "STRING", "enum": ["chat", "build"]},
        "message":              {"type": "STRING"},
        "routing":              _ROUTING_SCHEMA,
        "plugins_to_configure": {"type": "ARRAY", "items": {"type": "STRING"}},
    },
    "required": ["audio_analysis", "route", "message", "routing", "plugins_to_configure"],
}

# ---------------------------------------------------------------------------
# Stage 2 schema
# ---------------------------------------------------------------------------
STAGE2_PARAM_SCHEMA = {
    "type": "OBJECT",
    "properties": {
        "index": {"type": "INTEGER"},
        "value": {"type": "NUMBER"},
    },
    "required": ["index", "value"],
}

STAGE2_PLUGIN_SCHEMA = {
    "type": "OBJECT",
    "properties": {
        "uid":    {"type": "INTEGER"},
        "params": {"type": "ARRAY", "items": STAGE2_PARAM_SCHEMA},
    },
    "required": ["uid", "params"],
}

STAGE2_SCHEMA = {
    "type": "OBJECT",
    "properties": {
        "plugins":     {"type": "ARRAY", "items": STAGE2_PLUGIN_SCHEMA},
    },
    "required": ["plugins"],
}

# ---------------------------------------------------------------------------
# Database file paths (relative to project root)
# ---------------------------------------------------------------------------
_PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

DB_PATH = os.path.join(_PROJECT_ROOT, "data", "catalogs", "Waves_Master_DB.json")
HIGHRES_DB_PATH = os.path.join(_PROJECT_ROOT, "data", "catalogs", "Waves_HighRes_DB.json")
META_GT_PATH = os.path.join(_PROJECT_ROOT, "data", "metadata", "Waves_Meta_GroundTruth.json")

# ---------------------------------------------------------------------------
# Metadata Control Plane: provider-neutral capability & extraction tracking
# ---------------------------------------------------------------------------

class MetadataStatus:
    """Per-UID metadata lifecycle states (not an enum — plain strings for JSON)."""
    READY = "ready"                  # Has parameter maps from a provider (Stage 2 ready)
    NEEDS_EXTRACT = "needs_extract"  # Installed but no metadata from any provider
    STALE = "stale"                  # Has metadata but scan_timestamp too old
    FAILED = "failed"                # Extraction was attempted but failed

# Known metadata providers (extensible registry — add new extractors here)
KNOWN_PROVIDERS: dict[str, dict] = {
    "waves_truescan": {
        "name": "Waves TrueScan",
        "description": "1001-point VST3 parameter dump for Waves plugins",
        "version": 1,
    },
    # Future: "generic_vst3": {...}, "user_manual": {...}
}

# Per-provider metadata file paths (each provider writes its own envelope)
METADATA_DB_PATHS: dict[str, str] = {
    "waves_truescan": os.path.join(_PROJECT_ROOT, "data", "catalogs", "Waves_HighRes_DB.json"),
}

# Metadata older than this is considered stale (hours)
METADATA_STALE_HOURS = 720  # 30 days

# ---------------------------------------------------------------------------
# Installed Plugin Manifest (written by C++ after WaveShell scan)
# This is the SINGLE SOURCE OF TRUTH for what plugins are available.
# ---------------------------------------------------------------------------
_APPDATA = os.environ.get("APPDATA", os.path.expanduser("~"))
INSTALLED_MANIFEST_PATH = os.path.join(_APPDATA, "ChainPrompt", "installed_plugins.json")

# ---------------------------------------------------------------------------
# Stage 2 batching
# ---------------------------------------------------------------------------
STAGE2_CHUNK_SIZE = 3  # max plugins per parallel batch

# ---------------------------------------------------------------------------
# Manifest contract version (must match C++ GlobalPluginCache::exportInstalledManifest)
# ---------------------------------------------------------------------------
MANIFEST_VERSION = 2
