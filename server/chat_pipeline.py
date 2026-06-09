"""
ChainPrompt AI — Chat Pipeline: orchestration logic for /api/chat.

Extracted from app.py (Sprint 13: The Great Decoupling III).
Flask app.py becomes a thin transport layer; all orchestration lives here.
"""

import base64
import json
import logging

from google.genai import types as genai_types

from .plugin_catalog import (uid_to_name, uids_to_names,
                              enforce_plugin_format_list, resolve_name_to_uid)
from .inventory_manifest import validate_uids, is_inventory_available
from .audio_context import build_audio_context
from .unified_agent import unified_agent
from .chain_diff import compute_chain_diff, build_merged_preset
from .stage2_engineer import stage2_engineer
from .iron_dome import anti_bloat_uids
from .studio_memory import (upsert_track, log_chat, get_chat_history,
                             clear_chat_history,
                             upsert_mix_state, find_track_by_name,
                             get_all_tracks,
                             extract_and_store_preferences,
                             get_user_preferences,
                             normalize_chain, chain_uids,
                             CROSS_TRACK_TTL_MINUTES)

logger = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# Cross-Track Context Builder
# ---------------------------------------------------------------------------
_CROSS_TRACK_KEYWORDS = [
    "room for", "make room", "space for", "clash with", "compete with",
    "separate from", "sit with", "complement", "звільни місце", "місце для",
    "конфлікт з", "відокрем від",
]


def _build_cross_track_context(prompt: str, current_instance_id: str,
                                project_scope: str | None = None) -> str:
    """If the user references another track, inject that track's metrics/chain."""
    prompt_lower = prompt.lower()

    has_keyword = any(kw in prompt_lower for kw in _CROSS_TRACK_KEYWORDS)
    if not has_keyword:
        return ""

    all_tracks = get_all_tracks(project_scope=project_scope,
                                max_age_minutes=CROSS_TRACK_TTL_MINUTES)
    if not all_tracks:
        return ""

    for track in all_tracks:
        if track["instance_id"] == current_instance_id:
            continue
        track_name = track.get("track_name", "")
        if not track_name:
            continue
        if track_name.lower() in prompt_lower:
            ctx = _format_track_context(track)
            if ctx:
                return ctx

    for keyword in ["bass", "vocal", "drum", "kick", "guitar", "piano", "synth",
                     "бас", "вокал", "барабан", "кік", "гітар", "піано", "синт"]:
        if keyword in prompt_lower:
            match = find_track_by_name(keyword, exclude_instance=current_instance_id,
                                        project_scope=project_scope,
                                        max_age_minutes=CROSS_TRACK_TTL_MINUTES)
            if match:
                ctx = _format_track_context(match)
                if ctx:
                    return ctx

    return ""


def _format_track_context(track: dict) -> str:
    """Format a track's state as context for the LLM."""
    name = track.get("track_name", "Unknown")
    metrics = track.get("metrics")
    chain_entries = track.get("current_chain_entries", [])
    chain_uids_list = track.get("current_chain", [])

    parts = [f"[CROSS-TRACK REFERENCE: '{name}']"]

    if chain_entries:
        slot_strs = []
        for entry in chain_entries:
            uid = entry.get("uid", 0)
            sid = entry.get("slot_id", 0)
            pname = uid_to_name(uid) or f"UID {uid}"
            flags = []
            if entry.get("bypassed"):
                flags.append("BYPASSED")
            if entry.get("user_owned"):
                flags.append("user-added")
            if entry.get("user_touched"):
                flags.append("user-tuned")
            id_prefix = f"S{sid}:" if sid else ""
            suffix = f" [{', '.join(flags)}]" if flags else ""
            slot_strs.append(id_prefix + pname + suffix)
        if slot_strs:
            parts.append(f"  Chain: {' → '.join(slot_strs)}")
    elif chain_uids_list:
        chain_names = uids_to_names(chain_uids_list)
        if chain_names:
            parts.append(f"  Chain: {' → '.join(chain_names)}")

    if metrics:
        m = metrics
        spectrum_summary = ""
        if m.get("spectrum31"):
            spec = m["spectrum31"]
            if len(spec) >= 31:
                freqs = ["20", "25", "31", "40", "50", "63", "80", "100", "125", "160",
                         "200", "250", "315", "400", "500", "630", "800", "1k", "1.25k",
                         "1.6k", "2k", "2.5k", "3.15k", "4k", "5k", "6.3k", "8k",
                         "10k", "12.5k", "16k", "20k"]
                peak_idx = max(range(31), key=lambda i: spec[i])
                spectrum_summary = f", dominant={freqs[peak_idx]}Hz"

        parts.append(
            f"  Metrics: RMS={m.get('rmsDB', -90):.1f}dB, "
            f"Peak={m.get('peakDB', -90):.1f}dB, "
            f"Crest={m.get('crestFactor', 0):.1f}dB"
            f"{spectrum_summary}"
        )
        parts.append(
            f"  Spectrum: sub={m.get('subPct', 0):.0f}%, "
            f"low={m.get('lowPct', 0):.0f}%, "
            f"lowMid={m.get('lowMidPct', 0):.0f}%, "
            f"hiMid={m.get('highMidPct', 0):.0f}%, "
            f"air={m.get('airPct', 0):.0f}%"
        )

    parts.append("  Use this data to avoid frequency masking between tracks.")
    return "\n".join(parts)


# ---------------------------------------------------------------------------
# Main pipeline: processes a single chat/build request end-to-end
# ---------------------------------------------------------------------------
def run_chat_pipeline(data: dict) -> dict:
    """Execute the full chat pipeline. Returns {"reply": str, "preset"?: dict}.

    Args:
        data: Parsed JSON from the C++ client.

    Returns:
        Response dict to be jsonified by Flask.
    """
    prompt = data.get("prompt", "")
    metrics = data.get("metrics")
    audio_b64 = data.get("audioBase64")
    audio_path = data.get("audioPath")
    audio_mime = data.get("audioMimeType", "audio/flac")
    current_chain_raw = data.get("current_chain", [])
    instance_id = data.get("instance_id", "unknown")
    track_name = data.get("track_name", "Unknown Track")
    project_scope = data.get("project_scope", "")
    logger.info(f"Track: '{track_name}' (ID: {instance_id}, scope: {project_scope[:20]})")

    # Normalize current_chain
    current_chain: list[int] = []
    current_chain_entries: list[dict] = []
    for item in current_chain_raw:
        if isinstance(item, dict):
            current_chain.append(item["uid"])
            current_chain_entries.append({
                "uid": item["uid"],
                "slot_id": item.get("slot_id", 0),
                "user_touched": bool(item.get("user_touched", False)),
                "bypassed": bool(item.get("bypassed", False)),
                "user_owned": bool(item.get("user_owned", False)),
            })
        else:
            uid = int(item)
            current_chain.append(uid)
            current_chain_entries.append({"uid": uid, "slot_id": 0,
                                          "user_touched": False,
                                          "bypassed": False,
                                          "user_owned": False})
    logger.debug(f"current_chain: {current_chain} ({len(current_chain)} plugins)")

    # Log intent flags
    touched_count = sum(1 for e in current_chain_entries if e["user_touched"])
    bypassed_count = sum(1 for e in current_chain_entries if e["bypassed"])
    owned_count = sum(1 for e in current_chain_entries if e["user_owned"])
    if touched_count or bypassed_count or owned_count:
        logger.info(f"Slot intent: touched={touched_count}, bypassed={bypassed_count}, "
                    f"owned={owned_count} / {len(current_chain_entries)} total")

    # New Chat
    is_new_chat = data.get("is_new_chat", False)
    if is_new_chat:
        clear_chat_history(instance_id)
        logger.info(f"New Chat: cleared history for instance {instance_id}")

    # History reconciliation
    cpp_history = data.get("history", [])
    if cpp_history and not is_new_chat:
        db_history = get_chat_history(instance_id, limit=1)
        if not db_history:
            logger.info(f"History reconciliation: seeding DB with {len(cpp_history)} entries from C++")
            for entry in cpp_history:
                role = entry.get("role", "user")
                text_val = entry.get("text", "")
                if role in ("model", "assistant"):
                    role = "ai"
                if text_val:
                    log_chat(instance_id, role, text_val)

    # Studio Memory: persist track state
    upsert_track(instance_id, track_name, current_chain_entries)
    upsert_mix_state(instance_id, track_name, metrics,
                     current_chain_entries, project_scope or None)
    log_chat(instance_id, "user", prompt)
    extract_and_store_preferences(prompt)

    # Build audio context
    if metrics:
        logger.debug(f"Keys received: {list(metrics.keys())}")
        audio_ctx = build_audio_context(metrics)
    else:
        audio_ctx = ""

    # Build multimodal audio parts
    audio_parts: list | None = None
    if audio_path:
        try:
            with open(audio_path, "rb") as f:
                audio_bytes = f.read()
            audio_parts = [
                genai_types.Part.from_bytes(data=audio_bytes, mime_type=audio_mime)
            ]
            logger.info(f"Read {len(audio_bytes)} bytes from {audio_path}")
        except Exception as e:
            logger.error(f"Failed to read audio file: {e}")
    elif audio_b64:
        try:
            audio_bytes = base64.b64decode(audio_b64)
            audio_parts = [
                genai_types.Part.from_bytes(data=audio_bytes, mime_type=audio_mime)
            ]
            logger.info(f"Decoded {len(audio_bytes)} bytes of {audio_mime} (Base64 fallback)")
        except Exception as e:
            logger.error(f"Failed to decode audio: {e}")

    # Dual Memory
    chat_history_long = get_chat_history(instance_id, limit=30)
    chat_history_short = chat_history_long[-12:] if len(chat_history_long) > 12 else list(chat_history_long)

    # Resolve format
    is_stereo = metrics.get("isStereo", True) if metrics else True
    prompt_lower = prompt.lower()
    if "stereo" in prompt_lower or "стерео" in prompt_lower:
        is_stereo = True

    current_chain_names = uids_to_names(current_chain) if current_chain else None

    # Cross-Track Context
    cross_track_ctx = _build_cross_track_context(prompt, instance_id,
                                                  project_scope=project_scope or None)
    if cross_track_ctx:
        audio_ctx = (audio_ctx + "\n\n" + cross_track_ctx) if audio_ctx else cross_track_ctx

    # User Preferences
    prefs = get_user_preferences(limit=10)
    if prefs:
        pref_block = "\n".join(f"- {p}" for p in prefs)
        pref_ctx = f"\n\n[USER PREFERENCES (respect these across all sessions)]:\n{pref_block}"
        audio_ctx = (audio_ctx + pref_ctx) if audio_ctx else pref_ctx

    # ─── EARLY FAIL-CLOSED GATE ───
    # Check inventory BEFORE spending LLM tokens on unified_agent.
    if not is_inventory_available():
        fail_msg = ("Plugin inventory is not available. "
                   "Please scan your plugins first (click the Rescan button in the plugin sidebar). "
                   "The AI needs to know which plugins are installed before it can help.")
        logger.warning("Pipeline blocked: inventory unavailable (no manifest) — "
                      "zero LLM tokens spent")
        log_chat(instance_id, "system", fail_msg)
        return {"reply": f"⚠️ {fail_msg}"}

    # ─── UNIFIED AGENT ───
    logger.info(f"{'='*60}")
    logger.info(f"Unified Agent prompt: {prompt[:80]}... "
          f"(metrics={'yes' if metrics else 'no'}, audio={'yes' if audio_parts else 'no'}, "
          f"format={'STEREO' if is_stereo else 'MONO'})")

    agent_result = unified_agent(
        prompt, audio_ctx, chat_history_short,
        is_stereo=is_stereo, audio_parts=audio_parts,
        metrics=metrics,
        current_chain=current_chain if current_chain else None,
        current_chain_names=current_chain_names,
        current_chain_entries=current_chain_entries if current_chain_entries else None,
    )

    route = agent_result["route"]
    reply = agent_result["message"]
    routing = agent_result["routing"]
    configure_uids = agent_result["configure_uids"]

    desired_uids = routing["main_uids"]
    desired_names = routing.get("main_names", [])
    bypassed_uids_from_llm = agent_result.get("bypassed_uids", [])

    logger.info(f"Unified Agent route={route}, "
                f"main={len(desired_uids)} plugins, "
                f"bypassed={len(bypassed_uids_from_llm)} plugins, "
                f"configure={len(configure_uids)} plugins")

    # Hallucination Guard
    if route == "build" and len(desired_uids) < len(desired_names):
        failed_names = [n for n in desired_names
                        if resolve_name_to_uid(n, is_stereo) is None]
        if failed_names:
            fail_msg = f"[Plugin not found: {', '.join(failed_names)}. Use EXACT names from catalog.]"
            log_chat(instance_id, "system", fail_msg)
            logger.warning(f"Hallucination Guard: {failed_names}")

    # ─── CHAT ROUTE ───
    if route == "chat":
        log_chat(instance_id, "ai", reply)
        return {"reply": reply}

    # ─── BUILD ROUTE ───
    # (Inventory already validated at top of pipeline — no double-check needed)

    # Fail-Fast: unavailable UIDs
    unavailable = validate_uids(desired_uids)
    if unavailable:
        unavail_names = [uid_to_name(u) or f"UID {u}" for u in unavailable]
        fail_msg = (f"Cannot build chain — plugins not installed on this machine: "
                    f"{', '.join(unavail_names)}. "
                    f"Please rescan plugins or choose alternatives.")
        logger.warning(f"Fail-Fast: unavailable UIDs {unavailable} -> {unavail_names}")
        log_chat(instance_id, "system", fail_msg)
        log_chat(instance_id, "ai", reply)
        return {"reply": reply + f"\n\n⚠️ {fail_msg}"}

    # Format Enforcer
    desired_uids = enforce_plugin_format_list(desired_uids, is_stereo)
    configure_uids = enforce_plugin_format_list(configure_uids, is_stereo)

    # Anti-Bloat
    pre_bloat = list(desired_uids)
    desired_uids = anti_bloat_uids(desired_uids)
    if len(desired_uids) < len(pre_bloat):
        kept_counts: dict[int, int] = {}
        for u in desired_uids:
            kept_counts[u] = kept_counts.get(u, 0) + 1
        dropped_names = []
        used_counts: dict[int, int] = {}
        for u in pre_bloat:
            used_counts[u] = used_counts.get(u, 0) + 1
            if used_counts[u] > kept_counts.get(u, 0):
                dropped_names.append(uid_to_name(u) or f"UID {u}")
        logger.info(f"Anti-Bloat: trimmed {len(pre_bloat)} → {len(desired_uids)}")
        trim_msg = (f"[Safety limit: chain trimmed from {len(pre_bloat)} to "
                    f"{len(desired_uids)} plugins. Dropped: {', '.join(dropped_names)}.]")
        log_chat(instance_id, "system", trim_msg)
        desired_set = set(desired_uids)
        configure_uids = [u for u in configure_uids if u in desired_set]

    # Bypass positions
    desired_bypassed_positions: set[int] | None = None
    if bypassed_uids_from_llm:
        bypassed_uids_enforced = enforce_plugin_format_list(bypassed_uids_from_llm, is_stereo)
        remaining_bypass = list(bypassed_uids_enforced)
        desired_bypassed_positions = set()
        for i, uid in enumerate(desired_uids):
            if uid in remaining_bypass:
                desired_bypassed_positions.add(i)
                remaining_bypass.remove(uid)
        logger.info(f"Bypass positions: {sorted(desired_bypassed_positions)}")
    elif agent_result.get("_bypass_explicit", False):
        desired_bypassed_positions = set()
        logger.info("Bypass: LLM explicitly cleared all bypass (enable all)")
    else:
        logger.debug("Bypass: not mentioned by LLM — preserving current state")

    # Flat Diff Engine
    diff = compute_chain_diff(current_chain_entries or [], desired_uids,
                              desired_bypassed_positions=desired_bypassed_positions)

    logger.info(f"Diff: identity={diff['is_identity']}, "
                f"reorder={diff['is_reorder']}, "
                f"bypass_changes={diff.get('has_bypass_changes', False)}, "
                f"new={len(diff['new_uids'])}, "
                f"retained={len(diff['retained_uids'])}, "
                f"removed={len(diff['removed_uids'])}")

    # Empty result
    if not diff["final_uids"]:
        logger.info("Diff: chain empty")
        log_chat(instance_id, "ai", reply)
        return {"reply": reply, "preset": {"plugins": []}}

    # Stage 2 targeting
    new_positions: set[int] = set()
    for i, entry in enumerate(diff["final_entries"]):
        if entry["slot_id"] == 0:
            new_positions.add(i)

    configure_positions: set[int] = set()
    configure_remaining = list(configure_uids)
    for i, uid in enumerate(desired_uids):
        if uid in configure_remaining:
            configure_positions.add(i)
            configure_remaining.remove(uid)

    user_touched_positions: set[int] = set()
    bypassed_positions: set[int] = set()
    for i, entry in enumerate(diff["final_entries"]):
        if entry.get("user_touched", False):
            user_touched_positions.add(i)
        if entry.get("bypassed", False):
            bypassed_positions.add(i)

    if user_touched_positions:
        protected_retained = user_touched_positions - new_positions
        if protected_retained:
            logger.info(f"User-intent: protecting {len(protected_retained)} user-touched positions "
                        f"from Stage 2 re-tuning: {sorted(protected_retained)}")
            configure_positions -= protected_retained

    if bypassed_positions:
        bypassed_retained = bypassed_positions - new_positions
        if bypassed_retained:
            logger.info(f"Bypass-intent: skipping {len(bypassed_retained)} bypassed positions "
                        f"from Stage 2: {sorted(bypassed_retained)}")
            configure_positions -= bypassed_retained

    s2_indices: list[int] = sorted(new_positions | configure_positions)
    s2_uids = [diff["final_entries"][i]["uid"] for i in s2_indices]

    # Stage 2
    if s2_uids:
        s2_names = [uid_to_name(u) or f"UID {u}" for u in s2_uids]
        logger.info(f"Stage 2: Configuring {len(s2_uids)} plugins: {s2_names}")
        s2 = stage2_engineer(prompt, audio_ctx, chat_history_short, s2_uids,
                             audio_parts=audio_parts,
                             stage1_strategy=reply)

        s2_params_by_pos: dict[int, list] = {}
        if s2.get("preset") and s2["preset"].get("plugins"):
            for j, p in enumerate(s2["preset"]["plugins"]):
                if j < len(s2_indices):
                    s2_params_by_pos[s2_indices[j]] = p.get("params", [])

        logger.info(f"Stage 2: Done. Got params for {len(s2_params_by_pos)} positions")
    else:
        s2_params_by_pos = {}
        if diff["is_identity"]:
            logger.info("Stage 2: SKIPPED — identity (no changes)")
        elif diff["is_reorder"]:
            logger.info("Stage 2: SKIPPED — reorder only (params preserved)")
        else:
            logger.info("Stage 2: SKIPPED — no plugins need param generation")

    # Build merged preset
    s2_pos_set = set(s2_indices)
    preset = build_merged_preset(diff["final_entries"], s2_pos_set, s2_params_by_pos)

    logger.info(f"Merge Final: {len(preset['plugins'])} plugins")

    # User-intent feedback
    preserved_names = []
    for pos in sorted(user_touched_positions | bypassed_positions):
        if pos < len(diff["final_entries"]):
            uid = diff["final_entries"][pos]["uid"]
            name = uid_to_name(uid) or f"UID {uid}"
            if pos < len(preset["plugins"]):
                flags = []
                if pos in user_touched_positions:
                    flags.append("manual settings preserved")
                if pos in bypassed_positions:
                    flags.append("bypassed")
                preserved_names.append(f"{name} ({', '.join(flags)})")
    if preserved_names:
        preservation_note = "\n🔒 " + ", ".join(preserved_names)
        reply += preservation_note

    # Persist post-diff chain
    post_diff_entries = [
        {k: v for k, v in e.items() if k != "action"}
        for e in diff["final_entries"]
    ]
    upsert_track(instance_id, track_name, post_diff_entries)
    upsert_mix_state(instance_id, track_name, metrics,
                     post_diff_entries, project_scope or None)

    log_chat(instance_id, "ai", reply)
    return {"reply": reply, "preset": preset}
