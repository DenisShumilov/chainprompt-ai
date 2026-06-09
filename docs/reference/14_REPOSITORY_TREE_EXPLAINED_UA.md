# Repository Tree Explained (UA)

## 1. For What This File Exists
This file is onboarding-only.

It is meant for a human who wants a quick plain-language map of the repository.

Agents should not read this by default. The canonical sources are:
- `docs/01_PRODUCT_VISION.md`
- `docs/02_ARCHITECTURE.md`
- `docs/04_REPOSITORY_CANON.md`
- `docs/05_SPRINT_BASELINE.md`
- `docs/06_AGENT_OPERATING_MODEL.md`
- `docs/12_RISK_REGISTER.md`

## 2. Project in One Paragraph
`ChainPrompt AI` is a local AI-assisted system for safe serial insert-chain work in a DAW.

The C++ plugin lives inside the DAW, the Python backend does planning and parameter reasoning, and local safety logic decides what is actually allowed to change.

## 3. Simplified Repository Map
```text
chainprompt-ai/
  CLAUDE.md
  docs/
    01_PRODUCT_VISION.md
    02_ARCHITECTURE.md
    04_REPOSITORY_CANON.md
    05_SPRINT_BASELINE.md
    06_AGENT_OPERATING_MODEL.md
    12_RISK_REGISTER.md
    future/
    reference/
  prompts/
    01_ANALYST_MASTER.md
    02_CODER_REVIEWER_MASTER.md
    03_ANALYST_INPUT_TEMPLATE.md
    05_BUGFIX_MODE_TEMPLATE.md
    06_QA_REPLY_TEMPLATE.md
  server/
  plugin/
  data/
```

## 4. Who Owns What
- `plugin/`: DAW-side runtime, graph, UI, scanning, state handling
- `server/`: backend planning, diff, metadata, safety filtering, memory
- `data/`: catalogs and metadata assets
- `docs/`: canonical project memory
- `prompts/`: agent workflow contracts and helper templates

## 5. Best Fast Reading Order For a Human
1. `README.md`
2. `docs/01_PRODUCT_VISION.md`
3. `docs/02_ARCHITECTURE.md`
4. `docs/04_REPOSITORY_CANON.md`
5. `docs/05_SPRINT_BASELINE.md`

## 6. Key Truths
- Real installed plugin inventory is authoritative.
- The AI is not execution authority.
- User edits and restore safety matter more than clever automation.
