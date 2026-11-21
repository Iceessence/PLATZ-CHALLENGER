#include "ai/command_registry.h"
#include "ai/specialist_model.h"
#include "ai/staged_session.h"
#include "core/project_state.h"
#include "creative/texture_generator.h"
#include "engine/render/vulkan/vulkan_renderer.h"
#include "engine/runtime/runtime_session.h"
#include "platform/audio_engine.h"
#include "platform/platform_paths.h"
#include "platform/sdl_window.h"
#include "studio/studio_ui.h"
#include "studio/validation_world.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

namespace {

struct SampleStats {
    double mean{0.0};
    double p50{0.0};
    double p95{0.0};
    double p99{0.0};
    double minVal{0.0};
    double maxVal{0.0};
};

SampleStats ComputeSampleStats(std::vector<double> values) {
    SampleStats st{};
    if (values.empty()) return st;
    std::sort(values.begin(), values.end());
    double sum = std::accumulate(values.begin(), values.end(), 0.0);
    st.mean = sum / static_cast<double>(values.size());
    st.minVal = values.front();
    st.maxVal = values.back();
    auto pct = [&](double p) {
        size_t idx = static_cast<size_t>(std::clamp(p * static_cast<double>(values.size() - 1), 0.0, static_cast<double>(values.size() - 1)));
        return values[idx];
    };
    st.p50 = pct(0.50);
    st.p95 = pct(0.95);
    st.p99 = pct(0.99);
    return st;
}

nlohmann::json StatsToJson(const SampleStats& s) {
    return {
        {"meanMs", s.mean},
        {"p50Ms", s.p50},
        {"p95Ms", s.p95},
        {"p99Ms", s.p99},
        {"minMs", s.minVal},
        {"maxMs", s.maxVal}
    };
}

} // namespace

int main(int argc, char* argv[]) {
    std::filesystem::path projectDir = platz::platform::GetRepoRootDir() / "projects" / "PLATZ-Validation";
    bool headless = false;
    int verifyFrames = 0;
    int benchmarkFrames = 0;
    bool disableVsync = false;
    float initialUiScale = 1.0f;
    std::string workspaceArg;
    std::string selectObjectArg;
    std::string windowCapturePathArg;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--project" && i + 1 < argc) {
            projectDir = argv[++i];
        } else if (arg == "--headless") {
            headless = true;
        } else if (arg == "--verify-frames" && i + 1 < argc) {
            verifyFrames = std::max(1, std::atoi(argv[++i]));
        } else if (arg == "--benchmark-frames" && i + 1 < argc) {
            benchmarkFrames = std::max(20, std::atoi(argv[++i]));
            disableVsync = true;
        } else if (arg == "--no-vsync") {
            disableVsync = true;
        } else if (arg == "--ui-scale" && i + 1 < argc) {
            initialUiScale = static_cast<float>(std::atof(argv[++i]));
        } else if (arg == "--workspace" && i + 1 < argc) {
            workspaceArg = argv[++i];
        } else if (arg == "--select" && i + 1 < argc) {
            selectObjectArg = argv[++i];
        } else if (arg == "--capture-window" && i + 1 < argc) {
            windowCapturePathArg = argv[++i];
        }
    }

    int targetAutoFrames = std::max(verifyFrames, benchmarkFrames);

    std::cout << "[PLATZ] Starting PLATZ Studio (One Place. Zero Limits.)\n";
    std::cout << "[PLATZ] Canonical Project Path: " << projectDir.string() << "\n";

    // 1. Open or populate the permanent living validation world (projects/PLATZ-Validation)
    platz::core::ProjectState project;
    std::string err;
    if (!platz::studio::ValidationWorldManager::EnsureValidationWorldPopulated(project, projectDir, &err)) {
        std::cerr << "[PLATZ ERROR] Failed initializing project at " << projectDir.string() << ": " << err << "\n";
        return 1;
    }
    std::cout << "[PLATZ] Loaded project '" << project.GetProjectName()
              << "' at revision r" << project.GetHeadRevision()
              << " (" << project.GetObjects().size() << " canonical objects, "
              << project.GetAssets().size() << " content-addressed assets).\n";

    // 2. Initialize SDL3 Platform Window & Vulkan 1.4 Renderer at realistic 1920x1040 desktop resolution
    platz::platform::WindowConfig winCfg{};
    winCfg.title = "PLATZ Studio — " + project.GetProjectName() + " [Vulkan 1.4]";
    winCfg.width = 1920;
    winCfg.height = 1040;
    winCfg.headless = headless;

    platz::platform::SdlWindow window;
    if (!window.Initialize(winCfg, &err)) {
        std::cout << "[PLATZ] Window creation note (" << err << "); continuing in headless Vulkan 1.4 mode.\n";
        winCfg.headless = true;
        window.Initialize(winCfg, &err);
    }

    platz::engine::vulkan::VulkanRenderer renderer;
    renderer.SetPreferVsync(!disableVsync);
    if (!renderer.Initialize(&window, 1280, 720, true, &err)) {
        std::cerr << "[PLATZ ERROR] Vulkan 1.4 Renderer initialization failed: " << err << "\n";
        return 1;
    }

    const auto& caps = renderer.GetContext().GetCapabilities();
    std::cout << "[PLATZ] Active GPU: " << caps.deviceName
              << " (Vulkan " << renderer.GetContext().GetFormattedApiVersion()
              << ", PresentMode=" << renderer.GetPerfMetrics().presentModeName
              << ", DynamicRendering=" << (caps.supportsDynamicRendering ? "YES" : "NO")
              << ", Sync2=" << (caps.supportsSynchronization2 ? "YES" : "NO")
              << ", Validation=" << (renderer.GetContext().IsValidationActive() ? "ACTIVE" : "OFF") << ")\n";

    // 3. Initialize Spatial Audio DSP Engine (SDL3 3.4.20 AudioStream or Headless Software DSP)
    platz::platform::SpatialAudioEngine audioEngine;
    std::string audioErr;
    audioEngine.Initialize(window.IsHeadless(), &audioErr);
    std::cout << "[PLATZ] Audio Engine: " << audioEngine.GetMetrics().backendStatus
              << (audioErr.empty() ? "" : (" (" + audioErr + ")")) << "\n";

    // 4. Initialize Runtime, CommandRegistry, StagedAiSession, SpecialistModel, and StudioUiController
    platz::engine::runtime::RuntimeSession runtime;
    runtime.AttachAudioEngine(&audioEngine);
    platz::ai::CommandRegistry commandRegistry;
    platz::ai::StagedAiSession stagedAiSession(commandRegistry);
    platz::ai::NativeMaterialSpecialistModel specialistModel;

    platz::studio::StudioUiController studioUi(
        project,
        renderer,
        runtime,
        commandRegistry,
        stagedAiSession,
        specialistModel);
    studioUi.AttachAudioEngine(&audioEngine);

    if (initialUiScale != 1.0f) {
        studioUi.SetUiScale(initialUiScale);
    }
    if (!selectObjectArg.empty()) {
        auto matches = project.FindObjectsByName(selectObjectArg);
        if (!matches.empty()) {
            studioUi.SetSelectedObjectId(matches.front());
            if (const auto* sel = project.FindObject(matches.front())) {
                studioUi.GetCamera().FocusOnPoint(sel->transform.position, 5.4f);
            }
        }
    }
    if (workspaceArg == "materials") {
        studioUi.SetWorkspaceMode(platz::studio::WorkspaceMode::MaterialsAndShaders);
    } else if (workspaceArg == "pixel") {
        studioUi.SetWorkspaceMode(platz::studio::WorkspaceMode::PixelArtAndSprites);
    } else if (workspaceArg == "animation") {
        studioUi.SetWorkspaceMode(platz::studio::WorkspaceMode::AnimationAndAudio);
    } else if (workspaceArg == "ai") {
        studioUi.SetWorkspaceMode(platz::studio::WorkspaceMode::PlatzAiWorkbench);
    } else if (workspaceArg == "model") {
        studioUi.SetWorkspaceMode(platz::studio::WorkspaceMode::ModelAndAssets);
    }

    std::vector<double> samplesCpuFrameMs;
    std::vector<double> samplesCpuActiveMs;
    std::vector<double> samplesCpuSyncMs;
    std::vector<double> samplesCpuUiMs;
    std::vector<double> samplesCpuRecordMs;
    std::vector<double> samplesCpuSubmitMs;
    std::vector<double> samplesCpuPresentWaitMs;
    std::vector<double> samplesGpuPassMs;
    if (benchmarkFrames > 0) {
        samplesCpuFrameMs.reserve(static_cast<size_t>(benchmarkFrames));
        samplesCpuActiveMs.reserve(static_cast<size_t>(benchmarkFrames));
        samplesCpuSyncMs.reserve(static_cast<size_t>(benchmarkFrames));
        samplesCpuUiMs.reserve(static_cast<size_t>(benchmarkFrames));
        samplesCpuRecordMs.reserve(static_cast<size_t>(benchmarkFrames));
        samplesCpuSubmitMs.reserve(static_cast<size_t>(benchmarkFrames));
        samplesCpuPresentWaitMs.reserve(static_cast<size_t>(benchmarkFrames));
        samplesGpuPassMs.reserve(static_cast<size_t>(benchmarkFrames));
    }

    auto lastTick = std::chrono::steady_clock::now();
    bool running = true;
    int framesRendered = 0;

    while (running) {
        if (!window.IsHeadless()) {
            SDL_Event ev{};
            while (SDL_PollEvent(&ev)) {
                if (ImGui::GetCurrentContext()) {
                    ImGui_ImplSDL3_ProcessEvent(&ev);
                }
                if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                    running = false;
                }
            }
        }

        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - lastTick).count();
        lastTick = now;

        // Keep spatial audio listener synced with active 3D camera
        glm::vec3 camEye = studioUi.GetCamera().GetEyePosition();
        glm::mat4 camView = studioUi.GetCamera().GetViewMatrix();
        glm::vec3 camRight(camView[0][0], camView[1][0], camView[2][0]);
        runtime.SetAudioListener(camEye, camRight);

        if (runtime.IsPlaying()) {
            runtime.StepSimulation(dt);
        } else {
            studioUi.AdvanceEditorPreview(dt);
        }

        // Only capture the swapchain framebuffer on the final verification frame (zero transfer cost on regular/benchmark frames!)
        if (targetAutoFrames > 0 && framesRendered == targetAutoFrames - 1 && !window.IsHeadless()) {
            renderer.RequestSwapchainCapture();
        }

        bool renderOk = false;
        if (window.IsHeadless()) {
            renderOk = renderer.RenderSceneOffscreen(
                project,
                studioUi.GetActiveSceneObjects(),
                studioUi.GetCamera(),
                studioUi.GetSelectedObjectId(),
                &err);
        } else {
            renderOk = renderer.RenderInteractiveFrame(
                project,
                studioUi.GetActiveSceneObjects(),
                studioUi.GetCamera(),
                studioUi.GetSelectedObjectId(),
                [&]() { studioUi.BuildImGuiStudioFrame(); },
                &err);
        }

        if (!renderOk) {
            std::cerr << "[PLATZ ERROR] Frame render error: " << err << "\n";
            return 1;
        }

        // Record sustained benchmark samples after a 10-frame pipeline/swapchain warmup
        if (benchmarkFrames > 0 && framesRendered >= 10 && framesRendered < targetAutoFrames - 1) {
            const auto& pm = renderer.GetPerfMetrics();
            samplesCpuFrameMs.push_back(pm.cpuFrameTimeMs);
            samplesCpuActiveMs.push_back(pm.cpuActiveWorkMs);
            samplesCpuSyncMs.push_back(pm.cpuSceneSyncMs);
            samplesCpuUiMs.push_back(pm.cpuUiBuildMs);
            samplesCpuRecordMs.push_back(pm.cpuCommandRecordMs);
            samplesCpuSubmitMs.push_back(pm.cpuQueueSubmitMs);
            samplesCpuPresentWaitMs.push_back(pm.cpuPresentWaitMs);
            samplesGpuPassMs.push_back(pm.gpuScenePassTimeMs);
        }

        ++framesRendered;
        if (targetAutoFrames > 0 && framesRendered >= targetAutoFrames) {
            running = false;
        }
    }

    if (targetAutoFrames > 0) {
        auto routeReceipt = platz::studio::ValidationWorldManager::RunAutomatedVerificationRoute(project);

        // 1. Capture raw 3D offscreen viewport image
        platz::creative::ImageData2D frameCapture;
        bool readbackOk = renderer.ReadbackViewportColorRgba(&frameCapture, &err);
        std::filesystem::path capturePath = platz::platform::GetRepoRootDir() / "out" / "verification_viewport_capture.png";
        if (readbackOk) {
            platz::creative::TextureGenerator::WriteImageToPng(frameCapture, capturePath);
        }

        // 2. Capture full composed Studio application window swapchain image
        platz::creative::ImageData2D windowCapture;
        bool winReadbackOk = renderer.ReadbackSwapchainWindowRgba(&windowCapture, &err);
        std::filesystem::path winCapturePath = windowCapturePathArg.empty()
            ? (platz::platform::GetRepoRootDir() / "out" / "verification_studio_window.png")
            : std::filesystem::path(windowCapturePathArg);
        if (winReadbackOk) {
            platz::creative::TextureGenerator::WriteImageToPng(windowCapture, winCapturePath);
        }

        const auto& perf = renderer.GetPerfMetrics();
        auto budget = renderer.GetContext().GetMemoryBudgetSnapshot();
        auto audioMetrics = audioEngine.GetMetrics();

        nlohmann::json receipt = {
            {"product", "PLATZ"},
            {"executablePath", (platz::platform::GetExecutableDir() / "PLATZ.exe").string()},
            {"projectDir", project.GetProjectDir().string()},
            {"headRevision", project.GetHeadRevision()},
            {"gpuDeviceName", caps.deviceName},
            {"vulkanApiVersion", renderer.GetContext().GetFormattedApiVersion()},
            {"presentMode", perf.presentModeName},
            {"validationLayerActive", renderer.GetContext().IsValidationActive()},
            {"validationErrorCount", renderer.GetContext().GetValidationErrorCount()},
            {"framesRendered", framesRendered},
            {"windowResolution", {windowCapture.width, windowCapture.height}},
            {"viewportResolution", {renderer.GetViewportWidth(), renderer.GetViewportHeight()}},
            {"uiDpiScaleFactor", studioUi.GetUiScale()},
            {"modifierEvaluationArchitecture", "CPU-evaluated non-destructive modifier stack uploaded to Vulkan VMA device buffers"},
            {"lastCpuFrameTimeMs", perf.cpuFrameTimeMs},
            {"lastCpuActiveWorkMs", perf.cpuActiveWorkMs},
            {"lastCpuSceneSyncMs", perf.cpuSceneSyncMs},
            {"lastCpuUiBuildMs", perf.cpuUiBuildMs},
            {"lastCpuCommandRecordMs", perf.cpuCommandRecordMs},
            {"lastCpuQueueSubmitMs", perf.cpuQueueSubmitMs},
            {"lastCpuPresentWaitMs", perf.cpuPresentWaitMs},
            {"lastGpuScenePassTimeMs", perf.gpuScenePassTimeMs},
            {"gpuSyncFastPathSkips", perf.gpuSyncSkipsCount},
            {"drawCallCount", perf.drawCallCount},
            {"triangleCount", perf.triangleCount},
            {"vmaActiveAllocatedBytes", budget.activeAllocatedBytes},
            {"vmaPeakAllocatedBytes", budget.peakAllocatedBytes},
            {"audioBackend", audioMetrics.backendStatus},
            {"audioSampleRateHz", audioMetrics.sampleRateHz},
            {"audioMeasuredUnderruns", audioMetrics.measuredUnderrunCount},
            {"validationWorldRoute", routeReceipt.ToJson()},
            {"materialSpecialistReceipt", specialistModel.GetActiveReceipt().ToJson()},
            {"gameplaySpecialistReceipt", studioUi.GetGameplaySpecialist().GetActiveReceipt().ToJson()},
            {"studioWindowCapturePng", winCapturePath.string()},
            {"viewportCapturePng", capturePath.string()},
            {"timestampUtc", platz::platform::UtcIso8601Now()}
        };

        if (!samplesCpuActiveMs.empty()) {
            auto stFrame = ComputeSampleStats(samplesCpuFrameMs);
            auto stActive = ComputeSampleStats(samplesCpuActiveMs);
            auto stSync = ComputeSampleStats(samplesCpuSyncMs);
            auto stUi = ComputeSampleStats(samplesCpuUiMs);
            auto stRec = ComputeSampleStats(samplesCpuRecordMs);
            auto stSub = ComputeSampleStats(samplesCpuSubmitMs);
            auto stWait = ComputeSampleStats(samplesCpuPresentWaitMs);
            auto stGpu = ComputeSampleStats(samplesGpuPassMs);
            receipt["sustainedBenchmark"] = {
                {"measuredSamples", samplesCpuActiveMs.size()},
                {"warmupFramesExcluded", 10},
                {"presentMode", perf.presentModeName},
                {"cpuTotalFrameWallClock", StatsToJson(stFrame)},
                {"cpuActiveWorkExclWait", StatsToJson(stActive)},
                {"cpuSceneSyncStage", StatsToJson(stSync)},
                {"cpuUiBuildStage", StatsToJson(stUi)},
                {"cpuCommandRecordStage", StatsToJson(stRec)},
                {"cpuQueueSubmitStage", StatsToJson(stSub)},
                {"cpuPresentWaitStage", StatsToJson(stWait)},
                {"gpuScenePassTimestamp", StatsToJson(stGpu)}
            };
            std::cout << "[PLATZ BENCHMARK] " << samplesCpuActiveMs.size() << " sustained frames (" << perf.presentModeName << "):\n"
                      << "  CPU Active Work: mean=" << stActive.mean << " ms, p50=" << stActive.p50 << " ms, p95=" << stActive.p95 << " ms, p99=" << stActive.p99 << " ms\n"
                      << "  CPU Breakdown (p50): Sync=" << stSync.p50 << " ms | UI=" << stUi.p50 << " ms | Record=" << stRec.p50 << " ms | Submit=" << stSub.p50 << " ms | PresentWait=" << stWait.p50 << " ms\n"
                      << "  GPU Scene Pass:  mean=" << stGpu.mean << " ms, p50=" << stGpu.p50 << " ms, p95=" << stGpu.p95 << " ms\n";
        }

        std::filesystem::path receiptPath = platz::platform::GetRepoRootDir() / "out" / "verification_receipt.json";
        {
            std::ofstream ofs(receiptPath, std::ios::binary | std::ios::trunc);
            ofs << receipt.dump(2);
        }

        std::cout << "[PLATZ VERIFY] Rendered " << framesRendered << " Vulkan 1.4 frames on " << caps.deviceName
                  << " (CPU Active=" << perf.cpuActiveWorkMs << " ms, Total=" << perf.cpuFrameTimeMs
                  << " ms, GPU=" << perf.gpuScenePassTimeMs
                  << " ms, SyncSkips=" << perf.gpuSyncSkipsCount
                  << ", ValidationErrors=" << renderer.GetContext().GetValidationErrorCount()
                  << ", StationsPassed=" << routeReceipt.stationsPassed << "/" << routeReceipt.stationsVisited << ")\n";
        std::cout << "[PLATZ VERIFY] Studio Window Screenshot: " << winCapturePath.string() << "\n";
        std::cout << "[PLATZ VERIFY] Receipt written to: " << receiptPath.string() << "\n";

        if (!routeReceipt.allStationsPassed || renderer.GetContext().GetValidationErrorCount() > 0) {
            audioEngine.Shutdown();
            renderer.Shutdown();
            window.Shutdown();
            return 2;
        }
    }

    audioEngine.Shutdown();
    renderer.Shutdown();
    window.Shutdown();
    return 0;
}
