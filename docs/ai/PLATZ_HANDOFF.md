# PLATZ Challenger — Canonical AI & Engineering Handoff (`PLATZ_HANDOFF.md`)

- **Product:** PLATZ — One Place. Zero Limits.
- **Canonical Challenger Repository:** `C:\PLATZ-CHALLENGER`
- **Protected Other Developer Repository:** `C:\PLATZ` (**STRICTLY OFF-LIMITS** — never inspect, read, copy, or modify)
- **Last Updated UTC:** `2026-11-21T17:28:00Z`
- **Highest Developer-Assigned Status:** `VERIFIED` (Never self-approve to `INDEPENDENTLY ACCEPTED`; see `PLZ-OPS-016` / `N-23`)

---

## 1. Current Gate Status Summary

| Gate | Scope | Status | Verification Evidence |
| :--- | :--- | :--- | :--- |
| **Gate 0** | Foundation: Clean isolated Git repo, C++23 MSVC + CMake/CTest Debug & Release presets, 15 SHA-256 pinned dependencies in `dependency-lock.json`, `ADR-0001`, VMA RAII + deferred deletion model, safety invariants | **VERIFIED** | `CMakeLists.txt`, `CMakePresets.json`, `dependency-lock.json`, `docs/architecture/ADR-0001-foundation.md`, `tools/bootstrap_deps.ps1` |
| **Gate 1** | Genuine Integrated PLATZ Vertical Slice: `PLATZ.exe` launches on Windows x64, renders real Vulkan 1.4 3D geometry with depth/perspective on NVIDIA GeForce RTX 5080, hierarchy + Inspector + 3D picking, create/duplicate/delete, scoped undo/redo, isolated Play/Stop, SQLite WAL + SHA-256 content-addressed save/close/reopen | **VERIFIED** | `PLATZ.exe --verify-frames 35` (`ValidationErrors=0`, `StationsPassed=8/8`), `platz_test_unit`, `platz_test_integration`, `platz_test_graphics` |
| **Gate 2** | Real Creative Capability: Wavefront `.obj` & glTF 2.0 (`.gltf`/`.glb`) 3D asset import/export with preserved SHA-256 blob lineage, viewport Translate/Rotate/Scale gizmo with 3D projected axis handles & configurable grid/angle/scale snapping, Slang `2026.22.1` -> SPIR-V 1.5 GGX PBR + Studio Environment Specular IBL + directional/point lighting + shader hot-reload with last-known-good fallback | **VERIFIED** | `src/creative/*`, `shaders/pbr_surface.slang`, `platz_test_unit`, `platz_test_graphics` |
| **Gate 3** | Gameplay & PLATZ AI Foundations: Rigid-body gravity/bounce/friction physics with NaN guards, no-code Event/Condition/Action behavior rules & waypoint patrol, transactional conserved resource transfers, separate gameplay save/load checkpoints, versioned typed `CommandRegistry`, revision-pinned `StagedAiSession` with Emergency Stop revocation, and two local from-scratch neural Specialist models (`12x24x7` Material Specialist + `10x20x5` Gameplay Balance Specialist) with holdout evaluation, negative result memory & 3-Role Coordinated Intelligence workflow | **VERIFIED** | `src/engine/runtime/*`, `src/ai/*`, `platz_test_integration`, `platz_test_security` |
| **Gate 4 (Batch A)** | Professional Studio UX Overhaul & Advanced Authoring: Programmatic `ImGui::DockBuilder` 5-zone docked workstation layout, crisp TrueType vector typography (`Segoe UI` + `Consolas`) across `100%–150%` High-DPI scaling, 2-column non-clipping Property Inspector, 3D projected viewport transform gizmo & orientation rose, Non-Destructive 3D Modifier Stack (`Subdivide`, `TaperTwist`, `BevelChamfer`, `MirrorX`, `VertexNoiseDisplace`), Visual Material Node Graph -> Slang 2026.22.1 compiler & live hot-reload, Interactive 2D Pixel-Art & Sprite Studio with `VK_FILTER_NEAREST` exact preview, and Time-Inspectable Visual Logic Debugger | **VERIFIED** | `src/studio/studio_ui.cpp`, `src/creative/mesh_generator.cpp`, `src/creative/texture_generator.cpp`, `ST-07`, `ST-08`, full-window screenshots in `out/verification_studio_*.png` |

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
# 1. Configure and build Release and Debug presets
$vcvars = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cmd.exe /c "`"$vcvars`" && cmake --preset release && cmake --build --preset release"
cmd.exe /c "`"$vcvars`" && cmake --preset debug && cmake --build --preset debug"

# 2. Run all 4 CTest verification suites (Unit, Integration, Vulkan Graphics, Security/Adversarial)
cmd.exe /c "`"$vcvars`" && ctest --preset release --output-on-failure"
cmd.exe /c "`"$vcvars`" && ctest --preset debug --output-on-failure"

# 3. Launch Interactive PLATZ Studio opening the permanent living validation world:
C:\PLATZ-CHALLENGER\out\build\release\PLATZ.exe --project C:\PLATZ-CHALLENGER\projects\PLATZ-Validation

# 4. Launch Automated Vulkan 1.4 Verification & Full-Window Screenshot Capture Pass:
C:\PLATZ-CHALLENGER\out\build\release\PLATZ.exe --project C:\PLATZ-CHALLENGER\projects\PLATZ-Validation --verify-frames 35 --workspace world --capture-window C:\PLATZ-CHALLENGER\out\verification_studio_window.png
```

- **Verified Release Executable:** `C:\PLATZ-CHALLENGER\out\build\release\PLATZ.exe`
- **Verified Debug Executable:** `C:\PLATZ-CHALLENGER\out\build\debug\PLATZ.exe`
- **Verification Receipt Output:** `C:\PLATZ-CHALLENGER\out\verification_receipt.json`
- **Full Studio Window Screenshots (`1920x1040`):**
  - `C:\PLATZ-CHALLENGER\out\verification_studio_window.png` (`100%` DPI, World & Asset Browser Workspace)
  - `C:\PLATZ-CHALLENGER\out\verification_studio_materials_hidpi.png` (`125%` High-DPI, Material Node Graph & Slang Compiler Workspace)
  - `C:\PLATZ-CHALLENGER\out\verification_studio_pixelart.png` (`100%` DPI, 2D Pixel-Art & Sprite Studio Workspace)
  - `C:\PLATZ-CHALLENGER\out\verification_studio_ai_workbench.png` (`100%` DPI, Logic & PLATZ AI Dual-Specialist Workbench)

---

## 4. Permanent Living Validation World (`projects/PLATZ-Validation`)

`C:\PLATZ-CHALLENGER\projects\PLATZ-Validation` contains one SQLite WAL database (`project.platzdb`), content-addressed SHA-256 asset blobs (`assets/blobs/sha256/...`), and **8 cumulative interactive test stations (`ST-01` through `ST-08`)**:
1. **`ST-01` (Spatial Hierarchy, Identity & Gizmo Snapping):** Parent pedestal + child beacon + duplicate-named `"BeaconNode"` pair (`OBJ-000003EA` vs `OBJ-000003EB`) proving `N-01` stable identity disambiguation.
2. **`ST-02` (Slang PBR Metallic-Roughness & Lighting Gallery):** Directional sun, local point light, brushed titanium sphere, polished gold torus, and ceramic glaze sphere rendered via `shaders/pbr_surface.slang` with procedural Studio Environment Specular IBL.
3. **`ST-03` (3D Mesh Import & Content-Addressed Asset Lineage):** Subdivided & Wavefront `.obj` round-tripped crystal mesh stored in `assets/blobs/sha256/e4/e43587be9f49a50c...blob` with `CC0-1.0` license metadata.
4. **`ST-04` (Rigid-Body Physics, Restitution & NaN Guard):** Dynamic gravity sphere with ground collision restitution bounce, friction damping, and NaN/Inf velocity sanitization (`N-10`).
5. **`ST-05` (No-Code Visual Behavior & Conserved Inventory):** `ST05_NoCode_Energy_Generator` and `ST05_Conserved_Energy_Vault` executing `OnTick`, `OnInteract`, state transitions, and transactional conserved `energy_crystal` transfers (`total = 100` invariant across interruptions, `N-09`).
6. **`ST-06` (PLATZ AI Staged Commands & Native Specialist Workbench):** Monument targeted by revision-pinned `StagedAiSession` proposals and the native `12x24x7` + `10x20x5` neural specialist models.
7. **`ST-07` (Non-Destructive 3D Modifier Stack & Material Graph Compiler):** `ST07_ModifierStack_Twisted_Spire` evaluating an ordered stack (`Subdivide` + `TaperTwist`) on the GPU and compiling a 4-node Material Graph to Slang -> SPIR-V 1.5.
8. **`ST-08` (2D Pixel-Exact Sprite & Time-Inspectable Patrol Logic):** `ST08_PixelSprite_Patrol_Drone` executing `PatrolWaypoints` with time-travel causal logic debugger timeline snapshots and nearest-neighbor `16x16` pixel emblem.

---

## 5. Highest-Value Next Prioritized Feature Batch (Gate 4 Batch B)

The next session should implement **Gate 4 Batch B — Keyframed Animation Timeline Sequencer, Spatial Audio Mixer & Standalone Game Export Runner**:
1. **Unified Keyframe Timeline Sequencer (`PLZ-MEDIA-001..002`, `PLZ-CREATEX-006`):** Multi-track keyframe curve editor in the bottom drawer for animating object transforms, camera cuts, light intensities, and material parameters with editor scrub preview and isolated Play execution.
2. **Procedural Spatial Audio & DSP Graph (`PLZ-MEDIA-008..009`, `PLZ-CREATEX-007`, `N-15`):** SDL3 audio stream output with synthesized/WAV sound cues triggered by no-code `OnInteract`/`OnCollision` rules and tested under heavy GPU shader compile load (`N-15`).
3. **Validation World Station `ST-09`:** Add `ST-09` to `projects/PLATZ-Validation` exercising multi-track cinematic keyframe sequencing and spatial audio triggers while preserving `ST-01..ST-08`.
