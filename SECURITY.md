# Security and Privacy Notes

ChainPrompt AI is a developer-operated portfolio prototype, not a production-ready product. This file documents the current security posture and the rules for handling local secrets and user data.

## API Keys

- Store Gemini keys only in a local `.env` file.
- Do not commit `.env`.
- Use `.env.example` only for placeholders.
- Rotate any key that was ever committed, pasted into logs, or shared in a screenshot.

Expected local format:

```bash
GEMINI_API_KEYS=your_gemini_api_key_here
```

## Local Backend

The Python backend runs locally on:

```text
http://127.0.0.1:5000
```

It is intended for local development and demo use. It has not been hardened as a network-exposed service.

Current posture:

- The backend binds to `127.0.0.1` only and does not enable CORS, so browser pages cannot read the unauthenticated local endpoints cross-origin.
- `/api/chat` accepts an `audioPath`, restricted to the plugin's capture directory (`<temp>/ChainPromptAI/`); paths outside it are rejected and the request continues without audio context.
- Debug logging is opt-in (`CHAINPROMPT_DEBUG=1`). When enabled, logs may include prompt snippets, model-output snippets, and track names.
- Local endpoints remain unauthenticated. Before broader internal testing, add an auth/transport model and review logging again.

## Data Sent to the Model

The backend may send prompt text, chain context, plugin metadata, audio metrics, and optional audio payloads to Gemini depending on the request path.

Before public release, the project would need:

- explicit user-facing privacy language
- clear opt-in behavior for any audio upload
- local redaction rules for sensitive project/session data
- configurable model/provider settings
- logging review to avoid storing sensitive prompts or paths unnecessarily

## Repository Hygiene

- `.env` is git-ignored; no API keys or secrets are tracked in this repository.
- A secret scan of the working tree and full git history was run before sharing (Gemini/OpenAI/GitHub token patterns, `.env` files).
- Demo media, local captures, and other generated artifacts are excluded from version control.
- Large catalog files are stored via Git LFS.

## Reporting

For now, this is a private portfolio prototype. If the repository becomes public, add a clear vulnerability reporting contact before accepting external users or issues.
