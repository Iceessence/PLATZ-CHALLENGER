# PLATZ Challenger — Canonical AI & Engineering Handoff (`PLATZ_HANDOFF.md`)

- **Product:** PLATZ — One Place. Zero Limits.
- **Canonical Challenger Repository:** `C:\PLATZ-CHALLENGER`
- **Protected Other Developer Repository:** `C:\PLATZ` (**STRICTLY OFF-LIMITS** — never inspect, read, copy, or modify)
- **Last Updated UTC:** `2026-11-21T20:13:00Z`
- **Highest Developer-Assigned Status:** `VERIFIED` (Never self-approve to `INDEPENDENTLY ACCEPTED`; see `PLZ-OPS-016` / `N-23`)

---

## 1. Current Gate Status Summary

| Gate | Scope | Status | Verification Evidence |
| :--- | :--- | :--- | :--- |
| **Gate 0** | Foundation: Clean isolated Git repo, C++23 MSVC + CMake/CTest Debug & Release presets, 15 SHA-256 pinned dependencies in `dependency-lock.json`, `ADR-0001`, VMA RAII + deferred deletion model, safety invariants | **VERIFIED** | `CMakeLists.txt`, `CMakePresets.json`, `dependency-lock.json`, `docs/architecture/ADR-0001-foundation.md`, `tools/bootstrap_deps.ps1` |
| **Gate 1** | Genuine Integrated PLATZ Vertical Slice: `PLATZ.exe` launches on Windows x64, renders real Vulkan 1.4 3D geometry with depth/perspective on NVIDIA GeForce RTX 5080, hierarchy + Inspector + 3D picking, create/duplicate/delete, scoped undo/redo, isolated Play/Stop, SQLite WAL + SHA-256 content-addressed save/close/reopen | **VERIFIED** | `PLATZ.exe --benchmark-frames 300` (`ValidationErrors=0`, `StationsPassed=9/9`), `platz_test_unit`, `platz_test_integration`, `platz_test_graphics` |
| **Gate 2** | Real Creative Capability: Wavefront `.obj` & glTF 2.0 (`.gltf`/`.glb`) 3D asset import/export with preserved SHA-256 blob lineage, viewport Translate/Rotate/Scale gizmo with 3D projected axis handles & configurable grid/angle/scale snapping, Slang `2026.22.1` -> SPIR-V 1.5 GGX PBR + Studio Environment Specular IBL + directional/point lighting + shader hot-reload with last-known-good fallback | **VERIFIED** | `src/creative/*`, `shaders/pbr_surface.slang`, `platz_test_unit`, `platz_test_graphics` |
| **Gate 3** | Gameplay & PLATZ AI Foundations: Rigid-body gravity/bounce/friction physics with NaN guards, no-code Event/Condition/Action behavior rules & waypoint patrol, transactional conserved resource transfers, separate gameplay save/load checkpoints, versioned typed `CommandRegistry`, revision-pinned `StagedAiSession` with Emergency Stop revocation, and two local from-scratch neural Specialist models (`12x24x7` Material Specialist + `10x20x5` Gameplay Balance Specialist) with real held-out predictions (`MSE vs Baseline`, `Accuracy`, `MAE`, `R²`, multi-pass timed latency), negative result memory & 3-Role Coordinated Intelligence workflow | **VERIFIED** | `src/engine/runtime/*`, `src/ai/*`, `platz_test_unit`, `platz_test_integration`, `platz_test_security` |
| **Gate 4 (Batch A + Quality Hardening)** | Professional Studio UX Overhaul, Editable Source Round-Trips & CPU Optimization: Programmatic `ImGui::DockBuilder` 5-zone docked workstation layout, crisp TrueType typography (`Segoe UI` + `Consolas`) verified at `100%` & `125%` High-DPI with zero clipped labels, CPU-evaluated non-destructive 3D modifier stack uploaded to Vulkan VMA device buffers (`Subdivide`, `TaperTwist`, `BevelChamfer`, `MirrorX`, `VertexNoiseDisplace`), expandable validated DAG Material Node Graph (`8` node types, dual-input topological Slang codegen, save/reopen per selected object), editable multi-layer 2D Pixel-Art Studio (Undo/Redo + save/reopen without flattening + live per-object `VK_FILTER_NEAREST` Vulkan 3D textures), and sustained `0.340 ms` p50 CPU active frame time optimization | **VERIFIED** | `src/studio/studio_ui.cpp`, `src/creative/texture_generator.cpp`, `src/engine/render/vulkan/vulkan_renderer.cpp`, `ST-07`, `ST-08`, `out/verify_*.png`, `out/verification_receipt.json` |
| **Gate 4 (Batch B)** | Unified Keyframed Animation Timeline Sequencer & Spatial 3D Audio DSP Engine: Multi-track `AnimationComponent` (`position`, `rotationEulerDeg`, `scale`, `emissiveStrength` with `EaseInOut`, `Linear`, `Step` interpolation, non-destructive editor scrub preview, isolated Play execution, Undo/Redo, SQLite persistence, and AI typed command `"object.add_animation_keyframe"`), SDL3 3.4.20 48 kHz Stereo F32 `SpatialAudioEngine` (3D distance attenuation, equal-power stereo panning, one-pole low-pass DSP filter, headless software-DSP fallback, runtime collision/interaction playback, AI command `"object.set_audio_source"`, and `N-15` audio-under-shader-compile-overload test), plus Living Validation World Station `ST-09` (`9/9` passing) | **VERIFIED** | `src/platform/audio_engine.cpp`, `src/engine/runtime/runtime_session.cpp`, `src/studio/studio_ui.cpp`, `ST-09`, `platz_test_integration` (`N-15`), `out/verify_anim_audio_100.png` |

---

## 2. Architecture & Honesty Clarifications

- **3D Mesh Modifier Evaluation:** Non-destructive mesh modifier stacks (`Subdivide`, `TaperTwist`, `BevelChamfer`, `MirrorX`, `VertexNoiseDisplace`) are **evaluated on the CPU** (`creative::MeshGenerator::EvaluateModifierStack`) whenever a modifier stack changes and **uploaded incrementally to Vulkan VMA device-local vertex/index buffers** (`modifiedObjectMeshes_`). Steady-state frames skip modifier re-evaluation and asset scanning in O(1) via `ProjectState::GetMutationGeneration()` (`299/300` fast-path skips during the 300-frame benchmark). GPU-compute mesh deformation is NOT claimed in this build.
- **AI Specialist Evaluation Metrics:** Both `NativeMaterialSpecialistModel` (`12x24x7` MLP) and `NativeGameplayBalanceSpecialistModel` (`10x20x5` MLP) compute **every** reported metric directly from real held-out forward-pass predictions (`finalHoldoutMse`, deterministic mean-target `baselineHoldoutMse`, `mseReductionOverBaselinePct`, `holdoutAccuracyPct`, `holdoutMae`, `holdoutRSquared`) and multi-pass `std::chrono::steady_clock` timed inference (`12,000` / `8,000` timed forward passes). Zero metrics are hardcoded.
- **CPU Frame-Time Profiling:** `FramePerfMetrics` explicitly separates active CPU work (`cpuSceneSyncMs + cpuUiBuildMs + cpuCommandRecordMs + cpuQueueSubmitMs`) from swapchain/VSync blocking (`cpuPresentWaitMs`). Swapchain GPU->Host capture (`vkCmdCopyImageToBuffer`) is strictly opt-in (`RequestSwapchainCapture()`), eliminating ~8 MB/frame copy overhead during normal rendering and benchmarking.

---

## 3. Exact Pinned Toolchain & Dependencies (`dependency-lock.json`)

- **Compiler:** MSVC `19.51.36260.0` (Visual Studio 2026 Community `18.10.3`) with `/std:c++latest` (**C++23**)
- **Build System:** CMake `4.3.1-msvc1` + Ninja `1.13.2`
- **GPU & Driver:** NVIDIA GeForce RTX 5080 (`Vulkan 1.4.351`, Driver `617.42`, Dynamic Rendering, Synchronization2, Timeline Semaphores)
- **Audio Backend:** `SDL3 3.4.20 Hardware AudioStream (48kHz Stereo F32)` (with automatic headless software-DSP fallback)
- **Pinned Libraries (all verified by SHA-256 in `out/deps_cache`):**
  - `Vulkan-Headers` `1.4.363.0`, `volk` `1.4.363.0`, `VulkanMemoryAllocator (VMA)` `3.5.0`, `Vulkan-ValidationLayers` `1.4.363-windows-x64`
  - `SDL3` `3.4.20`, `Slang` `2026.22.1` (`slangc.exe`), `SQLite` `3.54.1` (WAL mode), `Dear ImGui` `1.93.0-docking`
  - `GLM` `1.0.3`, `nlohmann/json` `3.12.0`, `doctest` `2.5.3`, `cgltf` `1.15-20260202`, `tinyobjloader` `2.0.0rc13`, `stb_image` `2.30`, `stb_image_write` `1.16`

---

## 4. Exact Build, Test, Benchmark, and Launch Commands

Run from PowerShell inside `C:\PLATZ-CHALLENGER`:

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

# 4. Run Sustained 300-Frame Unlocked (MAILBOX) CPU/GPU Benchmark & Full-Window Verification Receipt:
C:\PLATZ-CHALLENGER\out\build\release\PLATZ.exe --project C:\PLATZ-CHALLENGER\projects\PLATZ-Validation --benchmark-frames 300 --capture-window C:\PLATZ-CHALLENGER\out\verify_world_100.png
```

- **Verified Release Executable:** `C:\PLATZ-CHALLENGER\out\build\release\PLATZ.exe`
- **Verified Debug Executable:** `C:\PLATZ-CHALLENGER\out\build\debug\PLATZ.exe`
- **Verification Receipt Output:** `C:\PLATZ-CHALLENGER\out\verification_receipt.json`
- **Visually Inspected Full Studio Screenshots (`1920x1040`):**
  - `C:\PLATZ-CHALLENGER\out\verify_world_100.png` (`100%` DPI, World & Asset Browser + Live Textured ST-08 Drone)
  - `C:\PLATZ-CHALLENGER\out\verify_materials_dag_125.png` (`125%` High-DPI, 5-Node Expandable Material DAG loaded from `ST-07`)
  - `C:\PLATZ-CHALLENGER\out\verify_pixel_layers_100.png` (`100%` DPI, 2-Layer Editable Pixel Studio + Live `VK_FILTER_NEAREST` 3D Texture on `ST-08`)
  - `C:\PLATZ-CHALLENGER\out\verify_anim_audio_100.png` (`100%` DPI, Unified Animation Timeline + Spatial Audio Inspector on `ST-09`)
  - `C:\PLATZ-CHALLENGER\out\verify_ai_workbench_100.png` (`100%` DPI, Dual Native PLATZ AI Specialists with Real Held-Out Metrics)

---

## 5. Permanent Living Validation World (`projects/PLATZ-Validation`)

`C:\PLATZ-CHALLENGER\projects\PLATZ-Validation` contains one SQLite WAL database (`project.platzdb`, `r17`), content-addressed SHA-256 blobs (`assets/blobs/sha256/...`), and **9 cumulative interactive test stations (`ST-01` through `ST-09`)**:
1. **`ST-01`** — Spatial Hierarchy, Transform Gizmo Snapping & Duplicate-Name Identity Disambiguation (`N-01`)
2. **`ST-02`** — Slang `2026.22.1` GGX PBR Metallic-Roughness & Studio Specular IBL Lighting Gallery
3. **`ST-03`** — Wavefront `.obj` Mesh Import & Content-Addressed SHA-256 Asset Lineage (`CC0-1.0`)
4. **`ST-04`** — Rigid-Body Gravity, Floor Restitution Bounce, Friction Damping & NaN Guard (`N-10`)
5. **`ST-05`** — No-Code Event/Condition/Action Behavior Rules & Transactional Conserved Inventory (`N-09`)
6. **`ST-06`** — Revision-Pinned Staged PLATZ AI Commands & Local Neural Specialist Workbench (`J-05`, `N-18`, `N-22`, `N-23`)
7. **`ST-07`** — CPU-Evaluated Non-Destructive 3D Modifier Stack (`Subdivide` + `TaperTwist` -> VMA Upload) & 5-Node Expandable DAG Material Graph (`AlbedoColor -> WaveBands -> CheckerPattern -> FresnelRim -> PbrOutput`) -> Slang SPIR-V Hot-Reload
8. **`ST-08`** — Editable 2-Layer 2D Pixel-Art Canvas (`pixel_source` + `PLZT` `VK_FILTER_NEAREST` 3D Vulkan Texture) & Time-Inspectable Patrol Logic Debugger (`N-14`)
9. **`ST-09`** — Unified Multi-Track Keyframed Animation Timeline (`position`, `rotationEulerDeg`, `emissiveStrength` with `EaseInOut`/`Linear` curves) & 3D Spatial Audio Procedural DSP (`CrystalChime`, `N-15`)

---

## 6. Highest-Value Next Prioritized Feature Batch (Gate 4 Batch C)

1. **Interactive Sculpt/Mesh-Edit Brushes & Real UV Unwrap/Procedural Texture Baker (`PLZ-MODEL-002..003`, `PLZ-MODEL-007`):** Direct vertex/face selection & soft-brush sculpting in the 3D viewport with full Undo/Redo and live VMA mesh re-upload.
2. **Shadow-Mapping & Multi-Light Deferred/Clustered Pass (`PLZ-RENDER-003..004`, `PLZ-MAT-009`):** Real directional PCF shadow map pass in Vulkan 1.4 dynamic rendering + configurable multi-point light array (`SceneUniforms` SSBO).
3. **Standalone Playable Export Runner (`PLZ-RUNTIME-005..006`):** Launch exported `.platzpkg.json` bundles directly in a clean standalone Vulkan 1.4 Play viewport mode (`PLATZ.exe --play-bundle <path>`) plus Validation Station `ST-10`.
