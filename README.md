# PLATZ — One Place. Zero Limits. (Independent Challenger Implementation)

**Canonical Repository:** `C:\PLATZ-CHALLENGER`  
**Specification:** `docs/master/PLATZ_MASTER.md`  
**Architecture Decision Record:** `docs/architecture/ADR-0001-foundation.md`  
**AI Continuity Handoff:** `docs/ai/PLATZ_HANDOFF.md`  
**Owner Status Summary:** `docs/ai/PLATZ_OWNER_STATUS.md`  
**Validation Matrix:** `docs/qa/PLATZ_VALIDATION_MATRIX.md`

---

## 1. Architecture & Technology Stack

- **Language & Compiler:** Native C++23 (`MSVC 19.51.36260` / Visual Studio 2026 Community `18.10.3`)
- **Primary Graphics API:** Vulkan 1.4 (`Vulkan-Headers 1.4.363.0`, `volk 1.4.363.0`, Dynamic Rendering, Synchronization2, Timeline Semaphores, GPU Timestamp Profiling)
- **GPU Memory Management:** Vulkan Memory Allocator (`VMA 3.5.0`) wrapped in PLATZ RAII resource handles (`VulkanBuffer`, `VulkanImage`), frame-deferred destruction queues, staging upload/readback buffers, and real-time budget accounting
- **Shader Pipeline:** Slang (`2026.22.1`) compiled to Vulkan SPIR-V 1.5 with binary structural validation, layout verification, and live hot-reload with last-known-good fallback
- **Windowing & Platform:** SDL3 (`3.4.20`) with high-DPI support, keyboard/mouse/gamepad navigation, clipboard, and Vulkan surface creation
- **Studio UI:** PLATZ Studio UI architecture built on SDL3 + Dear ImGui Docking (`1.93.0-docking`) with custom PZ branding, Universal Command Bar (`Ctrl+K`), 3D Viewport & Transform Gizmos with Snapping, Hierarchy, Inspector, Asset Browser, Material PBR Editor, No-Code Behavior Editor, PLATZ AI Staged Session Workbench, Protected Approval/Stop Bar, and Accessibility Announcer
- **Canonical Project Storage:** SQLite (`3.54.1`) in WAL mode (`project.platzdb`) for ACID metadata, stable 64-bit ObjectIdentity, revisions, undo/redo history, and lineage + SHA-256 Content-Addressed Large Asset Store (`assets/blobs/sha256/...`) with atomic staging and crash recovery

---

## 2. Build, Test, and Launch Commands (Windows x64)

### Build Release & Debug Presets

From a Visual Studio 2026 x64 Developer Command Prompt or PowerShell with MSVC environment initialized:

```powershell
# Configure and build Release
cmake --preset release
cmake --build --preset release

# Run all CTest verification suites (Unit, Integration, Vulkan Graphics, Security/Adversarial)
ctest --preset release --output-on-failure
```

### Launch PLATZ Studio (`PLATZ.exe`)

```powershell
# Launch interactive PLATZ Studio opening the permanent living validation world:
C:\PLATZ-CHALLENGER\out\build\release\PLATZ.exe --project C:\PLATZ-CHALLENGER\projects\PLATZ-Validation

# Run automated end-to-end verification and benchmark pass (renders 3D viewport frames on Vulkan 1.4 GPU, runs Gate 1-3 journeys, exports diagnostic receipt):
C:\PLATZ-CHALLENGER\out\build\release\PLATZ.exe --project C:\PLATZ-CHALLENGER\projects\PLATZ-Validation --verify-frames 60
```
