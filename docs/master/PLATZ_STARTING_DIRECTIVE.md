# PLATZ — INDEPENDENT AI CHALLENGER: COMPLETE STARTING DIRECTIVE

**Read and follow the attached `PLATZ_CHALLENGER_MASTER.md` as your authoritative product specification.** This directive tells you how to implement it in your own repository and produce directly comparable, verified results. The 401 PLZ capability contracts are all retained as long-term scope; do not replace them with your own reduced vision.

**Product name:** PLATZ — One Place. Zero Limits.  
**Your independent repository root:** `C:\PLATZ-CHALLENGER`  
**Other developer's protected repository:** `C:\PLATZ` (do not inspect or modify)  
**Primary objective:** Build the **real** unified PLATZ creation application, not a demo or tutorial, using the same specified Vulkan-first foundations and first-release tests as the other developer.

## 1. Fair independent implementation

You are a separate competing developer, building the **same product independently** so the owner can compare actual engineering quality, usability, correctness, development efficiency and PLATZ AI progress. Do not open, copy, import, reverse engineer, compile, or modify anything in `C:\PLATZ` or its related old archives. Do not ask the owner to send the other developer's source. The second implementation belongs exclusively in `C:\PLATZ-CHALLENGER`.

Create the directory only if it is safe and not already occupied by unrelated content. This is the only canonical project root for your implementation. The application's actual executable may be called `PLATZ.exe`, but no global installer or file association may conflict with the other developer's app. Use an isolated dependency/build cache when possible. Never share writable build or source locations with the original implementation.

Follow the same product Master, first four Gate definitions, acceptance tests, project-data invariants, UX standards and permanent internal validation-project rule. A comparable result does not require copying the other AI's architecture or design. Record why you chose each allowed supporting library and make competing solutions independently. Avoid scoring yourself as the winner; report verifiable evidence for an independent review.

## 2. Lock a strong, current technology baseline—quickly

Your default starting architecture is binding unless a demonstrable technical defect warrants an owner-approved exception:

- **Native C++23** for Core/Engine/Runtime, with documented C++20 fallback only if necessary.
- **Vulkan 1.4** as the primary graphics API, with actual feature negotiation on the target GPU. Do not choose Direct3D 12 as the primary renderer, and do not substitute Qt/WPF/CPU-side illustration for 3D.
- **SDL3** for platform, windowing, input and Vulkan surface support.
- **Vulkan Memory Allocator (VMA)** for low-level GPU allocation; PLATZ owns resource lifetime, frame pools, streaming, budgets, residency and AI/render workload scheduling.
- **Slang-authored PLATZ shader code compiled to SPIR-V** and validated through SPIR-V Tools; use an explicitly justified HLSL/DXC fallback if Slang qualification fails. Do not write a custom shader compiler.
- **PLATZ-owned professional Studio UI architecture**, using mature supporting libraries for layout/text/docking/accessibility where needed. The UI is integrated with real Core/Engine objects, not a separate imitation or debug panel. Evaluate at most two credible UI foundations in one short qualification; record selection. Do not automatically use Qt because another developer may have done so.
- **PLATZ-owned canonical project format** backed by SQLite for appropriate metadata/revisions/transactions plus scalable integrity-checked content-addressed large assets; do not build a custom SQL database engine.
- **CMake/CTest/Git** with repeatable Debug and Release presets, pinned dependencies, appropriate testing and packaging tools.

Use the **newest suitable non-prerelease stable releases verified from official sources at the start**. As of November 21, 2026, candidates include LunarG Vulkan SDK 1.4.363, SDL 3.4.20, VMA 3.5.0, Slang 2026.22.1, SQLite 3.54.1, and CMake 4.5. Verify exact available releases and compatibility once; then pin versions, hashes/commits where appropriate and licenses in `dependency-lock.json`.

## 3. Create the permanent repo and exact entry points

Create a clean Git repository at `C:\PLATZ-CHALLENGER` with a concise documented structure matching the Master: root CMake project, `apps/studio`, `src/core`, `src/engine/render/vulkan`, `src/engine/runtime`, `src/platform`, `src/studio`, `src/creative`, `src/ai`, `shaders`, `tests`, `projects/PLATZ-Validation`, `docs/master`, `docs/architecture`, `docs/ai`, and generated outputs under ignored `out/`.

## 4. Implement a real Vulkan renderer—not a test facade

Build actual resource/device ownership with correct Vulkan initialization, queues, presentation, resize/minimize, dynamic rendering where appropriate, depth buffers, true GPU geometry, perspective camera, transforms, picking, descriptor/resource safety, synchronization, shader validation and error reporting. Use VMA with appropriate RAII wrappers, staging/upload/readback paths, deferred GPU destruction and memory tracking. Use Slang → SPIR-V with validation, reflection/layout checks, cache rules and a path for shader hot reload.

## 5. Make professional UI/UX advance with every feature

The application must be usable and attractive from its first meaningful release: real editor viewport, hierarchy, Inspector, project/asset interface, Create/Edit/Save/Reopen, numeric transforms, selection feedback and Play/Stop. Make keyboard focus, high-DPI layout, clipboard, readable text, recoverable panel layouts and accessible navigation part of the implementation.

## 6. Persistence and actual runtime

Maintain canonical stable object IDs, typed components, revisions, transactions, undo/redo and asset lineage. Use SQLite for appropriate structured data and a content-addressed asset store for large source data. Stage asset writes safely, verify hashes, avoid partial revision head advancement and recover from process termination during a write. Runtime Play operates on a safe projection/copy of canonical editor state and cannot silently corrupt it when stopped.

## 7. Same initial acceptance gates as the original developer

- **Gate 0 — Foundation**
- **Gate 1 — Genuine integrated PLATZ**
- **Gate 2 — Real creative capability**
- **Gate 3 — Gameplay and PLATZ AI**
- **Gate 4 — Advanced creator platform**
