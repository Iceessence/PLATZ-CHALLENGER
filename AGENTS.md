# AGENTS.md — PLATZ Challenger Developer & AI Continuity Contract

**Product:** PLATZ — One Place. Zero Limits.  
**Canonical Challenger Repository:** `C:\PLATZ-CHALLENGER`  
**Protected Other Developer Repository:** `C:\PLATZ` (**STRICTLY OFF-LIMITS** — never inspect, read, copy, or modify `C:\PLATZ` or any related archive).

---

## 1. Mandatory Session Startup Protocol

Every engineering or AI session working in `C:\PLATZ-CHALLENGER` MUST read these files first, in order:

1. `docs/ai/PLATZ_HANDOFF.md` — Current gate status, verified capabilities, exact build/test/launch commands, active blockers, and the next prioritized feature batch.
2. `docs/ai/PLATZ_OWNER_STATUS.md` — Executive summary of verified progress, hardware benchmarks, and capability maturity.
3. `docs/architecture/ADR-0001-foundation.md` and `dependency-lock.json` — Binding technology stack and pinned versions.
4. `docs/qa/PLATZ_VALIDATION_MATRIX.md` — Contract verification ledger and negative test results.
5. Relevant sections of `docs/master/PLATZ_MASTER.md` for the active feature batch.

Do **not** re-run broad web searches for already-pinned libraries, and do **not** re-plan the entire 401-capability constitution every session.

---

## 2. Hard Invariants

1. **Isolation:** All source, third-party extractions, build artifacts (`out/build/`), and dependency caches (`out/deps_cache/`, `out/tools/`) reside strictly inside `C:\PLATZ-CHALLENGER`.
2. **Real Vulkan 1.4 Engine:** No fake 2D isometric projections, mock screenshots, or CPU canvas substitutes for the 3D engine. Vulkan 1.4 + VMA 3.5.0 + Slang 2026.22.1 -> SPIR-V + SDL3 3.4.20 power both the Studio editor viewport and runtime Play mode.
3. **Whole-Feature Completeness (`PLZ-INTEG-001`):** Every new feature must wire together Core state, SQLite/blob persistence, Undo/Redo, Vulkan rendering, Runtime Play isolation, Studio UI/UX, Command Registry/AI safety, and CTest verification in the same batch.
4. **One Application & One Living Validation World (`PLZ-TEST-001`, `PLZ-FOUND-018`):** Build only one primary application (`PLATZ.exe`) and maintain one cumulative validation project at `projects/PLATZ-Validation`.
5. **Safety & Authority (`PLZ-SAFEX-001..011`):**
   - Never use owner accounts, money, credentials, or unauthorized network services.
   - AI edits must pass through the typed, revision-pinned `CommandRegistry` and `StagedSession`.
   - Emergency Stop (`StopSession`) immediately invalidates any in-flight or late AI commit (`N-18`).
   - Protected approval and Stop controls in the Studio UI cannot be hidden or disabled by custom themes (`N-24`).
   - Never self-approve work to `INDEPENDENTLY ACCEPTED`; the maximum developer-assigned status is `VERIFIED` with reproducible test receipts.
