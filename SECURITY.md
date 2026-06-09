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

Current security caveats:

- The Flask app currently enables broad CORS for local development.
- `/api/chat` can accept an `audioPath` and read audio from disk.
- Debug logs may include prompt snippets, model-output snippets, track names, audio paths, and analysis context.

Before public or broader internal testing, add a path allowlist or remove `audioPath`, gate debug logging, and keep the backend bound to localhost unless a proper auth and transport model exists.

## Data Sent to the Model

The backend may send prompt text, chain context, plugin metadata, audio metrics, and optional audio payloads to Gemini depending on the request path.

Before public release, the project would need:

- explicit user-facing privacy language
- clear opt-in behavior for any audio upload
- local redaction rules for sensitive project/session data
- configurable model/provider settings
- logging review to avoid storing sensitive prompts or paths unnecessarily

## Repository Hygiene

Before uploading this project to another GitHub account:

- run a secret scan on the working tree
- run a secret scan on git history
- confirm `.env` is absent from git
- confirm local assistant/tool settings are not tracked
- review screenshots and demo videos for exposed API keys, local usernames, private file paths, and licensed plugin serial data
- if creating a new GitHub repository, prefer a fresh clean repo or history rewrite so local-only files such as `.claude/` never existed in reachable history

## Reporting

For now, this is a private portfolio prototype. If the repository becomes public, add a clear vulnerability reporting contact before accepting external users or issues.
