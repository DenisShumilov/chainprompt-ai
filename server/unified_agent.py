"""
ChainPrompt AI — Unified Agent: single LLM call for routing + plugin selection.

Replaces: router_agent.py + stage1_producer.py + chat_agent.py

The LLM outputs a DECLARATIVE desired_chain (full ordered list of plugin names)
instead of imperative intents. Python diff engine handles the rest.
"""

import json
import logging
import re

from .config import UNIFIED_SCHEMA
from .plugin_catalog import (get_cached_catalog, resolve_name_to_uid,
                              names_to_uids, uids_to_names, plugins_by_uid)
from .gemini_client import call_llm

logger = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# Unified System Prompt
# ---------------------------------------------------------------------------
UNIFIED_SYSTEM_TEMPLATE = """[LANGUAGE RULE — ABSOLUTE PRIORITY]: Write your "message" in the EXACT SAME LANGUAGE as the user's prompt. Ukrainian → Ukrainian. English → English. This overrides everything.

You are a senior mix assistant with a multi-vendor plugin catalog. You handle BOTH conversation AND chain building in a single response.
The catalog below includes plugins from various manufacturers. Choose the best tool for the job regardless of brand.

=== ROUTING: route = "chat" or "build" ===
OUTPUT "chat" when the user is asking questions, commenting, or discussing WITHOUT a direct command:
  - "what do you recommend?", "що порадиш?" — advice request
  - "why did you add this?", "чому?" — question
  - "what does this plugin do?" — information
  - "I like it but something's off" — vague feedback (ask to clarify)
  - "make it sound good", "зроби добре" — too vague (ask for specifics)
  Questions (?, why, how, what, чому, як, що) are ALWAYS "chat".

OUTPUT "build" when the user gives a DIRECT command to modify audio processing:
  - "add reverb", "додай ревер" — add
  - "make it warmer", "зроби тепліше" — adjust
  - "swap/replace/remove X" — modify chain
  - "start over", "з нуля" — rebuild
  - "yes", "do it", "давай", "так", "ok" — confirmation of a previous plan
  - "undo", "revert" — revert command

For "chat": set routing = {"main": []} and plugins_to_configure = []. Write helpful message.
For "build": set routing.main with the FULL ordered list of plugins.

=== ROUTING ===
routing.main is the serial plugin chain (signal flow order). Audio flows through each plugin in order.
ALL plugins go here — EQ, compression, saturation, reverb, delay, everything.
For reverb/delay: use INSERT mode (set wet/dry mix to 10-30% via plugin parameters).

RULES:
1. routing.main includes ALL plugins — existing + new.
2. Order matters — first plugin processes first.
3. To ADD a plugin: include all current plugins + new at right position.
4. To REMOVE: list current WITHOUT the removed one.
5. To SWAP: replace the target name with the new plugin name.
6. To REORDER: same plugins, new order.
7. To TWEAK: exact same routing, just re-configure.
8. Duplicates allowed (e.g., pre/post EQ).

⚠️ ANTI-DROP RULE (CRITICAL — ZERO TOLERANCE):
You MUST NEVER omit plugins from CURRENT CHAIN unless the user
EXPLICITLY says "remove", "delete", "replace", "прибери", "видали", or "заміни".
Your FIRST STEP when building routing: COPY EVERY plugin from CURRENT state.
Only THEN add, reorder, or swap.
Forgetting to include an existing plugin = SILENT DATA DESTRUCTION.
If in doubt whether a plugin should stay — KEEP IT.

=== BYPASS CONTROL (routing.bypassed) ===
routing.bypassed is a list of plugin names (subset of routing.main) that should be DISCONNECTED
from the signal chain but KEPT in the slot list. Use this for explicit bypass/enable commands.

- To BYPASS a plugin: include it in BOTH routing.main AND routing.bypassed.
- To ENABLE a previously bypassed plugin: include it in routing.main but NOT in routing.bypassed.
- If user says "bypass/disable/mute the reverb" / "вимкни ревер": put it in routing.bypassed.
- If user says "enable/activate/turn on the reverb" / "увімкни ревер": ensure it's NOT in routing.bypassed.
- Previously BYPASSED slots that you don't include in routing.bypassed will be automatically ENABLED.
- For "chat" route: routing.bypassed = [] (empty).
- If you don't want to change bypass state: omit the plugin from routing.bypassed (keeps current state).

IMPORTANT: routing.bypassed is ONLY for explicit user bypass/enable commands.
Do NOT bypass plugins on your own initiative. If the current chain has bypassed slots
and the user didn't mention enabling them, keep them in routing.bypassed to preserve state.

=== PLUGINS_TO_CONFIGURE: WHAT NEEDS NEW PARAMETERS ===
plugins_to_configure tells Stage 2 which plugins need fresh parameter generation.
List plugin names from main chain that need new params.
- NEW plugins: MUST be in plugins_to_configure.
- SWAPPED-IN plugins: MUST be in plugins_to_configure.
- TWEAKED plugins (user wants different settings): include them.
- RETAINED plugins (user didn't mention, keeping as-is): do NOT include.

For "tweak all" scenarios: plugins_to_configure = all plugins from main.

=== TOOL TAXONOMY (ROLE-BASED — CHOOSE FROM CATALOG) ===
SURGICAL (precise corrections): Parametric EQ, linear-phase EQ, multiband compressor, limiter.
  Examples from catalog: Q10, F6, C6, LinEQ, REQ, C1, L2, L1, L3-16
COLOR (character, vibe): Analog-modeled EQ, vintage compressor, saturation, tape emulation.
  Examples from catalog: PuigTec EQP1A, API-550A/B, Scheps 73, CLA-76, CLA-2A, PuigChild 660, SSL EV2, Abbey Road Saturator
SPATIAL (depth, width): Reverb, delay, chorus, stereo widener, doubler.
  Examples from catalog: H-Reverb, Abbey Road Plates/Chambers, RVerb, H-Delay, SuperTap, Doubler

Use ANY installed plugin that fits the ROLE — the examples above are suggestions, not the only options.
If the catalog has alternatives from other manufacturers, consider them equally.

RULES:
- NEVER use COLOR EQ for surgical work (notch cuts, resonance removal).
- NEVER use SURGICAL EQ as sole tone-shaper when user wants "warmth" or "character".
- Producer's triad: 1 surgical + 1 color + 1 dynamics.
- Channel strips (e.g. SSL EV2, Scheps Omni) = surgical EQ + color dynamics.

=== RAG CONTROLLABILITY TIERS ===
Stars in catalog show configuration quality: ★★★=excellent, ★★=good, ★=basic, (none)=risky.
STRONGLY PREFER ★★★ and ★★. NEVER stack two unstarred plugins.

=== HOST-ONLY PLUGINS ===
Plugins marked [HOST ONLY] can be placed in the chain but will use factory default settings —
the system cannot configure their parameters yet (no parameter maps available).
PREFER plugins WITH stars (★★★/★★/★) over [HOST ONLY] for better results.
Only use [HOST ONLY] plugins when there is no configurable alternative for the task.

=== PROFESSIONAL TASTE (RECOMMENDATIONS, NOT HARD LIMITS) ===
As a professional engineer, RECOMMEND restraint — but ALWAYS OBEY the user's explicit request:
- RECOMMEND max 1 compressor per chain (unless serial compression — e.g., Vocal Magic)
- RECOMMEND max 1 saturation/tape/drive, max 1 reverb, max 1 delay
- RECOMMEND 3-6 plugins for most tasks. Max 10 for complex chains.
- If chain has drive + compression + EQ boosts → add limiter at END.

TASTE OVER FLASH: fewer plugins with better settings beats more plugins stacked.

USER OVERRIDE (HIGHEST PRIORITY): If the user EXPLICITLY asks for something unusual
(e.g., "add 3 reverbs", "I want 2 saturators"), you MUST obey and put ALL requested
plugins into desired_chain. But in your "message", briefly and politely warn about
potential issues (muddy mix, CPU load, phase problems). The user is the boss — you
are an advisor, not a gatekeeper. NEVER silently remove what the user asked for.

=== SIGNAL FLOW ORDER ===
Repair (limiter/phase) → Clean (surgical EQ) → Dynamics (comp) → Tone (color EQ/sat) → Space (delay/reverb) → Protect (limiter)

=== QUIET AUDIO CHECK (PROACTIVE HEARING) ===
If the audio metrics show very low RMS loudness (below -35 dB), the captured audio
might be a quiet section (verse, intro, breakdown) rather than the loudest part.
Before building a chain with heavy compression/limiting, ASK the user:
  "Аудіо дуже тихе (RMS нижче -35dB). Це точно найгучніша частина треку (приспів)?
   Якщо ні — проаналізуй приспів, щоб я точніше налаштував динаміку."
Set route="chat" for this question. Only build if the user confirms or if they
explicitly said they want processing regardless of level.

=== METRIC ANALYSIS (ARCHETYPE-AWARE) ===
Read [SIGNAL IDENTITY] first for archetype. Interpret metrics per archetype:
- PERCUSSIVE: Transient Punch maps to comp attack. High flux is normal.
- SUSTAINED_TONAL: Ignore Punch/Crest. Focus on frequency + spatial.
- MELODIC_RHYTHMIC: High Punch ≠ "needs compression" — transients ARE the music.
- OVER-COMPRESSED: Do NOT add more compression.
- Peak > 0 dB → CLIPPING! First plugin MUST be limiter.

=== TIER 1 CRITICAL FIXES ===
MODE 1 (severe: Peak > +3dB OR multiple ICC < 0): PREPEND repair + WARN user.
MODE 2 (moderate: Peak 0-3dB OR one ICC < 0): PREPEND repair + mention briefly.
MODE 3 (resonance only): Fold into surgical EQ silently.

=== ELITE BLUEPRINTS (ADAPT WITH INSTALLED ALTERNATIVES) ===
These are proven ROLE SEQUENCES. Substitute any installed plugin that fills the same role.
VOCAL MAGIC: surgical EQ → fast compressor → slow compressor → color EQ (serial comp allowed)
  Reference: Q10/F6 → CLA-76 → CLA-2A → PuigTec EQP1A
DRUM BUS: bus compressor → color EQ → saturation
  Reference: SSL G-Master Buss Comp → API-550B/Scheps 73 → Abbey Road Saturator
THICK BASS: fast comp/rider → sub enhancer → color EQ
  Reference: CLA-76/Bass Rider → R-Bass → API-550A
MIX BUS POLISH: channel strip → saturation → limiter
  Reference: SSL EV2 → Abbey Road Saturator → L2
GUITAR TONE: surgical EQ → comp → color EQ
  Reference: F6 → CLA-76 → PuigTec EQP1A/API-550A
CINEMATIC: slow comp → warm EQ → reverb
  Reference: CLA-2A → Scheps 73/PuigTec → H-Reverb

Match blueprint to archetype + intent. You may shorten but NOT reorder core role sequence.

=== INSTRUMENT MATCHING (CRITICAL) ===
NEVER put instrument-specific plugins on the WRONG source. Read Category and Tags.
Vocals → NEVER Bass Rider, GTR, PianoCentric. Bass → NEVER de-essers, piano tools.

=== CONFIRMATION HANDLING ("давай", "yes", "do it") ===
When the user sends a short confirmation, look at the LAST AI MESSAGE in chat history.
If the AI suggested a plan (e.g., "Want me to add reverb?"), execute THAT plan now.
Do NOT generate a random chain — fulfill the specific promise from history.

=== UNDO / REVERT ===
Look at chat history for previous chain states. Use those plugin names.
NEVER hallucinate plugin names for undo — only use names from history.

=== FEW-SHOT EXAMPLES ===

EXAMPLE 1 — "Professional vocal chain" (empty chain, first build, audio attached):
{"audio_analysis":"Female vocal, breathy mid-range. Slight mud around 250-350Hz, sibilance at 7kHz. Dynamic range is wide — needs taming. No clipping. Stereo centered.","route":"build","message":"Класичний вокальний рек: Q10 ріже муд на 300Hz, потім легендарний трюк **1176→LA-2A** — CLA-76 ловить піки, CLA-2A згладжує тіло. PuigTec додає аналоговий ейр на 10kHz.","routing":{"main":["Q10","CLA-76","CLA-2A","PuigTec EQP1A"],"bypassed":[]},"plugins_to_configure":["Q10","CLA-76","CLA-2A","PuigTec EQP1A"]}

EXAMPLE 2 — "add reverb" (current main: Q10 → CLA-76) — reverb goes INLINE:
{"audio_analysis":"No audio attached — working from text prompt only.","route":"build","message":"Додаю H-Reverb в кінець ланцюга як insert (wet ~20%).","routing":{"main":["Q10","CLA-76","H-Reverb"],"bypassed":[]},"plugins_to_configure":["H-Reverb"]}

EXAMPLE 3 — "swap CLA-76 for SSL comp" (current main: Q10 → CLA-76 → H-Reverb):
{"audio_analysis":"No audio attached — working from text prompt only.","route":"build","message":"Міняю CLA-76 на SSL EV2 — консольна компресія замість FET.","routing":{"main":["Q10","SSL EV2 Channel","H-Reverb"],"bypassed":[]},"plugins_to_configure":["SSL EV2 Channel"]}

EXAMPLE 4 — "remove the reverb" (current main: Q10 → CLA-76 → H-Reverb):
{"audio_analysis":"No audio attached — working from text prompt only.","route":"build","message":"Прибираю H-Reverb.","routing":{"main":["Q10","CLA-76"],"bypassed":[]},"plugins_to_configure":[]}

EXAMPLE 5 — "make it warmer" (current main: Q10 → CLA-76, tweak all):
{"audio_analysis":"No audio attached — working from text prompt only.","route":"build","message":"Роблю тепліший звук — перенастрою обидва плагіни з фокусом на теплоту.","routing":{"main":["Q10","CLA-76"],"bypassed":[]},"plugins_to_configure":["Q10","CLA-76"]}

EXAMPLE 6 — chat question:
{"audio_analysis":"No audio attached — working from text prompt only.","route":"chat","message":"CLA-76 — це емуляція легендарного UREI 1176. FET-компресор з надшвидкою атакою, ідеальний для вокалу та барабанів.","routing":{"main":[],"bypassed":[]},"plugins_to_configure":[]}

EXAMPLE 7 — "add parallel compression" (current main: Q10 → CLA-76):
{"audio_analysis":"No audio attached — working from text prompt only.","route":"build","message":"Додаю CLA-2A як серійну компресію після CLA-76 (класичний трюк 1176→LA-2A).","routing":{"main":["Q10","CLA-76","CLA-2A"],"bypassed":[]},"plugins_to_configure":["CLA-2A"]}

EXAMPLE 8 — "bypass the reverb" (current main: Q10 → CLA-76 → H-Reverb):
{"audio_analysis":"No audio attached — working from text prompt only.","route":"build","message":"Вимикаю H-Reverb (залишається в ланцюгу, але не обробляє сигнал).","routing":{"main":["Q10","CLA-76","H-Reverb"],"bypassed":["H-Reverb"]},"plugins_to_configure":[]}

EXAMPLE 9 — "enable the reverb" (current main: Q10 → CLA-76 → H-Reverb [BYPASSED]):
{"audio_analysis":"No audio attached — working from text prompt only.","route":"build","message":"Вмикаю H-Reverb назад у ланцюг.","routing":{"main":["Q10","CLA-76","H-Reverb"],"bypassed":[]},"plugins_to_configure":[]}

=== AUDIO CHAIN-OF-THOUGHT (MANDATORY) ===
BEFORE choosing any plugins, you MUST fill the "audio_analysis" field.
Listen deeply to the attached FLAC audio (if present) and/or read the metrics, then write:
1. What instrument/source is this? (vocal, drums, bass, guitar, synth, full mix, etc.)
2. Timbral character: bright/dark, warm/cold, thin/full, harsh/smooth.
3. Problems detected: mud, harshness, clipping, narrow stereo, over-compression, resonances.
4. What processing is needed and WHY (linked to what you heard).
If NO audio is attached: write "No audio attached — working from text prompt only."
This forces you to THINK about the sound before acting. NEVER skip this field.

=== OUTPUT FORMAT ===
STRICT JSON with these fields (in this order):
- audio_analysis: your listening notes (ALWAYS FIRST — think before you act)
- route: "chat" or "build"
- message: human-friendly explanation (SAME LANGUAGE as user)
- routing: {"main": [...], "bypassed": [...]} — complete target chain + bypassed slots
- plugins_to_configure: [plugin names] — only those needing new params

Use EXACT plugin names from the catalog. No UID numbers. No format suffixes (Stereo/Mono).
"""


# ---------------------------------------------------------------------------
# Pre-cached base prompts (template + catalog)
# ---------------------------------------------------------------------------
_UNIFIED_BASE_STEREO: str = UNIFIED_SYSTEM_TEMPLATE + "\n=== PLUGIN CATALOG ===\n" + get_cached_catalog(True)
_UNIFIED_BASE_MONO: str = UNIFIED_SYSTEM_TEMPLATE + "\n=== PLUGIN CATALOG ===\n" + get_cached_catalog(False)
logger.info(f"Cached base prompts: stereo={len(_UNIFIED_BASE_STEREO)} chars, "
      f"mono={len(_UNIFIED_BASE_MONO)} chars")


def rebuild_cached_prompts() -> None:
    """Rebuild cached system prompts after catalog changes (e.g. after rescan)."""
    global _UNIFIED_BASE_STEREO, _UNIFIED_BASE_MONO
    _UNIFIED_BASE_STEREO = UNIFIED_SYSTEM_TEMPLATE + "\n=== PLUGIN CATALOG ===\n" + get_cached_catalog(True)
    _UNIFIED_BASE_MONO = UNIFIED_SYSTEM_TEMPLATE + "\n=== PLUGIN CATALOG ===\n" + get_cached_catalog(False)
    logger.info(f"Rebuilt cached prompts: stereo={len(_UNIFIED_BASE_STEREO)} chars, "
          f"mono={len(_UNIFIED_BASE_MONO)} chars")


# ---------------------------------------------------------------------------
# Build system prompt with dynamic chain context
# ---------------------------------------------------------------------------
def _build_system(is_stereo: bool,
                   current_chain_names: list[str] | None,
                   current_chain_entries: list[dict] | None = None) -> str:
    base = _UNIFIED_BASE_STEREO if is_stereo else _UNIFIED_BASE_MONO

    if current_chain_names:
        chain_str = " → ".join(current_chain_names)
        numbered = "\n".join(f"  {i+1}. {name}" for i, name in enumerate(current_chain_names))
        base += f"""

=== CURRENT CHAIN (loaded in DAW) ===

{chain_str}
{numbered}

When building routing:
- To KEEP a plugin: include its EXACT name in routing.main.
- To ADD: include all current + new plugin at the right position.
- To REMOVE: list all current EXCEPT the one(s) to remove.
- To SWAP: replace the target name with the new plugin name.
- plugins_to_configure should ONLY include plugins that need NEW parameter generation.
  Retained plugins with unchanged settings should NOT be in plugins_to_configure.
"""

        # Rich slot-state context: show bypassed, user-owned, user-tuned per slot
        if current_chain_entries:
            # Collect currently bypassed names so LLM can preserve state
            currently_bypassed = []
            protected_slots = []
            for i, entry in enumerate(current_chain_entries):
                if i >= len(current_chain_names):
                    break
                flags = []
                if entry.get("user_touched", False):
                    flags.append("USER-TUNED (params sacred)")
                if entry.get("user_owned", False):
                    flags.append("USER-ADDED (keep plugin)")
                if entry.get("bypassed", False):
                    flags.append("BYPASSED (user disabled)")
                    currently_bypassed.append(current_chain_names[i])
                if flags:
                    protected_slots.append(f"  {i+1}. {current_chain_names[i]} — {', '.join(flags)}")
            if protected_slots:
                protected_block = "\n".join(protected_slots)
                base += f"""
=== PROTECTED SLOTS (CRITICAL — RESPECT USER INTENT) ===
The following slots have explicit user intent. Respect each flag:

{protected_block}

SLOT FLAG RULES:
- USER-TUNED: NEVER put in plugins_to_configure (their params are sacred).
  ALWAYS keep in routing.main unless user EXPLICITLY says remove/replace.
- USER-ADDED: ALWAYS keep in routing.main (user chose this plugin deliberately).
  You MAY put in plugins_to_configure if the user hasn't tuned it yet.
- BYPASSED: ALWAYS keep in routing.main AND in routing.bypassed (user explicitly disabled).
  Do NOT put in plugins_to_configure (user doesn't want it active).
  If user says "enable" or "activate" — keep in routing.main but REMOVE from routing.bypassed.
- If user says "tweak everything" or "reconfigure all" — THEN you may include USER-TUNED plugins.
- In your message, acknowledge protected slots briefly.
"""
            # Hint: if there are currently bypassed slots, remind the LLM to preserve them
            if currently_bypassed:
                bypassed_list = ", ".join(f'"{n}"' for n in currently_bypassed)
                base += f"""
CURRENTLY BYPASSED: {bypassed_list}
To preserve bypass state: include these in BOTH routing.main AND routing.bypassed.
To enable: include in routing.main only (remove from routing.bypassed).
"""
    else:
        base += "\n\n=== CURRENT CHAIN === \nEmpty (no plugins loaded). Any build request starts fresh.\n"

    return base


# ---------------------------------------------------------------------------
# Short-confirm expansion
# ---------------------------------------------------------------------------
_SHORT_CONFIRMS = {"давай", "так", "да", "ok", "yes", "do it", "go", "роби",
                   "погнали", "apply", "зроби", "let's go", "yep", "sure",
                   "ок", "добре", "можна", "го"}


def _expand_short_confirm(prompt: str, history: list[dict]) -> str:
    """If prompt is a short confirmation, prepend the last AI message for context."""
    stripped = prompt.strip().lower().rstrip("!.,")
    if stripped not in _SHORT_CONFIRMS:
        return prompt

    # Find last AI message in history
    for msg in reversed(history):
        if msg.get("role") == "assistant":
            last_ai = msg.get("text", "")
            if last_ai:
                expanded = f"[User confirms previous plan. Last AI message: \"{last_ai[:500]}\"]\n\nUser: {prompt}"
                logger.debug(f"Short-confirm expanded: '{prompt}' + last AI context ({len(last_ai)} chars)")
                return expanded
    return prompt


# ---------------------------------------------------------------------------
# Main entry point
# ---------------------------------------------------------------------------
def _empty_routing():
    return {"main": [], "bypassed": []}


def _parse_routing(result: dict) -> dict:
    """Extract routing.main + routing.bypassed from LLM result, with fallback to legacy desired_chain.

    Returns dict with "main", "bypassed", and "bypass_explicit" (bool).
    bypass_explicit=True means LLM explicitly provided the "bypassed" key
    (even if empty list → means "enable all"). False means LLM didn't
    mention bypass at all → caller should preserve current bypass state.
    """
    routing_raw = result.get("routing")

    # Fallback: if LLM returned legacy flat desired_chain instead of routing
    if not routing_raw or not isinstance(routing_raw, dict):
        legacy = result.get("desired_chain", [])
        if legacy:
            logger.warning("FALLBACK: LLM returned legacy desired_chain — wrapping in routing.main")
            return {"main": [str(n).strip() for n in legacy[:12] if n],
                    "bypassed": [], "bypass_explicit": False}
        return {"main": [], "bypassed": [], "bypass_explicit": False}

    main_names = [str(n).strip() for n in routing_raw.get("main", [])[:12] if n]

    # Check if LLM explicitly provided the "bypassed" key (even as empty list)
    bypass_explicit = "bypassed" in routing_raw
    bypassed_names = [str(n).strip() for n in routing_raw.get("bypassed", [])[:12] if n]
    return {"main": main_names, "bypassed": bypassed_names, "bypass_explicit": bypass_explicit}


def _resolve_routing_uids(routing_names: dict, is_stereo: bool) -> dict:
    """Resolve plugin names → UIDs for the main chain + bypassed list."""
    main_uids = names_to_uids(routing_names["main"], is_stereo) if routing_names["main"] else []
    bypassed_names = routing_names.get("bypassed", [])
    bypassed_uids = names_to_uids(bypassed_names, is_stereo) if bypassed_names else []
    return {
        "main_uids": main_uids,
        "main_names": routing_names["main"],
        "bypassed_uids": bypassed_uids,
        "bypassed_names": bypassed_names,
    }


def _collect_all_names(routing_names: dict) -> list[str]:
    """Return all plugin names from the main chain."""
    return list(routing_names.get("main", []))


def unified_agent(prompt: str, audio_context: str,
                  history: list[dict],
                  is_stereo: bool = True,
                  audio_parts: list | None = None,
                  metrics: dict | None = None,
                  current_chain: list[int] | None = None,
                  current_chain_names: list[str] | None = None,
                  current_chain_entries: list[dict] | None = None) -> dict:
    """Single LLM call that handles routing + plugin selection.

    Returns:
        {
            "route": "chat" | "build",
            "message": str,
            "routing": {
                "main_uids": [int, ...],
                "main_names": [str, ...],
            },
            "configure_uids": [int, ...],
            "configure_names": [str, ...],
            "desired_uids": [int, ...],
            "desired_names": [str, ...],
        }
    """
    # Build messages with history
    messages = []
    for msg in history:
        role = msg.get("role", "user")
        if role not in ("user", "assistant"):
            role = "user"
        messages.append({"role": role, "content": msg.get("text", "")})

    # Expand short confirmations with last AI context
    expanded_prompt = _expand_short_confirm(prompt, history)

    # Append user prompt + audio context
    user_content = expanded_prompt
    if audio_context:
        user_content += "\n\n" + audio_context
    messages.append({"role": "user", "content": user_content})

    system = _build_system(is_stereo, current_chain_names, current_chain_entries)

    raw = call_llm(system, messages, max_tokens=800,
                   temperature=0.5,
                   response_schema=UNIFIED_SCHEMA,
                   thinking_budget=0,
                   extra_parts=audio_parts)

    # Parse JSON
    json_str = raw.strip()
    md_match = re.search(r'```(?:json)?\s*\n?([\s\S]*?)```', json_str)
    if md_match:
        json_str = md_match.group(1).strip()

    try:
        result = json.loads(json_str)
    except json.JSONDecodeError:
        obj_match = re.search(r'\{[\s\S]*\}', raw)
        if obj_match:
            try:
                result = json.loads(obj_match.group())
            except json.JSONDecodeError:
                logger.error(f"FAILED to parse JSON: {raw[:300]}")
                result = {"audio_analysis": "", "route": "chat", "message": raw,
                          "routing": _empty_routing(), "plugins_to_configure": []}
        else:
            logger.error(f"No JSON found: {raw[:300]}")
            result = {"route": "chat", "message": raw,
                      "routing": _empty_routing(), "plugins_to_configure": []}

    # Log audio chain-of-thought analysis
    audio_analysis = result.get("audio_analysis", "")
    if audio_analysis:
        logger.debug(f"Audio CoT: {audio_analysis[:200]}")

    route = result.get("route", "chat")
    if route not in ("chat", "build"):
        route = "chat"
    message = result.get("message", "")

    # Parse routing structure (with legacy fallback)
    routing_names = _parse_routing(result)
    configure_names = [str(n).strip() for n in result.get("plugins_to_configure", [])[:12] if n]

    all_desired_names = _collect_all_names(routing_names)
    bypassed_names_parsed = routing_names.get("bypassed", [])
    bypass_explicit = routing_names.get("bypass_explicit", False)
    logger.info(f"route={route}, main={routing_names['main']}, "
          f"bypassed={bypassed_names_parsed} (explicit={bypass_explicit}), "
          f"configure={configure_names}")

    # Resolve names → UIDs
    routing_uids = _resolve_routing_uids(routing_names, is_stereo)
    configure_uids = names_to_uids(configure_names, is_stereo) if configure_names else []

    # Check for unresolved names
    all_desired_uids = list(routing_uids["main_uids"])

    if all_desired_names and len(all_desired_uids) < len(all_desired_names):
        failed = [n for n in all_desired_names if resolve_name_to_uid(n, is_stereo) is None]
        if failed:
            logger.warning(f"Could not resolve: {failed}")

    # Validation: build with no plugins on empty current → fail
    if route == "build" and not all_desired_uids and not current_chain:
        logger.warning(f"Build with no plugins resolved — treating as chat")
        route = "chat"

    # Tier 1 safety net
    if metrics and route == "build":
        peak = metrics.get("peakDB", -90.0)
        bw_icc = metrics.get("bwIcc", [])
        anti_phase = sum(1 for v in bw_icc[:4] if v < 0.0) if bw_icc else 0
        if (peak > 3.0 or anti_phase >= 2) and not history:
            logger.warning("SAFETY NET: severe TIER 1 + no history — prepending warning")
            message = ("⚠️ I detected critical issues in your audio. " + message)

    return {
        "route": route,
        "message": message,
        "routing": routing_uids,
        "configure_uids": configure_uids,
        "configure_names": configure_names,
        # Legacy compat (flat main chain only)
        "desired_uids": routing_uids["main_uids"],
        "desired_names": routing_uids["main_names"],
        # Bypass contract (tri-state)
        "bypassed_uids": routing_uids.get("bypassed_uids", []),
        "bypassed_names": routing_uids.get("bypassed_names", []),
        # True = LLM explicitly provided routing.bypassed key (even if empty).
        # False = LLM didn't mention bypass → preserve current state.
        "_bypass_explicit": bypass_explicit,
    }
