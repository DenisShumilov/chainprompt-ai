"""
ChainPrompt AI — Iron Dome safety filter + physical-to-VST value translation
+ Anti-Bloat chain validation.
"""

import logging

from .plugin_catalog import highres_by_uid, highres_params, plugins_by_uid

logger = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# Translate physical target value -> normalized VST float (0.0-1.0)
# ---------------------------------------------------------------------------
def translate_to_vst_float(uid: int, index: int,
                           target_value, high_res_db=None) -> float | None:
    """Convert a physical target value to a normalized VST float.

    ALGORITHM V4: O(N) linear scan + linear interpolation on original ordered map.
    """
    if high_res_db is None:
        high_res_db = highres_params

    logger.debug(f"uid={uid} idx={index} target={target_value}")
    param = high_res_db.get((uid, index))
    if param is None:
        return None

    ptype = param.get("type", "")

    # --- Discrete: target_value is already a normalized float ---
    if ptype == "discrete":
        if isinstance(target_value, str):
            options = param.get("options", {})
            target_lower = target_value.lower().strip()
            for label, norm in options.items():
                if label.lower().strip() == target_lower:
                    return float(norm)
            for label, norm in options.items():
                if target_lower in label.lower():
                    return float(norm)
            return None
        return max(0.0, min(1.0, float(target_value)))

    # --- Continuous: O(N) linear scan + linear interpolation ---
    if ptype != "continuous":
        return None

    val_array = param.get("map", [])
    n = len(val_array)
    if n < 2:
        return None

    try:
        target = float(target_value)
    except (ValueError, TypeError):
        return None

    step = 1.0 / (n - 1)

    min_val = min(val_array)
    max_val = max(val_array)

    if target <= min_val:
        for i, v in enumerate(val_array):
            if v == min_val:
                return round(i * step, 5)
        return 0.0
    if target >= max_val:
        for i in range(n - 1, -1, -1):
            if val_array[i] == max_val:
                return round(i * step, 5)
        return 1.0

    best_idx = -1
    best_gap = float("inf")

    for i in range(n - 1):
        v1 = val_array[i]
        v2 = val_array[i + 1]
        lo_v = min(v1, v2)
        hi_v = max(v1, v2)

        if lo_v <= target <= hi_v:
            gap = hi_v - lo_v
            if gap < best_gap:
                best_gap = gap
                best_idx = i

    if best_idx < 0:
        return None

    v1 = val_array[best_idx]
    v2 = val_array[best_idx + 1]
    n1 = best_idx * step
    n2 = (best_idx + 1) * step

    phys_range = v2 - v1
    if abs(phys_range) < 1e-12:
        return round(n1, 5)

    ratio = (target - v1) / phys_range
    exact_float = n1 + ratio * (n2 - n1)

    logger.debug(f"uid={uid} idx={index}: "
          f"target={target} seg[{best_idx}]: {v1}@{n1:.4f}->{v2}@{n2:.4f} "
          f"ratio={ratio:.6f} => norm={exact_float:.6f}")

    return round(max(0.0, min(1.0, exact_float)), 5)


# ---------------------------------------------------------------------------
# Iron Dome Filter
# ---------------------------------------------------------------------------
_PRESET_SELECTOR_NAMES = {"bank", "preset", "load", "preset select",
                          "preset selector", "program", "program select"}


def _get_param_name(uid: int, idx: int) -> str:
    """Look up a parameter name from HighRes or Master DB (lowercase)."""
    hr = highres_by_uid.get(uid)
    if hr:
        for param in hr.get("parameters", []):
            if param.get("index") == idx:
                return param.get("name", "").lower()
    master = plugins_by_uid.get(uid)
    if master:
        for param in master.get("parameters", []):
            if param.get("index") == idx:
                return param.get("name", "").lower()
    return ""


def iron_dome_filter(preset: dict) -> dict:
    """IRON DOME: remove index==0, force preset selectors to 0.0,
    translate physical values -> normalized VST floats,
    clamp values, and validate bypass logic."""
    if not preset or "plugins" not in preset:
        return preset

    for plugin in preset["plugins"]:
        uid = plugin.get("uid", 0)
        if "params" not in plugin:
            continue
        clean = []
        for p in plugin["params"]:
            idx = p.get("index", -1)
            if idx == 0:
                logger.debug(f"Blocked master bypass on uid {uid}")
                continue
            if idx < 0:
                logger.warning(f"Blocked negative index {idx} on uid {uid}")
                continue

            param_name = _get_param_name(uid, idx)
            if param_name in _PRESET_SELECTOR_NAMES or (idx == 1 and param_name in ("", "bank", "preset")):
                logger.debug(f"Forced preset selector to 0.0: uid={uid} idx={idx} name='{param_name}'")
                p["value"] = 0.0
                clean.append(p)
                continue

            raw_val = p.get("value", 0.5)

            translated = translate_to_vst_float(uid, idx, raw_val)
            if translated is not None:
                logger.debug(f"uid={uid} idx={idx}: "
                      f"physical={raw_val} -> norm={translated:.5f}")
                p["value"] = translated
            else:
                try:
                    val = float(raw_val)
                except (ValueError, TypeError):
                    val = 0.5
                p["value"] = max(0.0, min(1.0, val))

            clean.append(p)
        plugin["params"] = clean
    return preset


# ---------------------------------------------------------------------------
# Anti-Bloat: safety-net limit (matches C++ kMaxPluginsInChain)
# Taste enforcement is now in the LLM prompt (soft recommendation),
# Python only enforces the hard C++ limit as a safety net.
# ---------------------------------------------------------------------------
MAX_PLUGINS_IN_CHAIN = 16


def anti_bloat_uids(uids: list[int]) -> list[int]:
    """Enforce max chain size and remove consecutive duplicate UIDs.

    No category limits — serial compression, dual reverbs, etc. are valid
    professional techniques. Only enforces:
      1. Remove consecutive identical UIDs (obvious stutter dupes).
      2. Max total chain size (MAX_PLUGINS_IN_CHAIN).
    """
    if not uids:
        return uids

    # Pass 1: remove consecutive identical UIDs
    deduped: list[int] = []
    for uid in uids:
        if deduped and deduped[-1] == uid:
            name = plugins_by_uid.get(uid, {}).get("name", f"UID {uid}")
            logger.debug(f"Removed consecutive duplicate: {name} (uid={uid})")
            continue
        deduped.append(uid)

    # Pass 2: enforce total limit
    dropped = 0
    if len(deduped) > MAX_PLUGINS_IN_CHAIN:
        overflow = deduped[MAX_PLUGINS_IN_CHAIN:]
        deduped = deduped[:MAX_PLUGINS_IN_CHAIN]
        for uid in overflow:
            name = plugins_by_uid.get(uid, {}).get("name", f"UID {uid}")
            logger.warning(f"Dropped {name} (uid={uid}): "
                  f"exceeds max chain size {MAX_PLUGINS_IN_CHAIN}")
            dropped += 1

    total_removed = len(uids) - len(deduped)
    if total_removed:
        logger.info(f"Removed {total_removed} plugin(s): "
              f"{len(uids)} -> {len(deduped)}")

    return deduped


def anti_bloat_filter(preset: dict) -> dict:
    """Enforce max chain size on a full preset dict.

    No category limits — only enforces max total chain size and removes
    consecutive duplicate UIDs. Returns the filtered preset dict (mutated in place).
    """
    if not preset or "plugins" not in preset:
        return preset

    plugins = preset["plugins"]
    if not plugins:
        return preset

    # Pass 1: remove consecutive identical UIDs
    kept: list[dict] = []
    for plugin in plugins:
        uid = plugin.get("uid", 0)
        if kept and kept[-1].get("uid", -1) == uid:
            name = plugins_by_uid.get(uid, {}).get("name", f"UID {uid}")
            logger.debug(f"Removed consecutive duplicate: {name} (uid={uid})")
            continue
        kept.append(plugin)

    # Pass 2: enforce total plugin count (trim from the end)
    if len(kept) > MAX_PLUGINS_IN_CHAIN:
        overflow = kept[MAX_PLUGINS_IN_CHAIN:]
        kept = kept[:MAX_PLUGINS_IN_CHAIN]
        for plugin in overflow:
            uid = plugin.get("uid", 0)
            name = plugins_by_uid.get(uid, {}).get("name", f"UID {uid}")
            logger.warning(f"Dropped {name} (uid={uid}): "
                  f"exceeds max chain size {MAX_PLUGINS_IN_CHAIN}")

    total_removed = len(plugins) - len(kept)
    if total_removed:
        logger.info(f"Removed {total_removed} plugin(s): "
              f"{len(plugins)} -> {len(kept)}")

    preset["plugins"] = kept
    return preset
