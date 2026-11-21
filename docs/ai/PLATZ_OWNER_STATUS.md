# PLATZ Challenger — Owner Status & Benchmark Summary (`PLATZ_OWNER_STATUS.md`)

- **Product:** PLATZ — One Place. Zero Limits.
- **Repository:** `C:\PLATZ-CHALLENGER` (Strictly isolated from `C:\PLATZ`)
- **Date:** 2026-11-21
- **Current Verified Release Envelope:** **Gates 0, 1, 2, and 3 (`VERIFIED`)**
- **Verified Executable:** `C:\PLATZ-CHALLENGER\out\build\release\PLATZ.exe`

---

## 1. Executive Summary of Verified Capabilities

1. **Real Vulkan 1.4 Engine & VMA 3.5.0 GPU Ownership (`PLZ-RENDER-001..002`, `PLZ-FOUND-015..017`):**
   - Negotiates Vulkan `1.4.351` directly on the host **NVIDIA GeForce RTX 5080** via `volk` `1.4.363.0` and `Vulkan-Headers` `1.4.363.0`.
   - Uses Vulkan 1.4 **Dynamic Rendering** (`vkCmdBeginRendering`) and **Synchronization2** (`vkCmdPipelineBarrier2`) to render an offscreen 3D viewport color target (`VK_FORMAT_R8G8B8A8_UNORM`), a hardware object-ID picking attachment (`VK_FORMAT_R32_UINT`), and a 32-bit float depth buffer (`VK_FORMAT_D32_SFLOAT`), composited into the SDL3 (`3.4.20`) Studio window swapchain.
   - All GPU buffers and images are managed through RAII handles over **VMA 3.5.0** with generation handles, frame-deferred destruction queues (`kMaxFramesInFlight = 2`), real-time VRAM budget tracking, and device-loss/contention recovery (`N-11`, `N-12`).
   - Shaders are authored in **Slang** (`shaders/pbr_surface.slang`, `shaders/grid_sky.slang`), compiled to SPIR-V 1.5 via **`slangc.exe` `v2026.22.1`**, verified by `SpirvValidator`, and support live hot-reload with automatic last-known-good pipeline fallback on syntax or validation errors (`N-13`).

2. **Canonical SQLite 3.54.1 + Content-Addressed Asset Persistence (`PLZ-CORE-001..005`, `PLZ-ARCH-001..004`):**
   - Structured project metadata, stable 64-bit `ObjectId`s, parent-child hierarchy transforms, revision history, and separate runtime gameplay checkpoints reside in `<ProjectDir>/project.platzdb` (**SQLite 3.54.1** in WAL mode).
   - Binary 3D meshes and textures are staged to `assets/staging/*.tmp`, SHA-256 verified on disk, and atomically promoted to `assets/blobs/sha256/<first2>/<sha256>.blob` before advancing `head_revision_id`.
   - Tested against simulated crashes mid-save (`N-03`), stale revision writes (`N-02`), duplicate object names (`N-01`), corrupt GPU caches (`N-04`), asset dependency invalidation (`N-05`), and incompatible future schema versions (`N-26`).

3. **Professional PLATZ Studio UI/UX & Accessibility (`PLZ-UX-001..013`, `PLZ-UXPLUS-001..008`, `N-24`, `N-29`):**
   - Integrated dockable Studio built on SDL3 + Dear ImGui Docking (`1.93.0-docking`) with PZ Obsidian Dark and High-Contrast Accessible themes, scalable high-DPI typography (`100%–200%`), keyboard navigation, Universal Command Bar (`Ctrl+K`), 5 adaptive task workspaces, 3D viewport gizmo with snapping, Hierarchy, Inspector, PBR Material Editor, Asset Browser, No-Code Logic & HUD panel, PLATZ AI Staged Diff Workbench, and `AccessibilityAnnouncer` screen-reader log.
   - Enforces **Protected Security Controls (`PLZ-SAFEX-006`, `N-24`)**: custom or malicious themes cannot hide or reduce the opacity/contrast of the AI Approval and Emergency Stop controls.

4. **Isolated Play Runtime, No-Code Logic & Conserved Gameplay Resources (`PLZ-RUNTIME-001..002`, `PLZ-LOGIC-001..008`, `PLZ-ARCH-009`):**
   - Pressing Play (`F5`) clones canonical authoring state into an isolated runtime simulation (fixed-substep rigid-body gravity, bounce, friction, and `OnStart`/`OnTick`/`OnProximity`/`OnInteract` no-code rules).
   - Stopping Play restores canonical authoring state cleanly (`N-07`), while gameplay checkpoints save and resume from `runtime_saves` (`N-08`).
   - Conserved inventory transfers (`energy_crystal`) execute atomically and roll back cleanly on mid-transfer interruption (`N-09`).

5. **PLATZ AI Command Registry, Staged Sessions & Original Local Specialist Model (`PLZ-AI-001..015`, `PLZ-AIX-001..012`, `PLZ-SAFEX-001..011`):**
   - All AI edits pass through the versioned `CommandRegistry` and revision-pinned `StagedAiSession` with before/after diffs, proportional logic/material validators (`PLZ-SAFEX-008`), selective approval, and scoped batch rollback after manual edits (`N-25`).
   - **Emergency Stop (`N-18`)** immediately revokes session tokens and blocks late async commits.
   - **Poisoned Retrieval Defense (`N-19`)** blocks prompt-injection privilege escalation inside retrieved text.
   - **Original Local Specialist Model (`NativeMaterialSpecialistModel`):** A genuine, locally trained `12x24x7` neural network in native C++23 trained on rights-cleared PLATZ material/lighting data with an immutable hash-locked holdout evaluation split, privacy/consent enforcement (`N-22`), anti-self-approval gate (`N-23`), and negative result memory (`PLZ-AIX-012`).

---

## 2. Measured Hardware Benchmark & Verification Receipt

Measured on **2026-11-21** via `C:\PLATZ-CHALLENGER\out\build\release\PLATZ.exe --project C:\PLATZ-CHALLENGER\projects\PLATZ-Validation --verify-frames 60` (`out/verification_receipt.json`):

| Metric | Measured Result | Conditions |
| :--- | :--- | :--- |
| **GPU & Driver** | **NVIDIA GeForce RTX 5080** (`Vulkan 1.4.351`, Driver `617.42`) | Discrete GPU, Dynamic Rendering + Synchronization2 |
| **Window / Viewport Resolution** | `1600x920` Studio Swapchain / `1280x720` Offscreen 3D Viewport | `R8G8B8A8_UNORM` + `R32_UINT` Picking + `D32_SFLOAT` Depth |
| **Khronos Validation Layers** | **ACTIVE (`0` Errors, `0` Warnings)** | `VK_LAYER_KHRONOS_validation` (`1.4.363`) |
| **Scene Complexity** | `14` Canonical 3D Objects, `15` Draw Calls, `8,362` Triangles | `projects/PLATZ-Validation` (All 6 stations active) |
| **GPU 3D Scene Pass Time** | **`0.0190 ms`** (`18.98 us` via `VK_QUERY_TYPE_TIMESTAMP`) | 60-frame steady state, Release x64 |
| **CPU Total Frame Time** | **`4.20 ms`** (~238 FPS unlocked; VSync FIFO swapchain) | Includes Offscreen 3D Pass + ImGui Docking UI + Swapchain Present |
| **VMA GPU Memory Usage** | **`10.89 MB` Active** (`11,420,672` B) / **`14.41 MB` Peak** | All meshes, PBR textures, offscreen targets, and UBO rings |
| **Native Specialist Training** | **`1.11 ms`** (`160` epochs, `30` train / `12` holdout samples) | Holdout MSE `0.14290 -> 0.00019`, Accuracy **`100.0%`**, Inference **`0.13 us`** |
| **CTest Verification Suites** | **`4 / 4` Suites Passed (`100%`)** in both `Release` and `Debug` | `platz_test_unit`, `platz_test_integration`, `platz_test_graphics`, `platz_test_security` |
| **Living Validation World** | **`6 / 6` Stations Passed (`ST-01` through `ST-06`)** | Automated verification route receipt in `out/verification_receipt.json` |
