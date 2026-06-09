"""
ChainPrompt AI — Inventory Authority: installed plugin manifest lifecycle.

SINGLE SOURCE OF TRUTH for plugin availability on this machine.
FAIL-CLOSED: if manifest is missing, NO plugins are available.

Extracted from db.py (Sprint 13: The Great Decoupling III).
"""

import json
import logging
import os
import threading
from datetime import datetime, timezone

from .config import (INSTALLED_MANIFEST_PATH, MANIFEST_VERSION,
                     MetadataStatus, METADATA_STALE_HOURS, METADATA_DB_PATHS)

logger = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# Helper: strip Mono/Stereo/Live suffixes to get base plugin name
# ---------------------------------------------------------------------------
def get_base_name(full_name: str) -> str:
    base = full_name
    if base.endswith(" Mono/Stereo"):
        base = base[:-12]
    elif base.endswith(" Stereo"):
        base = base[:-7]
    elif base.endswith(" Mono"):
        base = base[:-5]
    if base.endswith("/Stereo"):
        last_space = base.rfind(" ")
        if last_space >= 0:
            base = base[:last_space]
    if base.endswith(" Quad"):
        base = base[:-5]
    if base.endswith(" Live"):
        base = base[:-5]
    return base.strip()


# ---------------------------------------------------------------------------
# Installed Plugin Manifest state
# ---------------------------------------------------------------------------
_installed_uids: set[int] = set()
_installed_base_names: set[str] = set()
_installed_plugin_info: dict[int, dict] = {}
_installed_instruments: set[str] = set()   # base names flagged isInstrument
_installed_surround: set[str] = set()      # base names with numOutputChannels > 2
_manifest_lock = threading.Lock()
_manifest_loaded = False

_manifest_version: int = 0
_manifest_scan_mode: str = ""
_manifest_failed_files: list[str] = []
_last_rescan_summary: dict | None = None

# ---------------------------------------------------------------------------
# Metadata Control Plane — per-UID status, provider, and timestamps
# ---------------------------------------------------------------------------
_metadata_status: dict[int, str] = {}
_metadata_provider: dict[int, str] = {}
_metadata_timestamps: dict[str, str] = {}

# These will be set by plugin_catalog after loading metadata envelopes
_highres_by_uid_ref: dict[int, dict] = {}


def set_highres_reference(highres: dict[int, dict]) -> None:
    """Called by plugin_catalog after loading metadata envelopes.

    Provides a read reference so capability checks can test for metadata.
    """
    global _highres_by_uid_ref
    _highres_by_uid_ref = highres


# ---------------------------------------------------------------------------
# Manifest loading
# ---------------------------------------------------------------------------
def _load_installed_manifest() -> None:
    """Load installed_plugins.json and populate availability sets.

    FAIL-CLOSED: if the manifest doesn't exist, NO plugins are available.
    """
    global _installed_uids, _installed_base_names, _manifest_loaded
    global _manifest_version, _manifest_scan_mode, _manifest_failed_files
    global _installed_plugin_info, _installed_instruments, _installed_surround

    if not os.path.isfile(INSTALLED_MANIFEST_PATH):
        logger.warning(f"installed_plugins.json not found at {INSTALLED_MANIFEST_PATH} "
                       f"— inventory UNAVAILABLE (fail-closed)")
        _manifest_loaded = False
        return

    try:
        with open(INSTALLED_MANIFEST_PATH, "r", encoding="utf-8") as f:
            manifest = json.load(f)

        _manifest_version = manifest.get("version", 1)
        _manifest_scan_mode = manifest.get("scanMode", "unknown")
        _manifest_failed_files = manifest.get("failedFiles", [])
        plugins = manifest.get("plugins", [])

        new_uids: set[int] = set()
        new_base_names: set[str] = set()
        new_info: dict[int, dict] = {}
        new_instruments: set[str] = set()
        new_surround: set[str] = set()

        for p in plugins:
            uid = p.get("uid")
            name = p.get("name", "")
            if uid is not None:
                base = get_base_name(name)
                new_uids.add(uid)
                new_base_names.add(base)
                new_info[uid] = {
                    "name": name,
                    "category": p.get("category", ""),
                    "manufacturer": p.get("manufacturer", ""),
                    "format": p.get("format", "VST3"),
                    "pluginFile": p.get("pluginFile", ""),
                    "isInstrument": p.get("isInstrument", False),
                    "numInputChannels": p.get("numInputChannels", 0),
                    "numOutputChannels": p.get("numOutputChannels", 0),
                    "version": p.get("version", ""),
                }
                if p.get("isInstrument", False):
                    new_instruments.add(base)
                if p.get("numOutputChannels", 0) > 2:
                    new_surround.add(base)

        with _manifest_lock:
            _installed_uids = new_uids
            _installed_base_names = new_base_names
            _installed_plugin_info = new_info
            _installed_instruments = new_instruments
            _installed_surround = new_surround
            _manifest_loaded = True

        logger.info(f"Installed manifest v{_manifest_version} ({_manifest_scan_mode}): "
                    f"{len(new_uids)} UIDs, {len(new_base_names)} base names, "
                    f"{len(_manifest_failed_files)} failed scans")
    except Exception as e:
        logger.error(f"Failed to load installed manifest: {e}")
        _manifest_loaded = False


def reload_installed_manifest(rebuild_catalogs_fn=None) -> dict:
    """Hot-reload the manifest after a rescan. Returns detailed summary dict.

    Args:
        rebuild_catalogs_fn: Optional callback to rebuild cached catalogs
            after manifest reload (injected by plugin_catalog).
    """
    global _last_rescan_summary

    old_uids = set(_installed_uids)
    _load_installed_manifest()
    new_uids = set(_installed_uids)

    added = new_uids - old_uids
    removed = old_uids - new_uids

    recompute_metadata_status()

    if rebuild_catalogs_fn:
        rebuild_catalogs_fn()

    backlog = get_metadata_backlog()

    summary = {
        "total": len(new_uids),
        "added": len(added),
        "removed": len(removed),
        "configurable": backlog["ready"],
        "hostable_only": backlog["needs_extract"],
        "stale": backlog["stale"],
        "failed_scan_files": len(_manifest_failed_files),
        "manifest_version": _manifest_version,
        "scan_mode": _manifest_scan_mode,
        "extraction_backlog": backlog["needs_extract"],
        "metadata_backlog": backlog,
    }
    _last_rescan_summary = summary
    logger.info(f"Manifest reloaded: {summary}")
    return summary


# ---------------------------------------------------------------------------
# Public accessors
# ---------------------------------------------------------------------------
def is_uid_installed(uid: int) -> bool:
    """FAIL-CLOSED: returns False when manifest hasn't been loaded."""
    if not _manifest_loaded:
        return False
    with _manifest_lock:
        return uid in _installed_uids


def is_base_name_installed(base_name: str) -> bool:
    """FAIL-CLOSED: returns False when manifest hasn't been loaded."""
    if not _manifest_loaded:
        return False
    with _manifest_lock:
        return base_name in _installed_base_names


def validate_uids(uids: list[int]) -> list[int]:
    """Return list of UIDs that are NOT installed. Empty = all OK."""
    if not _manifest_loaded:
        return list(uids)
    with _manifest_lock:
        return [uid for uid in uids if uid not in _installed_uids]


def is_inventory_available() -> bool:
    """True if the installed plugin manifest has been loaded successfully."""
    return _manifest_loaded


def get_inventory_status() -> dict:
    """Return detailed inventory status for diagnostics and health endpoint."""
    return {
        "available": _manifest_loaded,
        "installed_count": len(_installed_uids),
        "manifest_version": _manifest_version,
        "scan_mode": _manifest_scan_mode,
        "failed_scan_files": len(_manifest_failed_files),
        "last_rescan": _last_rescan_summary,
        "metadata_backlog": get_metadata_backlog() if _manifest_loaded else None,
    }


def get_installed_uids() -> set[int]:
    """Read-only snapshot of installed UIDs."""
    with _manifest_lock:
        return set(_installed_uids)


def get_all_plugin_info() -> dict[int, dict]:
    """Read-only snapshot of all installed plugin info (uid → metadata dict)."""
    with _manifest_lock:
        return dict(_installed_plugin_info)


def is_fx_eligible(base_name: str) -> bool:
    """True if plugin is an FX effect (not instrument, not surround).

    Vendor-neutral: uses isInstrument and numOutputChannels from manifest.
    FAIL-CLOSED: returns False when manifest hasn't been loaded.
    """
    if not _manifest_loaded:
        return False
    with _manifest_lock:
        if base_name in _installed_instruments:
            return False
        if base_name in _installed_surround:
            return False
    return True


def get_manifest_scan_mode() -> str:
    """Return the scan mode from the last loaded manifest."""
    return _manifest_scan_mode


# ---------------------------------------------------------------------------
# Capability tiers
# ---------------------------------------------------------------------------
def get_plugin_capability(uid: int) -> str:
    """'configurable' | 'hostable' | 'unavailable'."""
    if not is_uid_installed(uid):
        return "unavailable"
    if uid in _highres_by_uid_ref:
        return "configurable"
    return "hostable"


def get_base_name_capability(base_name: str, uid_format_map: dict) -> str:
    """Return best capability tier across all format variants of a plugin."""
    if not is_base_name_installed(base_name):
        return "unavailable"
    for uid, info in uid_format_map.items():
        if info["base"] == base_name and uid in _highres_by_uid_ref:
            return "configurable"
    return "hostable"


# ---------------------------------------------------------------------------
# Metadata Control Plane
# ---------------------------------------------------------------------------
def get_metadata_status(uid: int) -> str:
    if not is_uid_installed(uid):
        return "unavailable"
    return _metadata_status.get(uid, MetadataStatus.NEEDS_EXTRACT)


def get_metadata_provider(uid: int) -> str | None:
    return _metadata_provider.get(uid)


def set_metadata_maps(provider: dict[int, str], timestamps: dict[str, str]) -> None:
    """Called by plugin_catalog after loading metadata envelopes."""
    global _metadata_provider, _metadata_timestamps
    _metadata_provider = provider
    _metadata_timestamps = timestamps


def recompute_metadata_status() -> None:
    """Recompute per-UID metadata status based on installed UIDs and loaded metadata."""
    global _metadata_status
    new_status: dict[int, str] = {}
    for uid in _installed_uids:
        if uid in _highres_by_uid_ref:
            provider_id = _metadata_provider.get(uid, "unknown")
            timestamp = _metadata_timestamps.get(provider_id, "")
            if timestamp and _is_stale(timestamp):
                new_status[uid] = MetadataStatus.STALE
            else:
                new_status[uid] = MetadataStatus.READY
        else:
            new_status[uid] = MetadataStatus.NEEDS_EXTRACT
    _metadata_status = new_status


def get_metadata_backlog() -> dict:
    """Return categorized metadata status for all installed UIDs."""
    counts = {
        MetadataStatus.READY: 0,
        MetadataStatus.NEEDS_EXTRACT: 0,
        MetadataStatus.STALE: 0,
        MetadataStatus.FAILED: 0,
    }
    for uid in _installed_uids:
        status = _metadata_status.get(uid, MetadataStatus.NEEDS_EXTRACT)
        counts[status] = counts.get(status, 0) + 1

    by_provider: dict[str, int] = {}
    for uid, prov in _metadata_provider.items():
        if uid in _installed_uids:
            by_provider[prov] = by_provider.get(prov, 0) + 1

    return {
        "total_installed": len(_installed_uids),
        "ready": counts[MetadataStatus.READY],
        "needs_extract": counts[MetadataStatus.NEEDS_EXTRACT],
        "stale": counts[MetadataStatus.STALE],
        "failed": counts[MetadataStatus.FAILED],
        "by_provider": by_provider,
        "provider_timestamps": dict(_metadata_timestamps),
    }


def _is_stale(timestamp_str: str) -> bool:
    if not timestamp_str:
        return False
    try:
        dt = datetime.fromisoformat(timestamp_str.replace("Z", "+00:00"))
        age_hours = (datetime.now(timezone.utc) - dt).total_seconds() / 3600
        return age_hours > METADATA_STALE_HOURS
    except (ValueError, TypeError):
        return False


# ---------------------------------------------------------------------------
# Initialization: load manifest at import time (fail-closed: empty = nothing)
# ---------------------------------------------------------------------------
_load_installed_manifest()
