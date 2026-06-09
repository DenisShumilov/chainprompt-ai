# Release Policy

## 1. Purpose
This is a future-only posture note, not part of the hard canon.

Its job is to keep release language honest once the project moves beyond internal development use.

## 2. Current Reality
The project is still stabilization-first.

Current safe assumption:

**Internal Development Build only**

Do not describe the product as broadly release-ready while these remain materially unresolved:
- developer-operated backend lifecycle
- fragile path/bootstrap behavior
- incomplete trust-critical UX
- incomplete packaging
- unresolved restore and mutation risk

## 3. What Must Be True Before Broader Release
Before any broader release level is claimed, the project should have:
- explicit release scope
- explicit known limitations
- reproducible enough build or package flow
- understandable startup and degraded-mode behavior
- acceptable restore safety
- QA evidence appropriate to the target audience

## 4. Future Priority Areas
When release work becomes active, focus on:
- packaging and bootstrap
- restore safety
- degraded-mode visibility
- support and runbook readiness
- honest release notes

## 5. Practical Rule
Sprint acceptance is not release readiness.

Treat this file as future planning context only, not as mandatory reading for normal maintainer runs.
