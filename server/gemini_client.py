"""
ChainPrompt AI — Gemini LLM API wrapper with key rotation, retry & fallback.
"""

import json
import logging
import re
import time

from google import genai as genai_sdk
from google.genai import types as genai_types

from .config import GEMINI_API_KEYS, GEMINI_MODEL, GEMINI_FALLBACK_MODEL

logger = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Retry configuration
# ---------------------------------------------------------------------------
MAX_ATTEMPTS = 3
BACKOFF_SECONDS = [1.5, 3.0]  # wait between attempt 1→2, 2→3

# Errors that trigger a retry (substring match on exception message)
_RETRIABLE_ERRORS = {"RESOURCE_EXHAUSTED", "429", "503", "UNAVAILABLE",
                     "DEADLINE_EXCEEDED", "INTERNAL", "502", "500"}


def _is_retriable(error: Exception) -> bool:
    """Check if an exception is retriable."""
    err_str = str(error)
    return any(code in err_str for code in _RETRIABLE_ERRORS)


# ---------------------------------------------------------------------------
# Single-attempt LLM call (tries all API keys for one model)
# ---------------------------------------------------------------------------
def _call_single_attempt(model: str, contents: list, config: genai_types.GenerateContentConfig,
                         response_schema: dict | None) -> str | None:
    """Try all API keys for a given model. Returns text or None."""
    for key_idx, api_key in enumerate(GEMINI_API_KEYS):
        client = genai_sdk.Client(api_key=api_key)
        try:
            response = client.models.generate_content(
                model=model,
                contents=contents,
                config=config,
            )
            content = response.text
            if content is None:
                logger.warning(f"Key#{key_idx+1} {model} returned empty text, trying next key...")
                continue

            content = content.strip()
            content = re.sub(r'<think>[\s\S]*?</think>', '', content).strip()

            # Validate JSON if schema was expected
            if response_schema and content:
                try:
                    json.loads(content)
                except json.JSONDecodeError:
                    logger.warning(f"Key#{key_idx+1} {model} returned malformed JSON, trying next key...")
                    continue

            usage = response.usage_metadata
            think_tokens = getattr(usage, 'thoughts_token_count', 0) or 0
            total_tokens = getattr(usage, 'total_token_count', 0) or 0
            think_info = f", thinking={think_tokens}tok" if think_tokens else ""
            logger.info(f"OK {model} ({len(content)} chars, "
                  f"total={total_tokens}tok{think_info}): {content[:200]}...")
            return content

        except Exception as e:
            err_str = str(e)
            if "RESOURCE_EXHAUSTED" in err_str or "429" in err_str:
                logger.warning(f"Key#{key_idx+1} {model} quota exhausted, trying next key...")
            else:
                logger.error(f"Key#{key_idx+1} {model} error: {e}")
            if not _is_retriable(e):
                raise  # Non-retriable error — don't waste time on more keys
            continue

    return None  # All keys exhausted for this model


# ---------------------------------------------------------------------------
# Public API: call_llm with retry + fallback
# ---------------------------------------------------------------------------
def call_llm(system_text: str, messages: list[dict],
             max_tokens: int = 512,
             temperature: float = 0.7,
             response_schema: dict | None = None,
             thinking_budget: int | None = None,
             extra_parts: list | None = None) -> str:
    """Call Gemini with retry logic and automatic model fallback.

    Attempt 1: primary model (GEMINI_MODEL)
    Attempt 2-3: fallback model (GEMINI_FALLBACK_MODEL) with backoff
    If all fail: return safe empty JSON.
    """
    contents = []
    for i, msg in enumerate(messages):
        role = "model" if msg["role"] == "assistant" else msg["role"]
        parts = [genai_types.Part(text=msg["content"])]
        if extra_parts and i == len(messages) - 1 and role == "user":
            parts.extend(extra_parts)
        contents.append(genai_types.Content(role=role, parts=parts))

    config_kwargs: dict = {
        "temperature": temperature,
        "max_output_tokens": max_tokens,
        "system_instruction": system_text,
    }

    if response_schema:
        config_kwargs["response_mime_type"] = "application/json"
        config_kwargs["response_schema"] = response_schema

    if thinking_budget is not None:
        config_kwargs["thinking_config"] = genai_types.ThinkingConfig(
            thinking_budget=thinking_budget
        )

    config = genai_types.GenerateContentConfig(**config_kwargs)

    last_error = None
    for attempt in range(MAX_ATTEMPTS):
        # Attempt 1 = primary model, 2+ = fallback
        model = GEMINI_MODEL if attempt == 0 else GEMINI_FALLBACK_MODEL

        if attempt > 0:
            backoff = BACKOFF_SECONDS[min(attempt - 1, len(BACKOFF_SECONDS) - 1)]
            logger.warning(f"Retry #{attempt} after {backoff}s backoff, switching to {model}")
            time.sleep(backoff)

        think_str = f"budget={thinking_budget}" if thinking_budget is not None else "default"
        logger.debug(f"Attempt {attempt+1}/{MAX_ATTEMPTS}: {model}, {len(contents)} msgs, "
              f"schema={'yes' if response_schema else 'no'}, "
              f"thinking={think_str}, "
              f"audio={'yes' if extra_parts else 'no'}, temp={temperature}")

        try:
            result = _call_single_attempt(model, contents, config, response_schema)
            if result is not None:
                return result
            last_error = RuntimeError(f"All API keys returned empty for {model}")
        except Exception as e:
            last_error = e
            if not _is_retriable(e):
                logger.error(f"Non-retriable error: {e} — aborting retries")
                break

    # All attempts failed — return safe fallback
    logger.error(f"ALL {MAX_ATTEMPTS} ATTEMPTS FAILED. Last error: {last_error}")
    logger.error("Returning safe fallback response")

    if response_schema:
        # Return minimal valid JSON matching UNIFIED_SCHEMA
        return json.dumps({
            "audio_analysis": "",
            "route": "chat",
            "message": "The AI core is currently overloaded. Please try again in a moment.",
            "routing": {"main": []},
            "plugins_to_configure": [],
        })
    else:
        return "The AI core is currently overloaded. Please try again in a moment."
