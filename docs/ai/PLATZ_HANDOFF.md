# PLATZ Challenger — Canonical AI & Engineering Handoff (`PLATZ_HANDOFF.md`)

- **Product:** PLATZ — One Place. Zero Limits.
- **Canonical Challenger Repository:** `C:\PLATZ-CHALLENGER`
- **Protected Other Developer Repository:** `C:\PLATZ` (**STRICTLY OFF-LIMITS** — never inspect, read, copy, or modify)
- **Last Updated UTC:** `2026-11-21T16:50:00Z`
- **Highest Developer-Assigned Status:** `VERIFIED` (Never self-approve to `INDEPENDENTLY ACCEPTED`; see `PLZ-OPS-016` / `N-23`)

---

## 1. Current Gate Status Summary

| Gate | Scope | Status | Verification Evidence |
| :--- | :--- | :--- | :--- |
| **Gate 0** | Foundation: Clean isolated Git repo, C++23 MSVC + CMake/CTest Debug & Release presets, 15 SHA-256 pinned dependencies in `dependency-lock.json`, `ADR-0001`, VMA RAII + deferred deletion model, safety invariants | **VERIFIED** | `CMakeLists.txt`, `CMakePresets.json`, `dependency-lock.json`, `docs/architecture/ADR-0001-foundation.md`, `tools/bootstrap_deps.ps1` |
| **Gate 1** | Genuine Integrated PLATZ Vertical Slice: `PLATZ.exe` launches on Windows x64, renders real Vulkan 1.4 3D geometry with depth/perspective on NVIDIA GeForce RTX 5080, hierarchy + Inspector + 3D picking, create/duplicate/delete, scoped undo/redo, isolated Play/Stop, SQLite WAL + SHA-256 content-addressed save/close/reopen | **VERIFIED** | `PLATZ.exe --verify-frames 60` (`ValidationErrors=0`, `StationsPassed=6/6`), `platz_test_unit`, `platz_test_integration`, `platz_test_graphics` |
| **Gate 2** | Real Creative Capability: Wavefront `.obj` & glTF 2.0 (`.gltf`/`.glb`) 3D asset import/export with preserved SHA-256 blob lineage, non-destructive subdivision modifier, viewport Translate/Rotate/Scale gizmo with configurable grid/angle/scale snapping, Slang `2026.22.1` -> SPIR-V 1.5 GGX PBR metallic-roughness + directional/point lighting + shader hot-reload with last-known-good fallback | **VERIFIED** | `src/creative/*`, `shaders/pbr_surface.slang`, `platz_test_unit`, `platz_test_graphics` |
| **Gate 3** | Gameplay & PLATZ AI Foundations: Rigid-body gravity/bounce/friction physics with NaN guards, no-code Event/Condition/Action behavior rules & state transitions, transactional conserved resource transfers, separate gameplay save/load checkpoints, versioned typed `CommandRegistry`, revision-pinned `StagedAiSession` with Emergency Stop revocation, and local from-scratch `12x24x7` neural Material Specialist model with holdout evaluation & negative result memory | **VERIFIED** | `src/engine/runtime/*`, `src/ai/*`, `platz_test_integration`, `platz_test_security` |
| **Gate 4+** | Advanced Creator Platform Expansion: Multi-layer 2D raster/vector/tilemap canvas editor, node-based visual material graph compiler, skeletal animation timeline, standalone executable game packager, multi-specialist orchestration | **BACKLOG (Next Priority)** | Tracked in Section 6 below with honest UI completion badges (`PLZ-UXPLUS-008`) |

---

## 2. Exact Pinned Toolchain & Dependencies (`dependency-lock.json`)

- **Compiler:** MSVC `19.51.36260.0` (Visual Studio 2026 Community `18.10.3`) with `/std:c++latest` (**C++23**)
- **Build System:** CMake `4.3.1-msvc1` + Ninja `1.13.2`
- **GPU & Driver:** NVIDIA GeForce RTX 5080 (`Vulkan 1.4.351`, Driver `617.42`, Dynamic Rendering, Synchronization2, Timeline Semaphores)
- **Pinned Libraries (all verified by SHA-256 in `out/deps_cache`):**
  - `Vulkan-Headers` `1.4.363.0` & `volk` `1.4.363.0`
  - `VulkanMemoryAllocator (VMA)` `3.5.0`
  - `SDL3` `3.4.20`
  - `Slang` `2026.22.1` (`slangc.exe`)
  - `SQLite` `3.54.1` (`sqlite-amalgamation-3540100`, WAL mode)
  - `Dear ImGui (Docking)` `1.93.0-docking`
  - `GLM` `1.0.3`, `nlohmann/json` `3.12.0`, `doctest` `2.5.3`
  - `cgltf` `1.15-20260202`, `tinyobjloader` `2.0.0rc13`, `stb_image` `2.30`, `stb_image_write` `1.16`
  - `Vulkan-ValidationLayers` `1.4.363-windows-x64` (loaded via isolated `VK_ADD_LAYER_PATH`)

---

## 3. Exact Build, Test, and Launch Commands

Run from PowerShell (initializes Visual Studio 2026 x64 environment automatically):

```powershell
# 1. Configure and build Release (and/or Debug)
$vcvars = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cmd.exe /c "`"$vcvars`" && cmake --preset release && cmake --build --preset release"
cmd.exe /c "`"$vcvars`" && cmake --preset debug && cmake --build --preset debug"

# 2. Run all 4 CTest verification suites (Unit, Integration, Vulkan Graphics, Security/Adversarial)
cmd.exe /c "`"$vcvars`" && ctest --preset release --output-on-failure"
cmd.exe /c "`"$vcvars`" && ctest --preset debug --output-on-failure"

# 3. Launch Interactive PLATZ Studio opening the permanent living validation world:
C:\PLATZ-CHALLENGER\out\build\release\PLATZ.exe --project C:\PLATZ-CHALLENGER\projects\PLATZ-Validation

# 4. Launch Automated 60-Frame Vulkan 1.4 Verification & Receipt Generation Pass:
C:\PLATZ-CHALLENGER\out\build\release\PLATZ.exe --project C:\PLATZ-CHALLENGER\projects\PLATZ-Validation --verify-frames 60
```

- **Verified Release Executable:** `C:\PLATZ-CHALLENGER\out\build\release\PLATZ.exe`
- **Verified Debug Executable:** `C:\PLATZ-CHALLENGER\out\build\debug\PLATZ.exe`
- **Verification Receipt Output:** `C:\PLATZ-CHALLENGER\out\verification_receipt.json`
- **Rendered 3D Viewport Frame Capture:** `C:\PLATZ-CHALLENGER\out\verification_viewport_capture.png`

---

## 4. Permanent Living Validation World (`projects/PLATZ-Validation`)

`C:\PLATZ-CHALLENGER\projects\PLATZ-Validation` contains one SQLite WAL database (`project.platzdb`), content-addressed SHA-256 asset blobs (`assets/blobs/sha256/...`), and **6 cumulative interactive test stations**:
1. **`ST-01` (Spatial Hierarchy, Identity & Gizmo Snapping):** Parent pedestal + child beacon + duplicate-named `"BeaconNode"` pair (`OBJ-000003EA` vs `OBJ-000003EB`) proving `N-01` stable identity disambiguation.
2. **`ST-02` (Slang PBR Metallic-Roughness & Lighting Gallery):** Directional sun, local point light, brushed titanium sphere, polished gold torus, and ceramic glaze sphere rendered via `shaders/pbr_surface.slang`.
3. **`ST-03` (3D Mesh Import & Content-Addressed Asset Lineage):** Subdivided & Wavefront `.obj` round-tripped crystal mesh stored in `assets/blobs/sha256/e4/e43587be9f49a50c...blob` with `CC0-1.0` license metadata.
4. **`ST-04` (Rigid-Body Physics, Restitution & NaN Guard):** Dynamic gravity sphere with ground collision restitution bounce, friction damping, and NaN/Inf velocity sanitization (`N-10`).
5. **`ST-05` (No-Code Visual Behavior & Conserved Inventory):** `ST05_NoCode_Energy_Generator` and `ST05_Conserved_Energy_Vault` executing `OnTick`, `OnInteract`, state transitions, and transactional conserved `energy_crystal` transfers (`total = 100` invariant across interruptions, `N-09`).
6. **`ST-06` (PLATZ AI Staged Commands & Native Specialist Workbench):** Monument targeted by revision-pinned `StagedAiSession` proposals and the native `12x24x7` MLP Material Specialist model.

---

## 5. Active Blockers & Known Risks

- **Active Blockers:** None. All Release and Debug builds, all 4 CTest suites, and `PLATZ.exe` interactive + headless verification passes succeed with 0 errors and 0 Vulkan validation errors.
- **Known Technical Risks Managed:**
  - *Swapchain Semaphore Reuse (`VUID-vkQueueSubmit-pSignalSemaphores-00067`):* Solved by allocating `swapchainRenderFinishedSemaphores_` per swapchain image rather than per frame-in-flight slot.
  - *Custom Theme Security Spoofing (`N-24`):* `StudioUiController::ApplyThemeWithProtectedControlGuard` enforces 100% opacity and high contrast on the AI Approval & Emergency Stop bar regardless of user theme settings.

---

## 6. Highest-Value Next Prioritized Feature Batch (Gate 4 First Slice)

The next session should implement **Gate 4 Batch A — Integrated 2D Pixel/Tilemap Authoring & Visual Material Node Graph Compiler** in `C:\PLATZ-CHALLENGER`:
1. **2D Pixel-Art & Tilemap Canvas Panel (`PLZ-ART-007..009`, `PLZ-ARCH-005`):** Interactive layered pixel canvas in Studio using `TextureGenerator`'s nearest-neighbor Vulkan texture pipeline, saving layered sprite sheets into `ContentAddressedAssetStore` with live 3D/2D scene preview and undo/redo.
2. **Visual Material Graph -> Slang Code Generator (`PLZ-MAT-001`, `PLZ-RENDERX-002`):** Editable typed material node graph (Albedo, Noise/Checker, Metallic/Roughness, Fresnel Rim, Emissive) that compiles graph nodes into `.slang` shader code and hot-reloads the Vulkan 1.4 pipeline via `VulkanRenderer::HotReloadPbrShaderFromSlangSource` with last-known-good fallback.
3. **Validation World Station `ST-07`:** Add `ST-07` to `projects/PLATZ-Validation` exercising live 2D sprite/tilemap edits and Material Graph -> Slang hot-reload while preserving `ST-01..ST-06`.
