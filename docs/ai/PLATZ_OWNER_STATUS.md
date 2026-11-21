# PLATZ Challenger — Owner Status & Benchmark Summary (`PLATZ_OWNER_STATUS.md`)

- **Product:** PLATZ — One Place. Zero Limits.
- **Repository:** `C:\PLATZ-CHALLENGER` (Strictly isolated from `C:\PLATZ`)
- **Date:** 2026-11-21
- **Current Verified Release Envelope:** **Gates 0, 1, 2, 3, and Gate 4 Batch A (`VERIFIED`)**
- **Verified Executable:** `C:\PLATZ-CHALLENGER\out\build\release\PLATZ.exe`

---

## 1. Executive Summary of Verified Capabilities & Studio UX Overhaul

1. **Professional Studio Workstation UI/UX, DockBuilder Layout & High-DPI Vector Typography (`PLZ-UX-001..013`, `PLZ-UXPLUS-001..008`, `J-04`, `N-24`, `N-29`):**
   - **Coherent 5-Zone Programmatic `ImGui::DockBuilder` Layout:** Eliminates floating/overlapping windows and establishes a balanced, resizable, recoverable workstation:
     - *Top Protected Ribbon:* Brand header (`PZ PLATZ STUDIO`), Menu bar, 5 Workspace Task Mode tabs (`1. World`, `2. 3D Mesh Lab`, `3. Material Graph`, `4. 2D Pixel Art`, `5. Logic & AI`), Play/Pause/Stop transport bar (`F5`), Universal Command Bar trigger (`Ctrl+K`), and the **Protected AI Approval & Emergency Stop Bar** (`PLZ-SAFEX-006` / `N-24`).
     - *Left Dock (`22%` width):* Scene Outliner with quick creation buttons, search filter, one-click jump pills for all 8 Living Validation World stations (`ST-01`..`ST-08`), and unclipped object category badges (`Plane`, `Cylinder`, `Cube`, `Sphere`, `Torus`, `Light`, `MeshAsset`, `ModStack`, `Physics`).
     - *Center Top (`68%` height):* Vulkan 1.4 3D Viewport with manipulation toolbar (`Move [W]`, `Rotate [E]`, `Scale [R]`, `Snap`, camera bookmarks, `Sun` elevation slider), 3D projected wireframe selection bounding box, interactive 3D X/Y/Z transform gizmo handles, floating object coordinate badge, and 3D orientation axis rose.
     - *Right Dock (`26%` width):* Two-column non-clipping Property Inspector (`ImGui::BeginTable` with fixed label column and stretching value column) covering Object Identity, 3D Spatial Transform & Snapping, 3D Geometry & Non-Destructive Modifier Stack, Slang PBR Metallic-Roughness Material, and Rigid-Body Physics.
     - *Center Bottom (`32%` height):* 5 unclipped creation tabs (`Asset Browser`, `Material Node Graph`, `2D Pixel & Sprite Studio`, `No-Code Logic & Debugger`, `PLATZ AI & Specialists`).
   - **Scalable Vector Typography:** Crisp anti-aliased Windows TrueType fonts (`Segoe UI` regular/bold + `Consolas` monospace) verified at `1920x1040` across `100%`, `125%` High-DPI, and `150%` scaling.

2. **Real Vulkan 1.4 Engine, Studio Specular IBL & Non-Destructive 3D Modifier Stack (`PLZ-RENDER-001..002`, `PLZ-MODEL-001`, `PLZ-MODEL-004..005`, `PLZ-MODEL-012`):**
   - Evaluates ordered, non-destructive 3D mesh modifier stacks (`Subdivide`, `TaperTwist`, `BevelChamfer`, `MirrorX`, `VertexNoiseDisplace`) with topology quality validation (`AnalyzeMeshQuality`) and incremental per-object GPU vertex/index buffer updates (`PLZ-ARCH-004`).
   - Upgraded `shaders/pbr_surface.slang` with physically based GGX direct lighting + procedural Studio Environment Specular IBL reflection so high-metallic conductors (`Brushed Titanium`, `Polished Gold Torus`, `Twisted Spire`) render with realistic reflections.

3. **Visual Material Node Graph -> Slang 2026.22.1 Compiler & Live Vulkan Hot-Reload (`PLZ-MAT-001`, `PLZ-MAT-003`, `PLZ-MAT-006`, `PLZ-RENDERX-002`):**
   - Interactive 2x2 visual node graph (`AlbedoColor` -> `CheckerPattern` -> `FresnelRim` -> `PbrOutput`) with bezier spline wires, cycle/broken-link diagnostics (`PLZ-RENDERX-002`), CPU 2D preview texture baker, and direct compilation to `.slang` -> SPIR-V 1.5 with live Vulkan pipeline hot-reload and last-known-good fallback (`N-13`).

4. **Interactive 2D Pixel-Art & Sprite Studio (`PLZ-ART-001`, `PLZ-ART-007..009`, `PLZ-ARCH-005`, `N-14`):**
   - `16x16` indexed-palette pixel-art canvas with Mirror-X symmetry painting, Flood Fill, Tile Grid stamper, razor-sharp `VK_FILTER_NEAREST` GPU preview, and content-addressed asset binding to 3D scene objects.

5. **Time-Inspectable Visual Logic Debugger & Dual Native PLATZ AI Specialists (`PLZ-LOGIC-008`, `PLZ-CREATEX2-004`, `PLZ-AIX-003..006`):**
   - Records causal `RuleDebuggerTimelineEntry` snapshots during isolated Play mode with a Time-Travel Scrubber in the Studio UI.
   - Features **two** locally trained from-scratch neural specialists in native C++23 (`12x24x7` `NativeMaterialSpecialistModel` + `10x20x5` `NativeGameplayBalanceSpecialistModel`, both achieving `100.0%` holdout accuracy in `< 1.2 ms` training time) orchestrated by the **3-Role Coordinated Intelligence Workflow** (*Coordinating Planner* -> *Domain Specialists* -> *Independent Critic & Verifier*).

---

## 2. Measured Hardware Benchmark & Verification Receipt

Measured on **2026-11-21** via `C:\PLATZ-CHALLENGER\out\build\release\PLATZ.exe --project C:\PLATZ-CHALLENGER\projects\PLATZ-Validation --verify-frames 35` (`out/verification_receipt.json`):

| Metric | Measured Result | Conditions |
| :--- | :--- | :--- |
| **GPU & Driver** | **NVIDIA GeForce RTX 5080** (`Vulkan 1.4.351`, Driver `617.42`) | Discrete GPU, Dynamic Rendering + Synchronization2 |
| **Window / Viewport Resolution** | `1920x1040` Studio Swapchain / `1280x720` Offscreen 3D Viewport | Verified at both `100%` and `125%` High-DPI scaling |
| **Khronos Validation Layers** | **ACTIVE (`0` Errors, `0` Warnings)** | `VK_LAYER_KHRONOS_validation` (`1.4.363`) |
| **Scene Complexity** | `16` Canonical 3D Objects, `17` Draw Calls, `8,422` Triangles | `projects/PLATZ-Validation` (All 8 stations `ST-01..ST-08` active) |
| **GPU 3D Scene Pass Time** | **`0.0215 ms`** (`21.47 us` via `VK_QUERY_TYPE_TIMESTAMP`) | Steady state, Release x64 |
| **CPU Total Frame Time** | **`3.77 – 4.12 ms`** (~245 FPS unlocked; VSync FIFO swapchain) | Offscreen 3D Pass + Docked Studio UI + Swapchain Capture & Present |
| **VMA GPU Memory Usage** | **`17.78 MB` Active** (`18,640,896` B) / **`18.14 MB` Peak** | Includes 3D meshes, modifier stack caches, textures & swapchain readback buffer |
| **Native Specialist #1 (Material)** | **`1.11 ms` Train / `0.12 us` Inference** (`12x24x7` MLP) | Holdout MSE `0.14290 -> 0.00019`, Accuracy **`100.0%`** (`12` holdout samples) |
| **Native Specialist #2 (Gameplay)** | **`0.78 ms` Train / `0.09 us` Inference** (`10x20x5` MLP) | Holdout MSE `0.07489 -> 0.00062`, Accuracy **`100.0%`** (`8` holdout samples) |
| **CTest Verification Suites** | **`4 / 4` Suites Passed (`100%`)** in both `Release` and `Debug` | `platz_test_unit`, `platz_test_integration`, `platz_test_graphics`, `platz_test_security` |
| **Living Validation World** | **`8 / 8` Stations Passed (`ST-01` through `ST-08`)** | Automated verification route receipt in `out/verification_receipt.json` |
