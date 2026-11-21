# PLATZ Challenger — Owner Status & Benchmark Summary (`PLATZ_OWNER_STATUS.md`)

- **Product:** PLATZ — One Place. Zero Limits.
- **Repository:** `C:\PLATZ-CHALLENGER` (Strictly isolated from `C:\PLATZ`)
- **Date:** 2026-11-21
- **Current Verified Release Envelope:** **Gates 0, 1, 2, 3, and Gate 4 Batches A & B (`VERIFIED`)**
- **Verified Executable:** `C:\PLATZ-CHALLENGER\out\build\release\PLATZ.exe`

---

## 1. Executive Summary of Verified Progress (Quality Hardening + Gate 4 Batch B)

1. **Complete Save / Reopen / Edit / Undo / Redo Workflows for Professional Creation Tools:**
   - **Expandable Validated DAG Material Node Graph (`PLZ-MAT-001`, `PLZ-RENDERX-002`):** Loads each selected object's saved `materialGraphJson` automatically on project open, object selection, and Undo/Redo. Supports 8 node types (`AlbedoColor`, `CheckerPattern`, `WaveBands`, `HeightGradient`, `NoisePattern`, `BlendMix`, `FresnelRim`, `PbrOutput`), arbitrary node insertion/removal, dual-input wiring (`Input A` / `Input B`) with DFS cycle prevention & automatic rollback, live topological DAG compilation to Slang `2026.22.1` -> SPIR-V 1.5 hot-reload, and CPU preview baking.
   - **Multi-Layer Editable 2D Pixel-Art Studio + Live 3D Vulkan Textures (`PLZ-ART-001`, `PLZ-ART-007`, `N-14`):** Multi-layer indexed-palette canvases (`PixelArtCanvas` with layer stack, visibility toggles, Brush/Eraser/FloodFill/Mirror-X) persist their unflattened editable JSON source (`pixel_source` asset + inline `pixelCanvasJson`) alongside derived content-addressed `PLZT` textures bound live onto 3D objects in Vulkan 1.4 using `VK_FILTER_NEAREST`. Every stroke and layer edit participates atomically in Undo (`Ctrl+Z`), Redo (`Ctrl+Y`), and Save/Close/Reopen without flattening.

2. **Honest Engineering Architecture & Real Held-Out AI Specialist Evaluation:**
   - **CPU vs. GPU Clarity:** Non-destructive 3D mesh modifiers (`Subdivide`, `TaperTwist`, `BevelChamfer`, `MirrorX`, `VertexNoiseDisplace`) are explicitly documented and labeled in the UI as **CPU-evaluated non-destructive modifiers uploaded to Vulkan VMA device buffers** on change.
   - **Zero Hardcoded AI Metrics:** Both native C++23 specialists (`NativeMaterialSpecialistModel` `12x24x7` MLP and `NativeGameplayBalanceSpecialistModel` `10x20x5` MLP) calculate all metrics (`finalHoldoutMse`, deterministic mean-target `baselineHoldoutMse`, `mseReductionOverBaselinePct`, `holdoutAccuracyPct`, `holdoutMae`, `holdoutRSquared`, and `12,000`/`8,000`-pass `std::chrono::steady_clock` timed inference latency) directly from real held-out forward-pass predictions, and both enforce `N-22` privacy/consent rejection and `N-23` anti-self-approval.

3. **Real Sustained CPU Frame-Time Investigation & 10x Optimization:**
   - **Root Causes Measured & Fixed:**
     1. Eliminated unconditional `~8 MB/frame` (`1920x1040` RGBA8) swapchain `vkCmdCopyImageToBuffer` readback on normal frames (now strictly opt-in via `RequestSwapchainCapture()`).
     2. Added `ProjectState::GetMutationGeneration()` so `VulkanRenderer::EnsureProjectGpuResourcesSynced()` skips unchanged asset/modifier scans in **O(1)** (`0.0001 ms` p50; `299/300` fast-path skips).
     3. Added `themeDirty_` caching to `StudioUiController::ApplyThemeToImGui()` to avoid re-building ImGui style tables every frame.
     4. Separated active CPU work (`Sync + UI + Command Record + Submit`) from swapchain/VSync waiting (`PresentWait`) and added unlocked `VK_PRESENT_MODE_MAILBOX_KHR` sustained benchmarking (`--benchmark-frames 300`).
   - **Result:** Sustained active CPU frame work dropped from **`~3.8–4.5 ms`** down to **`0.340 ms` p50 (`0.350 ms` mean, `0.392 ms` p95, `0.435 ms` p99)** with Khronos Validation Layers active!

4. **Gate 4 Batch B Integrated Creative Expansion (`PLZ-MEDIA-001..002`, `PLZ-MEDIA-008..009`, `N-15`):**
   - **Unified Keyframed Animation Timeline Sequencer:** Multi-track `AnimationComponent` (`position`, `rotationEulerDeg`, `scale`, `emissiveStrength` with `EaseInOut`, `Linear`, `Step` curves), non-destructive editor timeline scrub preview, isolated Play execution, Undo/Redo, SQLite persistence, visual diamond-marker timeline panel, and AI command `"object.add_animation_keyframe"`.
   - **Spatial 3D Audio & Procedural DSP Engine:** Real `SDL3 3.4.20 Hardware AudioStream (48kHz Stereo F32)` (with automatic headless software-DSP fallback), 3D distance attenuation, equal-power stereo panning, one-pole low-pass filter, Inspector Section 6 controls + live cue auditioning, runtime collision/interaction playback, AI command `"object.set_audio_source"`, and `N-15` overload resilience verification under concurrent Slang shader compilation.

---

## 2. Measured Hardware Benchmark & Verification Receipt

Measured on **2026-11-21** via `C:\PLATZ-CHALLENGER\out\build\release\PLATZ.exe --project C:\PLATZ-CHALLENGER\projects\PLATZ-Validation --benchmark-frames 300 --capture-window C:\PLATZ-CHALLENGER\out\verify_world_100.png` (`out/verification_receipt.json`):

| Metric | Measured Result | Conditions |
| :--- | :--- | :--- |
| **GPU & Driver** | **NVIDIA GeForce RTX 5080** (`Vulkan 1.4.351`, Driver `617.42`) | Discrete GPU, Dynamic Rendering + Synchronization2, `MAILBOX` present mode |
| **Audio Hardware Backend** | **SDL3 3.4.20 Hardware AudioStream (`48 kHz` Stereo F32)** | `0` measured underruns (`N-15` verified concurrently with Slang compilation) |
| **Window / Viewport Resolution** | `1920x1040` Studio Swapchain / `1280x720` Offscreen 3D Viewport | Visually verified at both `100%` and `125%` High-DPI scaling (zero label clipping) |
| **Khronos Validation Layers** | **ACTIVE (`0` Errors, `0` Warnings across all 300 frames)** | `VK_LAYER_KHRONOS_validation` (`1.4.363`) |
| **Scene Complexity** | `17` Canonical Objects, `18` Draw Calls, `9,446` Triangles | `projects/PLATZ-Validation` (`r17`, all 9 stations `ST-01..ST-09` active) |
| **Sustained CPU Active Work (`289` samples)** | **`0.350 ms` mean \| `0.340 ms` p50 \| `0.392 ms` p95 \| `0.435 ms` p99** | Excludes present wait (`10` warmup frames excluded); `299/300` O(1) GPU sync skips |
| **CPU Stage Breakdown (`p50`)** | **Sync `0.0001 ms` \| UI `0.129 ms` \| Record `0.176 ms` \| Submit `0.037 ms`** | PresentWait (`MAILBOX`): `0.084 ms` p50 |
| **Sustained GPU 3D Scene Pass (`289` samples)** | **`0.0223 ms` mean \| `0.0217 ms` p50 \| `0.0222 ms` p95** | Hardware `VK_QUERY_TYPE_TIMESTAMP` deltas |
| **VMA GPU Memory Usage** | **`18.64 MB` Active** (`19,543,712` B) / **`22.15 MB` Peak** | Includes 3D meshes, CPU-evaluated modifier uploads, `PLZT` pixel textures & UI targets |
| **Native Specialist #1 (Material `12x24x7`)** | **Holdout MSE `0.00019` vs Baseline `0.08732` (`-99.78%`)** | Measured on `12` held-out samples: Acc **`100.0%`**, MAE **`0.0103`**, R² **`0.9978`**, Latency **`0.092 us`** (`12,000` timed passes) |
| **Native Specialist #2 (Gameplay `10x20x5`)** | **Holdout MSE `0.00055` vs Baseline `0.03989` (`-98.62%`)** | Measured on `8` held-out samples: Acc **`100.0%`**, MAE **`0.0183`**, R² **`0.9857`**, Latency **`0.062 us`** (`8,000` timed passes) |
| **CTest Verification Suites** | **`4 / 4` Suites Passed (`100%`)** in both `Release` (`1.80s`) & `Debug` (`2.68s`) | `platz_test_unit`, `platz_test_integration`, `platz_test_graphics`, `platz_test_security` |
| **Living Validation World** | **`9 / 9` Stations Passed (`ST-01` through `ST-09`)** | Automated verification route receipt in `out/verification_receipt.json` |
