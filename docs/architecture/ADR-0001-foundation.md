# ADR-0001: Foundational Architecture Decision Record (PLATZ Challenger)

- **Status:** Accepted and Binding
- **Date:** 2026-11-21
- **Repository:** `C:\PLATZ-CHALLENGER`
- **Covers Contracts:** `PLZ-FOUND-001`, `PLZ-FOUND-002`, `PLZ-FOUND-013`, `PLZ-FOUND-015`, `PLZ-FOUND-016`, `PLZ-FOUND-017`, `PLZ-FOUND-018`, `PLZ-FOUND-020`, `PLZ-FOUND-022`, `PLZ-FOUND-023`, `PLZ-ASSURE-001`

---

## 1. Context and Objective

PLATZ is a unified, local-first creative workstation combining a genuine 3D/2D game engine, integrated creation Studio, canonical transactional project storage, no-code/code execution runtime, and project-aware PLATZ AI with local specialist model training. This ADR locks the foundational toolchain, dependencies, GPU resource/lifetime model, Studio UI architecture, canonical SQLite + content-addressed blob storage schema, and verification harness.

---

## 2. Verified Host Hardware and Toolchain Baseline

Verified directly on the host machine on **2026-11-21**:

- **Operating System:** Windows 11 x64 (`Windows Kits 10.0.26100.0`)
- **Primary Compiler:** Visual Studio 2026 Community (`18.10.3+12224.181`), MSVC `cl.exe` `19.51.36260` (`14.51.36231`) with native **C++23** (`/std:c++latest` / `CMAKE_CXX_STANDARD 23`). Also verified: bundled LLVM/Clang `22.1.3`.
- **Build System:** Bundled CMake `4.3.1-msvc1` and Ninja `1.13.2` (Note: CMake 4.5 was checked as a candidate; CMake 4.3.1-msvc1 is the newest stable Visual Studio 2026 integrated release installed on the host, avoiding any unauthorized global system modification while supporting all CMake Preset v6 features).
- **Primary GPU (`GPU0`):** **NVIDIA GeForce RTX 5080** (Discrete GPU, `vendorID = 0x10de`, `deviceID = 0x2c02`, Vulkan `apiVersion = 1.4.351`, Driver `617.42`, Conformance `1.4.3.3`).
- **Secondary GPU (`GPU1`):** Intel(R) UHD Graphics 770 (Integrated GPU, Vulkan `apiVersion = 1.4.323`).
- **System Vulkan Loader:** `C:\Windows\System32\vulkan-1.dll` (`1.4.341.0`).

---

## 3. Pinned Dependencies (Verified Official Stable Releases)

All external dependencies are fetched into the isolated build/dependency cache (`C:\PLATZ-CHALLENGER\out\deps_cache`) and recorded with SHA-256 hashes and licenses in `dependency-lock.json`:

| Component | Selected Package & Version | License | Technical Rationale |
| :--- | :--- | :--- | :--- |
| **Vulkan API Headers** | `KhronosGroup/Vulkan-Headers` `vulkan-sdk-1.4.363.0` | Apache-2.0 / MIT | Latest stable Vulkan 1.4 header definitions matching the November 2026 specification baseline. |
| **Vulkan Meta-Loader** | `zeux/volk` `vulkan-sdk-1.4.363.0` | MIT | Dynamically binds Vulkan 1.4 instance and device entry points directly from `vulkan-1.dll` without requiring a machine-wide LunarG SDK installer that could affect the other developer's environment. |
| **Vulkan Memory Allocator** | `VulkanMemoryAllocator (VMA)` `v3.5.0` | MIT | Industry-standard suballocation layer for Vulkan 1.4 (`VMA_VULKAN_VERSION 1004000`); PLATZ wraps VMA with RAII handles, budget tracking, upload/readback rings, and frame-deferred destruction queues. |
| **Shader Compiler** | `shader-slang/slang` `v2026.22.1` (`slangc.exe` + Slang API) | Apache-2.0 with LLVM Exception | Compiles PLATZ `.slang` shader modules directly to Vulkan SPIR-V 1.6 with reflection, layout checks, and SPIR-V validation (`-validate-ir` + binary SPIR-V structure/opcode validator). Windows SDK `dxc.exe` (`10.0.26100.0`) is retained as an emergency fallback. |
| **Platform & Windowing** | `libsdl-org/SDL` `release-3.4.20` (`SDL3`) | Zlib | Provides Win32 windowing, high-DPI display scaling, keyboard/mouse/gamepad/pen input, clipboard, and `SDL_Vulkan_CreateSurface`. |
| **Studio UI Foundation** | `ocornut/imgui` `v1.93.0-docking` + PLATZ Studio UI Layer | MIT | Selected after two-candidate qualification (see Section 5). Rendered directly inside the Vulkan 1.4 swapchain pass with custom PZ theme, docking, keyboard navigation, high-DPI scaling, and accessibility annotations. |
| **Project Database** | `SQLite` `3.54.1` (`sqlite-amalgamation-3540100`) | Public Domain | Single-file ACID relational database engine in WAL mode for canonical object identities, components, revision heads, transactions, undo/redo journals, and asset lineage. |
| **Mathematics** | `g-truc/glm` `1.0.3` | MIT | Vulkan-compatible (`GLM_FORCE_DEPTH_ZERO_TO_ONE`, right-handed world space) 3D vector, matrix, quaternion, and ray intersection math. |
| **JSON & Metadata** | `nlohmann/json` `v3.12.0` | MIT | Structured serialization for component payloads, command diffs, AI training receipts, and `dependency-lock.json` verification. |
| **3D Mesh & Texture IO** | `cgltf` (`85cd6238`), `tinyobjloader` (`bbcf7570`), `stb` (`6b5799ba`) | MIT / Public Domain | Standards-compliant glTF 2.0 (`.gltf`/`.glb`), Wavefront `.obj`, and PNG/JPG/TGA texture import/export into PLATZ's content-addressed asset store. |
| **Unit/Integration Tests** | `doctest` `v2.5.3` | MIT | Fast C++23 test runner integrated with CTest for unit, integration, Vulkan graphics, and security/adversarial test suites. |

---

## 4. Vulkan 1.4 Renderer and GPU Resource Lifetime Model

1. **Instance & Physical Device Selection:**
   - Requests Vulkan API `VK_API_VERSION_1_4`.
   - Filters out incompatible older third-party overlay layers when necessary and supports `VK_EXT_debug_utils` + Khronos Validation Layers (`VK_LAYER_KHRONOS_validation`) when present or via isolated `VK_ADD_LAYER_PATH`.
   - Scores physical devices by type (`VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU` preferred -> selects **NVIDIA GeForce RTX 5080** on this host), verifying Vulkan 1.3/1.4 core features: `synchronization2`, `dynamicRendering`, `timelineSemaphore`, `bufferDeviceAddress`, `samplerAnisotropy`, and graphics/compute queue support.
2. **Memory & Lifetime Management above VMA:**
   - `VulkanBuffer` and `VulkanImage` are move-only C++23 RAII classes backed by `VmaAllocator`.
   - All live GPU resources are tracked by a generation-counted handle and registered with `GpuBudgetTracker` (reporting active bytes, peak bytes, allocation counts, and per-heap VMA budgets).
   - **Deferred Destruction:** When an asset or mesh is modified or deleted while frames are in flight (`MAX_FRAMES_IN_FLIGHT = 2`), its `VkBuffer`/`VkImage`/`VmaAllocation` is enqueued in the frame's `DeferredDeletionQueue` and destroyed only after that frame's fence/timeline semaphore signals completion—never using per-frame `vkDeviceWaitIdle` to mask synchronization hazards.
3. **Shader Pipeline & Hot-Reload:**
   - Shaders are authored in Slang (`shaders/*.slang`) and compiled via `slangc.exe` (`v2026.22.1`) to SPIR-V (`-target spirv -profile spirv_1_5 -O2`).
   - Compiled SPIR-V binaries undergo magic-number, version, word-alignment, bound, and entry-point/opcode structural validation before `vkCreateShaderModule`.
   - If a live shader edit or hot-reload fails compilation or validation, `ShaderManager` retains the **last-known-good** pipeline and surfaces diagnostics to the Studio UI (`N-13`).
4. **3D Scene Rendering & Offscreen Viewport + Picking:**
   - The 3D viewport renders to an offscreen color target (`VK_FORMAT_R8G8B8A8_UNORM`), object-ID picking target (`VK_FORMAT_R32_UINT`), and depth buffer (`VK_FORMAT_D32_SFLOAT`) using Vulkan 1.4 **Dynamic Rendering** (`vkCmdBeginRendering` / `vkCmdEndRendering`) and **Synchronization2** (`vkCmdPipelineBarrier2`).
   - Enables both **interactive Studio viewport display** (sampled as a texture inside the Studio dockspace) AND **headless/CI offscreen rendering with GPU readback verification** (`tests/graphics`), plus exact **3D object picking** (both analytical ray-AABB/triangle intersection and GPU ID-buffer readback).

---

## 5. Studio UI Foundation Qualification (Two Candidates Evaluated)

We evaluated two candidate UI foundations against PLATZ requirements (`PLZ-UX-001..013`, `PLZ-FOUND-022`, `PLZ-FOUND-023`):

### Candidate A: SDL3 (`3.4.20`) + Dear ImGui Docking (`v1.93.0-docking`) + PLATZ Studio Widget/Theme/Accessibility Layer (**SELECTED**)
- **Strengths:**
  - Zero external framework lock-in; renders directly inside PLATZ's own Vulkan 1.4 command buffer without split-HWND swapchain synchronization issues.
  - Built-in docking, split workspaces, persistent layout (`imgui.ini` + project workspace presets), full keyboard navigation (`ImGuiConfigFlags_NavEnableKeyboard`), high-DPI crisp font rasterization, clipboard integration, and immediate binding to canonical `Core::ProjectState`.
  - Allows PLATZ to enforce **protected UI regions** (such as the AI Approval & Emergency Stop bar, `PLZ-SAFEX-006` / `N-24`) that user themes or layout edits cannot hide or spoof.
- **Limitations & Mitigation:**
  - Out-of-the-box Dear ImGui lacks native OS screen-reader accessibility trees and high-level domain widgets. **Mitigation:** We implement a dedicated `platz::studio::AccessibilityAnnouncer` and focus tracking layer (supporting keyboard traversal, high-contrast PZ theme, scalable UI zoom `100%–200%`, and live screen-reader/narration log), plus PLATZ-owned typed property widgets (`NumericTransformWidget`, `MaterialPbrWidget`, `CommandBarWidget`, `AiStagingDiffWidget`).

### Candidate B: Qt 6 / WinUI 3 Outer Shell Hosting an Embedded Child HWND Vulkan Surface (**REJECTED**)
- **Weaknesses:**
  - Requires a multi-gigabyte external framework installation not installed on the clean host.
  - Introduces airspace/compositor latency between the native UI compositor and the Vulkan 1.4 swapchain during resize and docking splits.
  - Violates the requirement to avoid heavy external UI toolkits that obscure engine ownership or require system-wide SDK installations.

---

## 6. Canonical Project Format: SQLite + Content-Addressed Asset Store

Each PLATZ project resides in a self-contained directory (`<ProjectDir>/`):
1. **`project.platzdb` (SQLite 3.54.1 in WAL mode):**
   - `project_meta`: `project_uuid`, `name`, `schema_version`, `head_revision_id`, `created_utc`, `updated_utc`.
   - `revisions`: `revision_id` (monotonic integer), `parent_revision_id`, `revision_hash` (SHA-256 of canonical state), `author`, `summary`, `timestamp_utc`, `state_snapshot_json`.
   - `objects`: `object_id` (stable 64-bit UUID), `name`, `parent_id`, `layer`, `visible`, `locked`, `components_json`.
   - `assets`: `asset_id` (stable 64-bit UUID), `logical_name`, `asset_type` (`mesh`, `texture`, `material`, `shader`, `audio`), `content_sha256`, `byte_size`, `source_rel_path`, `lineage_json`, `license_info`.
2. **`assets/blobs/sha256/<first2>/<full_sha256>.blob` (Content-Addressed Large Asset Store):**
   - All large binary assets (meshes, textures, compiled caches) are first written to `assets/staging/<uuid>.tmp`, flushed to disk, and verified against their SHA-256 digest before atomically moving to `assets/blobs/sha256/...` and committing the SQLite transaction that advances `head_revision_id`.
   - If a crash or interruption occurs between blob staging and SQLite commit (`N-03`), reopening the project recovers the last committed `head_revision_id` cleanly and cleans up orphaned staging files without dangling references.

---

## 7. Reversal Triggers (`PLZ-DEVX-009`)

This ADR will be reopened only if:
1. A target GPU driver exhibits a reproducible conformance bug on Vulkan 1.4 dynamic rendering or VMA 3.5.0 that cannot be isolated via a device capability profile.
2. Slang `v2026.22.1` fails to compile a required shader stage on Windows x64, triggering the documented `dxc.exe` SPIR-V fallback.
3. Complex international text shaping (RTL/harfbuzz) in a future Localization Gate (`PLZ-UX-010`) requires linking `harfbuzz` + `freetype` into the PLATZ Studio text renderer.
