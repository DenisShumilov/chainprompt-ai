"""
ChainPrompt AI — Declarative Chain Diff Engine (flat, main-bus only).

Compares current vs desired plugin UIDs on the main serial chain.
Determines which plugins are new (need Stage 2), retained, or removed.
Computes per-slot ACTIONS (keep/insert/bypass/enable/reconfigure) for
the authoritative slot-state contract.
Supports duplicate UIDs via per-instance slot_id identity.
Propagates full slot intent: user_touched, bypassed, user_owned.
Sends are DISABLED (amputated at LLM level).
"""

import logging

from .plugin_catalog import uid_to_name

logger = logging.getLogger(__name__)


def _make_entry(uid: int, slot_id: int = 0, user_touched: bool = False,
                bypassed: bool = False, user_owned: bool = False,
                action: str = "keep") -> dict:
    """Create a normalized slot entry dict."""
    return {
        "uid": uid,
        "slot_id": slot_id,
        "user_touched": user_touched,
        "bypassed": bypassed,
        "user_owned": user_owned,
        "action": action,
    }


# ---------------------------------------------------------------------------
# Diff: current entries vs desired UIDs
# ---------------------------------------------------------------------------
def compute_chain_diff(current_entries: list[dict],
                       desired_uids: list[int],
                       desired_bypassed_positions: set[int] | None = None) -> dict:
    """Compute diff between current and desired main chain.

    Args:
        current_entries: [{"uid": int, "slot_id": int, "user_touched"?: bool,
                           "bypassed"?: bool, "user_owned"?: bool}, ...]
        desired_uids: [int, ...] (from LLM — no slot_ids)
        desired_bypassed_positions: set of indices into desired_uids that should
                                     be bypassed. None = no bypass changes.

    Returns:
        {
            "final_uids":      [int, ...],
            "final_entries":   [{"uid": ..., "slot_id": ..., "action": str, ...}, ...],
            "new_uids":        [int, ...],
            "retained_uids":   [int, ...],
            "retained_entries":[{"uid": ..., ...}, ...],
            "removed_uids":    [int, ...],
            "removed_entries": [{"uid": ..., ...}, ...],
            "is_reorder":      bool,
            "is_identity":     bool,
            "has_bypass_changes": bool,
        }
    """
    # None = no bypass info (preserve current state)
    # set() = explicitly nothing bypassed (enable all currently bypassed)
    # {1,2} = positions 1,2 should be bypassed, others enabled
    has_bypass_info = desired_bypassed_positions is not None
    bypassed_pos = desired_bypassed_positions if has_bypass_info else set()

    # Normalize entries to ensure all fields exist
    current_entries = [
        _make_entry(
            uid=e.get("uid", 0),
            slot_id=e.get("slot_id", 0),
            user_touched=e.get("user_touched", False),
            bypassed=e.get("bypassed", False),
            user_owned=e.get("user_owned", False),
        )
        for e in current_entries
    ]

    current_uids = [e["uid"] for e in current_entries]

    # --- Identity case: same UIDs in same order ---
    if current_uids == desired_uids:
        # Still check for bypass changes
        final_entries = []
        has_bypass_changes = False
        for i, entry in enumerate(current_entries):
            e = dict(entry)
            was_bypassed = e.get("bypassed", False)
            should_bypass = i in bypassed_pos

            if should_bypass and not was_bypassed:
                e["action"] = "bypass"
                e["bypassed"] = True
                has_bypass_changes = True
            elif not should_bypass and was_bypassed and has_bypass_info:
                # Only enable if the server explicitly provided bypass info
                # (None = "no change" — preserve current state)
                e["action"] = "enable"
                e["bypassed"] = False
                has_bypass_changes = True
            else:
                e["action"] = "keep"
            final_entries.append(e)

        if has_bypass_changes:
            logger.info(f"Diff: identity UIDs but bypass changes detected")

        return {
            "final_uids": list(desired_uids),
            "final_entries": final_entries,
            "new_uids": [],
            "retained_uids": list(current_uids),
            "retained_entries": final_entries,
            "removed_uids": [],
            "removed_entries": [],
            "is_reorder": False,
            "is_identity": not has_bypass_changes,
            "has_bypass_changes": has_bypass_changes,
        }

    # --- Non-identity: greedy matching ---
    available = list(current_entries)
    final_entries = []
    retained_entries = []
    new_uids = []

    for pos, uid in enumerate(desired_uids):
        matched = None
        for i, entry in enumerate(available):
            if entry["uid"] == uid:
                matched = available.pop(i)
                break

        if matched:
            e = dict(matched)
            was_bypassed = e.get("bypassed", False)
            should_bypass = pos in bypassed_pos

            # Compute bypass/enable action
            # Use has_bypass_info (not truthiness of bypassed_pos set) to distinguish
            # "LLM didn't mention bypass" (None → preserve) from "LLM said nothing
            # should be bypassed" (empty set → enable all).
            if should_bypass and not was_bypassed:
                e["action"] = "bypass"
                e["bypassed"] = True
            elif not should_bypass and was_bypassed and has_bypass_info:
                e["action"] = "enable"
                e["bypassed"] = False
            else:
                e["action"] = "keep"

            final_entries.append(e)
            retained_entries.append(e)
        else:
            # New plugin — slot_id=0 means C++ will assign a real one
            should_bypass = pos in bypassed_pos
            final_entries.append(_make_entry(uid=uid, action="insert",
                                             bypassed=should_bypass))
            new_uids.append(uid)

    removed_entries = available
    removed_uids = [e["uid"] for e in removed_entries]
    retained_uids = [e["uid"] for e in retained_entries]
    final_uids = [e["uid"] for e in final_entries]

    is_reorder = (not new_uids and not removed_entries
                  and current_uids != desired_uids)

    has_bypass_changes = any(
        e["action"] in ("bypass", "enable") for e in final_entries
    )

    # Log summary
    if new_uids:
        names = [uid_to_name(u) or f"UID {u}" for u in new_uids]
        logger.info(f"Diff: NEW plugins needing Stage 2: {names}")
    if removed_uids:
        names = [uid_to_name(u) or f"UID {u}" for u in removed_uids]
        logger.info(f"Diff: REMOVED: {names}")
    if retained_entries:
        touched = sum(1 for e in retained_entries if e["user_touched"])
        bypassed = sum(1 for e in retained_entries if e["bypassed"])
        owned = sum(1 for e in retained_entries if e["user_owned"])
        detail = f"({touched} touched, {bypassed} bypassed, {owned} owned)" if (touched or bypassed or owned) else ""
        logger.debug(f"Diff: RETAINED: {len(retained_entries)} plugins {detail}")
    if is_reorder:
        logger.info("Diff: REORDER detected")

    # Log actions
    action_counts: dict[str, int] = {}
    for e in final_entries:
        act = e.get("action", "keep")
        action_counts[act] = action_counts.get(act, 0) + 1
    if any(v for k, v in action_counts.items() if k != "keep"):
        logger.info(f"Diff actions: {action_counts}")

    return {
        "final_uids": final_uids,
        "final_entries": final_entries,
        "new_uids": new_uids,
        "retained_uids": retained_uids,
        "retained_entries": retained_entries,
        "removed_uids": removed_uids,
        "removed_entries": removed_entries,
        "is_reorder": is_reorder,
        "is_identity": False,
        "has_bypass_changes": has_bypass_changes,
    }


# ---------------------------------------------------------------------------
# Merge: combine retained plugins (keep params) + Stage 2 results + actions
# ---------------------------------------------------------------------------
def build_merged_preset(final_entries: list[dict],
                        s2_positions: set[int],
                        s2_params_by_position: dict[int, list]) -> dict:
    """Build flat preset by merging retained + Stage 2 params + slot actions.

    Uses positional matching (duplicate-safe — no UID-keyed dicts).
    Retained plugins get empty params (C++ keeps existing state).
    New/configured plugins get Stage 2 params.
    Each plugin gets an 'action' field for the C++ apply layer.

    Args:
        final_entries: [{"uid": int, "slot_id": int, "action": str, ...}, ...]
        s2_positions: set of indices into final_entries that have Stage 2 params
        s2_params_by_position: {position_index: [param_list]}

    Returns:
        {"plugins": [{"uid": int, "slot_id": int, "params": [...],
                       "action": str}, ...]}
    """
    plugins = []
    for i, entry in enumerate(final_entries):
        base_action = entry.get("action", "keep")

        if i in s2_positions:
            params = s2_params_by_position.get(i, [])
            # Retained plugin getting new params = reconfigure (not just keep)
            action = "reconfigure" if base_action == "keep" else base_action
        else:
            params = []
            action = base_action

        plugins.append({
            "uid": entry["uid"],
            "slot_id": entry["slot_id"],
            "params": params,
            "action": action,
        })
    return {"plugins": plugins}
