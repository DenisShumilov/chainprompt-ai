"""
ChainPrompt AI — Plugin Catalog: metadata, catalogs, name/UID resolution.

Owns:
  - Master DB loading (Waves_Master_DB.json)
  - Metadata envelope loading (provider-neutral HighRes)
  - RAG PresetRetriever initialization
  - Catalog building (filtered by inventory_manifest)
  - Name ↔ UID resolution (format-aware)
  - Format enforcement (Mono/Stereo swap)

Extracted from db.py (Sprint 13: The Great Decoupling III).
"""

import json
import logging
import os
import sys
from datetime import datetime, timezone

from .config import (DB_PATH, META_GT_PATH, METADATA_DB_PATHS,
                     METADATA_STALE_HOURS)
from .inventory_manifest import (
    get_base_name, is_uid_installed, is_base_name_installed,
    get_plugin_capability, get_base_name_capability,
    get_installed_uids, get_all_plugin_info, is_fx_eligible,
    set_highres_reference, set_metadata_maps,
    recompute_metadata_status,
)

logger = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# Load Meta GroundTruth (semantic catalog: name, category, tags, description)
# ---------------------------------------------------------------------------
logger.info(f"Loading Meta GroundTruth from: {META_GT_PATH}")
META_GROUNDTRUTH: list[dict] = []
PLUGIN_DESCRIPTIONS: dict[str, str] = {}
META_BY_NAME: dict[str, dict] = {}

if os.path.isfile(META_GT_PATH):
    try:
        with open(META_GT_PATH, "r", encoding="utf-8") as f:
            META_GROUNDTRUTH = json.load(f)
        for entry in META_GROUNDTRUTH:
            name = entry.get("name", "")
            if name:
                META_BY_NAME[name] = entry
                PLUGIN_DESCRIPTIONS[name] = entry.get("description", "")
        logger.info(f"Loaded {len(META_GROUNDTRUTH)} Meta GroundTruth entries")
    except (json.JSONDecodeError, OSError) as e:
        # A git-lfs pointer file or unreadable file lands here; metadata is
        # optional enrichment, so continue without it.
        META_GROUNDTRUTH = []
        logger.warning(f"Meta_GroundTruth unreadable ({e}) — continuing without it. "
                       "If you cloned without git-lfs, run: git lfs install && git lfs pull")
else:
    logger.warning(f"Meta_GroundTruth not found at {META_GT_PATH}!")

# ---------------------------------------------------------------------------
# Load Master Database (optional enrichment — not the catalog authority)
# ---------------------------------------------------------------------------
plugins_list: list[dict] = []
plugins_by_uid: dict[int, dict] = {}
uid_to_base_name: dict[int, str] = {}

if os.path.isfile(DB_PATH):
    try:
        logger.info(f"Loading master DB from: {DB_PATH}")
        with open(DB_PATH, "r", encoding="utf-8") as f:
            master_db = json.load(f)
        plugins_list = master_db["plugins"]
        plugins_by_uid = {p["uid"]: p for p in plugins_list}
        for p in plugins_list:
            uid_to_base_name[p["uid"]] = get_base_name(p["name"])
        logger.info(f"Master DB: {len(plugins_list)} plugins loaded (enrichment source)")
    except (json.JSONDecodeError, KeyError, OSError) as e:
        # Master DB is optional enrichment; a git-lfs pointer file or corrupt
        # file must not block startup — fall back to manifest-only catalog.
        plugins_list = []
        plugins_by_uid = {}
        uid_to_base_name = {}
        logger.warning(f"Master DB unreadable ({e}) — catalog runs from manifest only. "
                       "If you cloned without git-lfs, run: git lfs install && git lfs pull")
else:
    logger.warning(f"Master DB not found at {DB_PATH} — catalog runs from manifest only")

# ---------------------------------------------------------------------------
# Load metadata from provider-neutral envelopes
# ---------------------------------------------------------------------------
highres_by_uid: dict[int, dict] = {}
highres_params: dict[tuple[int, int], dict] = {}


def reload_metadata_envelopes() -> None:
    """Load/reload all provider metadata envelope files into shared caches.

    Called at startup and after /api/rescan for hot-reload."""
    global highres_by_uid, highres_params

    new_highres: dict[int, dict] = {}
    new_params: dict[tuple[int, int], dict] = {}
    new_provider: dict[int, str] = {}
    new_timestamps: dict[str, str] = {}

    for provider_id, db_path in METADATA_DB_PATHS.items():
        if not os.path.isfile(db_path):
            logger.warning(f"Metadata DB not found for provider '{provider_id}': {db_path}")
            continue

        try:
            logger.info(f"Loading metadata from provider '{provider_id}': {db_path}")
            with open(db_path, "r", encoding="utf-8") as f:
                envelope = json.load(f)

            file_provider = envelope.get("provider", "legacy")
            file_version = envelope.get("provider_version", 0)
            file_timestamp = envelope.get("scan_timestamp", "")

            if file_provider not in ("legacy", provider_id):
                logger.warning(f"Provider mismatch in {db_path}: "
                               f"file says '{file_provider}', config says '{provider_id}'")

            new_timestamps[provider_id] = file_timestamp

            plugins = envelope.get("plugins", [])
            loaded_count = 0
            param_count = 0
            for p in plugins:
                uid = p.get("uid")
                if uid is None:
                    continue
                new_highres[uid] = p
                new_provider[uid] = provider_id
                for param in p.get("parameters", []):
                    new_params[(uid, param["index"])] = param
                    param_count += 1
                loaded_count += 1

            logger.info(f"Provider '{provider_id}' v{file_version}: "
                        f"{loaded_count} plugins, {param_count} params"
                        + (f", timestamp={file_timestamp}" if file_timestamp else " (legacy, no timestamp)"))

        except Exception as e:
            logger.error(f"Failed to load metadata from provider '{provider_id}': {e}")

    highres_by_uid = new_highres
    highres_params = new_params

    # Wire metadata into inventory authority
    set_highres_reference(highres_by_uid)
    set_metadata_maps(new_provider, new_timestamps)
    recompute_metadata_status()

    if not new_highres:
        logger.warning("No metadata loaded from any provider — "
                       "Stage 2 will use legacy 5-point curves from master DB")


reload_metadata_envelopes()


def get_uid_metadata(uid: int) -> dict | None:
    """Provider-neutral accessor: return parameter metadata for a UID."""
    hr = highres_by_uid.get(uid)
    if hr:
        return hr
    return plugins_by_uid.get(uid)


# ---------------------------------------------------------------------------
# Initialize RAG Preset Retriever (SQLite-backed, optional local tool).
# The retriever lives outside this repository; degrade to None when absent so
# the backend boots without it (stage2_context already handles None).
# ---------------------------------------------------------------------------
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'tools'))
preset_retriever = None
try:
    from rag_retriever import PresetRetriever
    preset_retriever = PresetRetriever()
    logger.info("PresetRetriever ready")
except ImportError:
    logger.warning("rag_retriever not present — preset RAG disabled")
except Exception as e:
    logger.warning(f"PresetRetriever init failed: {e} — preset RAG disabled")

# ---------------------------------------------------------------------------
# Build Preset-Richness Index
# ---------------------------------------------------------------------------
_preset_count_by_base: dict[str, int] = {}
if preset_retriever is not None:
    _preset_count_by_base = dict(preset_retriever._preset_counts)
    logger.info(f"Preset richness index: {len(_preset_count_by_base)} plugins mapped")


def _get_rag_tier(base_name: str) -> str:
    preset_count = _preset_count_by_base.get(base_name, 0)
    if preset_count >= 10:
        return "★★★"
    if preset_count >= 1:
        return "★★"
    for uid, p in highres_by_uid.items():
        if get_base_name(p["name"]) == base_name:
            real_params = sum(1 for pr in p.get("parameters", []) if pr["index"] != 0)
            return "★" if real_params <= 15 else ""
    return ""


# ---------------------------------------------------------------------------
# Build FULL catalog (using Meta GroundTruth)
# ---------------------------------------------------------------------------
_SKIP_CATEGORIES = {"Fx|Surround", "Instrument|Waves", "Instrument|Synth"}
_SKIP_WORDS = {"5.0", "5.1", "7.0", "7.1", "9.0", "9.1",
               "LV1", "SoundGrid", "DiGiGrid", "Quad", "AmbiX", "FuMa"}
_VARIANT_PRIORITY = {"Stereo": 0, "Mono/Stereo": 1, "Mono": 2}

_all_variants: dict[str, dict] = {}
for p in plugins_list:
    name = p["name"]
    raw_cat = p.get("category", "")
    uid = p["uid"]

    if raw_cat in _SKIP_CATEGORIES:
        continue
    if any(w in name for w in _SKIP_WORDS):
        continue

    base_name = get_base_name(name)
    variant_type = None
    for suffix in _VARIANT_PRIORITY:
        if name.endswith(f" {suffix}"):
            variant_type = suffix
            break

    entry = _all_variants.get(base_name)
    if entry is None:
        entry = {"best_priority": 99, "best_uid": uid, "best_name": name,
                 "raw_cat": raw_cat,
                 "stereo_uid": None, "mono_stereo_uid": None, "mono_uid": None}
        _all_variants[base_name] = entry

    if variant_type == "Stereo":
        entry["stereo_uid"] = uid
    elif variant_type == "Mono/Stereo":
        entry["mono_stereo_uid"] = uid
    elif variant_type == "Mono":
        entry["mono_uid"] = uid
    else:
        if entry["stereo_uid"] is None:
            entry["stereo_uid"] = uid
        if entry["mono_uid"] is None:
            entry["mono_uid"] = uid

    priority = _VARIANT_PRIORITY.get(variant_type, 5)
    if priority < entry["best_priority"]:
        entry["best_priority"] = priority
        entry["best_uid"] = uid
        entry["best_name"] = name
        entry["raw_cat"] = raw_cat

# ---------------------------------------------------------------------------
# Inject manifest-only plugins (vendor-neutral: any installed VST3 not in master DB)
# ---------------------------------------------------------------------------
def _inject_manifest_plugins() -> int:
    """Add manifest-only plugins into catalog structures. Returns count injected."""
    manifest_info = get_all_plugin_info()
    injected = 0

    for uid, info in manifest_info.items():
        if uid in plugins_by_uid:
            continue  # Already covered by master DB enrichment

        name = info["name"]
        base_name = get_base_name(name)

        # Skip instruments and surround (universal eligibility)
        if not is_fx_eligible(base_name):
            continue

        # Add basic entry to plugins_by_uid (no parameter data — host-only)
        plugins_by_uid[uid] = {
            "uid": uid,
            "name": name,
            "category": info.get("category", ""),
            "manufacturer": info.get("manufacturer", ""),
            "parameters": [],
        }
        plugins_list.append(plugins_by_uid[uid])
        uid_to_base_name[uid] = base_name

        # Determine variant type from name suffix
        variant_type = None
        for suffix in _VARIANT_PRIORITY:
            if name.endswith(f" {suffix}"):
                variant_type = suffix
                break

        entry = _all_variants.get(base_name)
        if entry is None:
            entry = {"best_priority": 99, "best_uid": uid, "best_name": name,
                     "raw_cat": info.get("category", ""),
                     "stereo_uid": None, "mono_stereo_uid": None, "mono_uid": None}
            _all_variants[base_name] = entry

        if variant_type == "Stereo":
            entry["stereo_uid"] = uid
        elif variant_type == "Mono/Stereo":
            entry["mono_stereo_uid"] = uid
        elif variant_type == "Mono":
            entry["mono_uid"] = uid
        else:
            # No variant suffix — use channel count from manifest
            num_out = info.get("numOutputChannels", 2)
            if num_out <= 1:
                if entry["mono_uid"] is None:
                    entry["mono_uid"] = uid
            else:
                if entry["stereo_uid"] is None:
                    entry["stereo_uid"] = uid

        priority = _VARIANT_PRIORITY.get(variant_type, 5)
        if priority < entry["best_priority"]:
            entry["best_priority"] = priority
            entry["best_uid"] = uid
            entry["best_name"] = name
            entry["raw_cat"] = info.get("category", "")

        injected += 1

    if injected:
        logger.info(f"Injected {injected} manifest-only plugins into catalog "
                    f"(total base names: {len(_all_variants)})")
    return injected


_manifest_injected = _inject_manifest_plugins()


# Grouped full catalog text
_grouped: dict[str, list[str]] = {}
_total_catalog = 0
_described_count = 0
for base_name in sorted(_all_variants.keys()):
    entry = _all_variants[base_name]
    uid = entry["best_uid"]
    full_name = entry["best_name"]
    raw_cat = entry["raw_cat"]
    stereo_uid = entry["stereo_uid"]
    mono_uid = entry["mono_uid"]

    if stereo_uid and mono_uid and stereo_uid != mono_uid:
        uid_str = f"S:{stereo_uid} M:{mono_uid}"
    else:
        uid_str = str(stereo_uid or mono_uid or uid)

    meta = META_BY_NAME.get(base_name)
    if meta:
        display_cat = meta.get("category", "Other").upper()
        tags = meta.get("tags", [])
        desc = meta.get("description", "")
        tags_str = ", ".join(tags) if tags else ""
        line = f"[{uid_str}]: {base_name}"
        if display_cat:
            line += f" | Category: {meta.get('category', 'Other')}"
        if tags_str:
            line += f" | Tags: {tags_str}"
        if desc:
            first_sentence = desc.split(". ")[0]
            if not first_sentence.endswith("."):
                first_sentence += "."
            if len(first_sentence) > 180:
                first_sentence = first_sentence[:177] + "..."
            line += f" | {first_sentence}"
        _described_count += 1
    else:
        display_cat = raw_cat.split("|")[-1].upper().strip() if "|" in raw_cat else "OTHER"
        line = f"[{uid_str}]: {base_name}"

    if display_cat not in _grouped:
        _grouped[display_cat] = []
    _grouped[display_cat].append(line)
    _total_catalog += 1

_catalog_lines: list[str] = []
for cat_name in sorted(_grouped.keys()):
    _catalog_lines.append(f"\n--- {cat_name} ---")
    for line in _grouped[cat_name]:
        _catalog_lines.append(line)

FULL_CATALOG_TEXT = "\n".join(_catalog_lines)


# ---------------------------------------------------------------------------
# Format-filtered catalog (gated by installed manifest)
# ---------------------------------------------------------------------------
def build_filtered_catalog(is_stereo: bool) -> str:
    """Build a format-aware, installation-aware catalog for Stage 1."""
    lines: list[str] = []
    grouped: dict[str, list[str]] = {}

    for base_name in sorted(_all_variants.keys()):
        if not is_base_name_installed(base_name):
            continue

        # Universal FX eligibility: skip instruments and surround (vendor-neutral)
        if not is_fx_eligible(base_name):
            continue

        entry = _all_variants[base_name]
        stereo_uid = entry["stereo_uid"]
        mono_stereo_uid = entry["mono_stereo_uid"]
        mono_uid = entry["mono_uid"]

        eff_stereo = stereo_uid if is_uid_installed(stereo_uid) else None if stereo_uid else None
        eff_ms = mono_stereo_uid if is_uid_installed(mono_stereo_uid) else None if mono_stereo_uid else None
        eff_mono = mono_uid if is_uid_installed(mono_uid) else None if mono_uid else None

        if is_stereo:
            if not (eff_ms or eff_stereo):
                if not eff_mono:
                    continue
        else:
            if not (eff_mono or eff_ms or eff_stereo):
                continue

        rag_tier = _get_rag_tier(base_name)
        tier_badge = f" {rag_tier}" if rag_tier else ""

        capability = get_base_name_capability(base_name, _uid_format_map)
        cap_badge = " [HOST ONLY]" if capability == "hostable" else ""

        meta = META_BY_NAME.get(base_name)
        if meta:
            display_cat = meta.get("category", "Other").upper()
            tags = meta.get("tags", [])
            desc = meta.get("description", "")
            tags_str = ", ".join(tags) if tags else ""
            line = f"- {base_name}{tier_badge}{cap_badge}"
            if display_cat:
                line += f" | Category: {meta.get('category', 'Other')}"
            if tags_str:
                line += f" | Tags: {tags_str}"
            if desc:
                first_sentence = desc.split(". ")[0]
                if not first_sentence.endswith("."):
                    first_sentence += "."
                if len(first_sentence) > 180:
                    first_sentence = first_sentence[:177] + "..."
                line += f" | {first_sentence}"
        else:
            raw_cat = entry["raw_cat"]
            display_cat = raw_cat.split("|")[-1].upper().strip() if "|" in raw_cat else "OTHER"
            line = f"- {base_name}{tier_badge}{cap_badge}"

        if display_cat not in grouped:
            grouped[display_cat] = []
        grouped[display_cat].append(line)

    for cat_name in sorted(grouped.keys()):
        lines.append(f"\n--- {cat_name} ---")
        lines.extend(grouped[cat_name])

    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Auto-Swapper: fix wrong-format UIDs
# ---------------------------------------------------------------------------
_uid_format_map: dict[int, dict] = {}


def _rebuild_uid_format_map() -> None:
    """Rebuild the UID→format variant map from _all_variants."""
    _uid_format_map.clear()
    for _base, _entry in _all_variants.items():
        _s = _entry["stereo_uid"]
        _ms = _entry["mono_stereo_uid"]
        _m = _entry["mono_uid"]
        _map_val = {"stereo": _s, "mono_stereo": _ms, "mono": _m, "base": _base}
        for _uid in (_s, _ms, _m):
            if _uid is not None:
                _uid_format_map[_uid] = _map_val


_rebuild_uid_format_map()


def enforce_plugin_format(uid: int, is_stereo: bool) -> int:
    """Enforce correct Mono/Stereo format for a single UID."""
    info = _uid_format_map.get(uid)
    if info is None:
        return uid

    if is_stereo:
        candidates = [info["stereo"], info["mono_stereo"], info["mono"]]
    else:
        candidates = [info["mono"], info["mono_stereo"], info["stereo"]]

    best = None
    for c in candidates:
        if c is not None and is_uid_installed(c):
            best = c
            break

    if best is None:
        return uid

    if best != uid:
        fmt_src = "Mono" if uid == info["mono"] else ("Mono/Stereo" if uid == info["mono_stereo"] else "Stereo")
        fmt_dst = "Stereo" if best == info["stereo"] else ("Mono" if best == info["mono"] else "Mono/Stereo")
        logger.debug(f"{info['base']}: UID {uid} -> {best} ({fmt_src}->{fmt_dst})")

    return best


def enforce_plugin_format_list(uids: list[int], is_stereo: bool) -> list[int]:
    return [enforce_plugin_format(uid, is_stereo) for uid in uids]


# ---------------------------------------------------------------------------
# Name-to-UID resolution
# ---------------------------------------------------------------------------
def _normalize_name(s: str) -> str:
    return s.lower().replace("-", "").replace(" ", "")


_NORMALIZED_VARIANTS: dict[str, str] = {}


def _rebuild_normalized_variants() -> None:
    """Rebuild normalized name lookup from _all_variants."""
    _NORMALIZED_VARIANTS.clear()
    for base in _all_variants:
        _NORMALIZED_VARIANTS[_normalize_name(base)] = base


_rebuild_normalized_variants()


def _find_variant_entry(name: str) -> dict | None:
    entry = _all_variants.get(name)
    if entry is None:
        for base, e in _all_variants.items():
            if base.lower() == name.lower():
                entry = e
                break
    if entry is None:
        norm = _normalize_name(name)
        real_name = _NORMALIZED_VARIANTS.get(norm)
        if real_name is not None:
            entry = _all_variants[real_name]
            logger.debug(f"Fuzzy match: '{name}' -> '{real_name}'")
    return entry


def resolve_name_to_uid(name: str, is_stereo: bool) -> int | None:
    """Resolve a human-readable plugin name to the correct UID."""
    entry = _find_variant_entry(name)
    if entry is None:
        return None

    if is_stereo:
        candidates = [entry["stereo_uid"], entry["mono_stereo_uid"], entry["mono_uid"]]
    else:
        candidates = [entry["mono_uid"], entry["mono_stereo_uid"], entry["stereo_uid"]]

    for uid in candidates:
        if uid is not None and is_uid_installed(uid):
            return uid
    return None


def uid_to_name(uid: int) -> str | None:
    info = _uid_format_map.get(uid)
    if info:
        return info["base"]
    p = plugins_by_uid.get(uid)
    if p:
        return get_base_name(p["name"])
    return None


def uids_to_names(uids: list[int]) -> list[str]:
    names = []
    for uid in uids:
        name = uid_to_name(uid)
        names.append(name if name else f"Unknown({uid})")
    return names


def names_to_uids(names: list[str], is_stereo: bool) -> list[int]:
    """Convert a list of plugin names to UIDs with chain-aware channel routing."""
    uids = []
    channels = 2 if is_stereo else 1

    for name in names:
        clean = name.strip()
        entry = _find_variant_entry(clean)

        if entry is None:
            logger.warning(f"Cannot resolve '{clean}' — skipping")
            continue

        if channels == 2:
            candidates = [entry["stereo_uid"], entry["mono_stereo_uid"], entry["mono_uid"]]
        else:
            candidates = [entry["mono_uid"], entry["mono_stereo_uid"], entry["stereo_uid"]]

        uid = None
        for c in candidates:
            if c is not None and is_uid_installed(c):
                uid = c
                break

        if uid is None:
            logger.warning(f"Cannot resolve '{clean}' — not installed, skipping")
            continue

        if channels == 2:
            if uid == entry["mono_uid"] and entry["stereo_uid"] is None and entry["mono_stereo_uid"] is None:
                logger.warning(f"'{clean}' has only Mono — stereo image will collapse!")
                channels = 1
        else:
            if uid == entry["mono_stereo_uid"]:
                logger.debug(f"'{clean}' is Mono/Stereo — channels widen to 2")
                channels = 2
            elif uid == entry["stereo_uid"] and entry["mono_uid"] is None and entry["mono_stereo_uid"] is None:
                logger.debug(f"'{clean}' has only Stereo on mono track — forced stereo")
                channels = 2

        uids.append(uid)

    return uids


# ---------------------------------------------------------------------------
# Pre-cached catalogs (built once at startup, rebuilt after manifest reload)
# ---------------------------------------------------------------------------
_CACHED_CATALOG_STEREO: str = build_filtered_catalog(True)
_CACHED_CATALOG_MONO: str = build_filtered_catalog(False)


def _rebuild_cached_catalogs() -> None:
    """Rebuild cached catalog strings after manifest reload.

    Also re-injects manifest-only plugins and rebuilds format/name maps.
    """
    global _CACHED_CATALOG_STEREO, _CACHED_CATALOG_MONO
    _inject_manifest_plugins()
    _rebuild_uid_format_map()
    _rebuild_normalized_variants()
    _CACHED_CATALOG_STEREO = build_filtered_catalog(True)
    _CACHED_CATALOG_MONO = build_filtered_catalog(False)
    logger.info(f"Catalogs rebuilt: stereo={len(_CACHED_CATALOG_STEREO)} chars, "
                f"mono={len(_CACHED_CATALOG_MONO)} chars")


def get_cached_catalog(is_stereo: bool) -> str:
    return _CACHED_CATALOG_STEREO if is_stereo else _CACHED_CATALOG_MONO


# ---------------------------------------------------------------------------
# Startup summary
# ---------------------------------------------------------------------------
logger.info(f"Full catalog: {_total_catalog} plugins ({_described_count} with meta)")
logger.info(f"Format swap map: {len(_uid_format_map)} UIDs indexed")
logger.info(f"Name->UID resolver: {len(_all_variants)} base names indexed "
            f"({_manifest_injected} from manifest)")
logger.info(f"Cached catalogs: stereo={len(_CACHED_CATALOG_STEREO)} chars, mono={len(_CACHED_CATALOG_MONO)} chars")
logger.info(f"Master DB: {len(plugins_list)} plugins loaded")

from .inventory_manifest import _manifest_loaded, _installed_uids as _inv_uids
logger.info(f"Installed filter: {'ACTIVE (' + str(len(_inv_uids)) + ' UIDs)' if _manifest_loaded else 'LOCKED (no manifest — inventory unavailable)'}")

from .inventory_manifest import get_metadata_backlog
_startup_backlog = get_metadata_backlog()
logger.info(f"Metadata control plane: ready={_startup_backlog['ready']}, "
            f"needs_extract={_startup_backlog['needs_extract']}, "
            f"stale={_startup_backlog['stale']}, "
            f"providers={_startup_backlog['by_provider']}")
