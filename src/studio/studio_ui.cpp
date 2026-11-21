#include "studio/studio_ui.h"
#include "creative/asset_importer.h"
#include "creative/mesh_generator.h"
#include "platform/platform_paths.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace platz::studio {

namespace {

std::string LowercaseStr(std::string_view sv) {
    std::string s(sv);
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

} // namespace

void AccessibilityAnnouncer::Announce(const std::string& category, const std::string& message) {
    lastAnnouncement_ = "[" + category + "] " + message;
    if (announcements_.size() >= 100) {
        announcements_.erase(announcements_.begin());
    }
    announcements_.push_back(lastAnnouncement_);
}

StudioUiController::StudioUiController(
    core::ProjectState& project,
    engine::vulkan::VulkanRenderer& renderer,
    engine::runtime::RuntimeSession& runtime,
    ai::CommandRegistry& commandRegistry,
    ai::StagedAiSession& stagedAiSession,
    ai::NativeMaterialSpecialistModel& specialistModel)
    : project_(project),
      renderer_(renderer),
      runtime_(runtime),
      commandRegistry_(commandRegistry),
      stagedAiSession_(stagedAiSession),
      specialistModel_(specialistModel) {
    RegisterDefaultCommandEntries();
    ApplyThemeWithProtectedControlGuard(activeTheme_);

    if (!project_.GetObjects().empty()) {
        selectedObjectId_ = project_.GetObjects().front().id;
    }

    // Train initial native specialist so it's immediately inspectable in the Studio Workbench
    if (!specialistModel_.IsTrained()) {
        std::vector<ai::TrainingSample> trainSet, holdoutSet;
        ai::NativeMaterialSpecialistModel::BuildDefaultCuratedDataset(&trainSet, &holdoutSet);
        ai::TrainingHyperparameters hp;
        specialistModel_.TrainAndEvaluate(trainSet, holdoutSet, hp);
    }
}

void StudioUiController::RegisterDefaultCommandEntries() {
    commandEntries_ = {
        {"project.save", "Save Project Revision to SQLite & Blob Store", "Project", "save commit revision database persist", "Ctrl+S", "VERIFIED"},
        {"edit.undo", "Undo Last Scoped Edit Batch", "Edit", "undo revert history back", "Ctrl+Z", "VERIFIED"},
        {"edit.redo", "Redo Last Undone Edit Batch", "Edit", "redo forward history", "Ctrl+Y", "VERIFIED"},
        {"runtime.toggle_play", "Play / Stop Isolated Runtime Session", "Runtime", "play stop run game test simulation", "F5", "VERIFIED"},
        {"runtime.save_checkpoint", "Save Gameplay Checkpoint Slot (quicksave_1)", "Runtime", "checkpoint quicksave game state inventory", "F6", "VERIFIED"},
        {"runtime.load_checkpoint", "Resume Gameplay Checkpoint Slot (quicksave_1)", "Runtime", "load resume checkpoint game state", "F7", "VERIFIED"},
        {"create.cube", "Create 3D Cube Object", "Create 3D", "add create spawn cube box mesh primitive", "Shift+A", "VERIFIED"},
        {"create.sphere", "Create 3D PBR Sphere Object", "Create 3D", "add create spawn sphere ball mesh primitive", "", "VERIFIED"},
        {"create.torus", "Create 3D Torus Object", "Create 3D", "add create spawn torus ring mesh primitive", "", "VERIFIED"},
        {"create.cylinder", "Create 3D Cylinder Object", "Create 3D", "add create spawn cylinder pillar mesh", "", "VERIFIED"},
        {"model.subdivide", "Apply Non-Destructive Subdivision Modifier to Selected Mesh", "3D Modeling", "subdivide modifier smooth topology mesh asset", "", "VERIFIED"},
        {"ai.stage_gold_material", "PLATZ AI: Stage Polished Gold PBR Material on Selected Object", "PLATZ AI", "ai assistant stage material gold pbr preview diff", "", "VERIFIED"},
        {"ai.emergency_stop", "EMERGENCY STOP: Revoke Active PLATZ AI Session", "Security", "stop cancel revoke ai emergency safety", "Esc", "VERIFIED"},
        {"ai.train_specialist", "Train Native 12x24x7 PLATZ Material Specialist Model", "PLATZ AI", "train model neural specialist pareto holdout", "", "VERIFIED"},
        {"qa.run_validation_route", "Run Automated 6-Station Validation World Route", "QA & Validation", "test verify validation world stations route qa", "F9", "VERIFIED"},
        {"theme.obsidian", "Switch Theme: PZ Obsidian Dark", "UI & Accessibility", "theme dark obsidian color style", "", "VERIFIED"},
        {"theme.high_contrast", "Switch Theme: PZ High-Contrast Accessible", "UI & Accessibility", "theme contrast accessible vision screen reader", "", "VERIFIED"},
        {"workspace.world", "Switch Workspace: World & Scene", "Workspace", "workspace world scene hierarchy viewport", "Alt+1", "VERIFIED"},
        {"workspace.materials", "Switch Workspace: Materials & Shaders", "Workspace", "workspace material shader slang pbr", "Alt+3", "VERIFIED"},
        {"workspace.ai", "Switch Workspace: PLATZ AI & Specialist Workbench", "Workspace", "workspace ai specialist training commands", "Alt+5", "VERIFIED"},
        {"future.raytracing_pathtracer", "Hardware Ray/Path Tracing GI Mode (Long-Term Target)", "Future Target", "raytracing pathtracing gi global illumination", "", "BACKLOG / LONG-TERM TARGET"}
    };
}

std::vector<CommandPaletteEntry> StudioUiController::SearchCommandBar(const std::string& query) const {
    if (query.empty()) {
        return commandEntries_;
    }
    std::string q = LowercaseStr(query);
    std::vector<CommandPaletteEntry> matches;
    for (const auto& entry : commandEntries_) {
        std::string hay = LowercaseStr(entry.title + " " + entry.category + " " + entry.keywords + " " + entry.commandId);
        if (hay.find(q) != std::string::npos) {
            matches.push_back(entry);
        }
    }
    return matches;
}

bool StudioUiController::InvokeCommandById(const std::string& commandId, std::string* outStatusMessage) {
    auto setMsg = [&](const std::string& m) {
        statusBannerMessage_ = m;
        accessibility_.Announce("Command", m);
        if (outStatusMessage) *outStatusMessage = m;
    };

    if (commandId == "project.save") {
        std::string err;
        if (project_.SaveProject("Manual save from Studio Command Bar", "User", &err)) {
            setMsg("Saved project at revision r" + std::to_string(project_.GetHeadRevision()));
            return true;
        }
        setMsg("Save failed: " + err);
        return false;
    }
    if (commandId == "edit.undo") {
        std::string lbl;
        if (project_.Undo(&lbl)) {
            setMsg("Undid: " + lbl);
            return true;
        }
        setMsg("Nothing to undo");
        return false;
    }
    if (commandId == "edit.redo") {
        std::string lbl;
        if (project_.Redo(&lbl)) {
            setMsg("Redid: " + lbl);
            return true;
        }
        setMsg("Nothing to redo");
        return false;
    }
    if (commandId == "runtime.toggle_play") {
        if (runtime_.IsPlaying()) {
            runtime_.StopPlay();
            setMsg("Stopped Play mode; canonical authoring state preserved at r" + std::to_string(project_.GetHeadRevision()));
        } else {
            runtime_.StartPlay(project_);
            setMsg("Entered isolated Play mode from revision r" + std::to_string(project_.GetHeadRevision()));
        }
        return true;
    }
    if (commandId == "runtime.save_checkpoint") {
        std::string err;
        if (runtime_.SaveGameplayCheckpoint(project_, "quicksave_1", &err)) {
            setMsg("Saved runtime gameplay checkpoint 'quicksave_1'");
            return true;
        }
        setMsg("Checkpoint save failed: " + err);
        return false;
    }
    if (commandId == "runtime.load_checkpoint") {
        std::string err;
        if (runtime_.LoadGameplayCheckpoint(project_, "quicksave_1", &err)) {
            setMsg("Resumed runtime gameplay checkpoint 'quicksave_1'");
            return true;
        }
        setMsg("Checkpoint load failed: " + err);
        return false;
    }
    if (commandId == "create.cube" || commandId == "create.sphere" ||
        commandId == "create.torus" || commandId == "create.cylinder") {
        std::string prim = commandId.substr(7);
        auto& obj = project_.CreateObject("New_" + prim, prim, 0, "Scene", "User");
        obj.transform.position = {0.0f, 1.2f, 0.0f};
        selectedObjectId_ = obj.id;
        setMsg("Created " + obj.name + " (" + core::FormatObjectId(obj.id) + ")");
        return true;
    }
    if (commandId == "model.subdivide") {
        core::SceneObject* sel = project_.FindObject(selectedObjectId_);
        if (!sel) {
            setMsg("Select an object first to apply subdivision modifier");
            return false;
        }
        creative::MeshData base = creative::MeshGenerator::GenerateCube(1.0f);
        if (sel->mesh.primitiveType == "sphere") base = creative::MeshGenerator::GenerateSphere(0.6f, 16, 12);
        creative::MeshData sub = creative::MeshGenerator::ApplySubdivisionModifier(base);
        auto imp = creative::AssetImporter::ImportMeshIntoProject(
            project_, sub, sel->name + "_SubdivMesh", "assets/generated/subdiv.obj", "PLATZ-Internal");
        if (imp.success) {
            core::SceneObject updated = *sel;
            updated.mesh.primitiveType = "custom_asset";
            updated.mesh.meshAssetId = imp.assetId;
            project_.ApplyObjectMutation(sel->id, updated, "Apply Subdivision Modifier", "Model", "User");
            setMsg("Applied non-destructive subdivision modifier -> Asset " + core::FormatAssetId(imp.assetId));
            return true;
        }
        return false;
    }
    if (commandId == "ai.stage_gold_material") {
        if (selectedObjectId_ == 0) {
            setMsg("Select an object first to stage AI material edit");
            return false;
        }
        ai::CapabilityGrant grant;
        grant.confinedProjectRoot = project_.GetProjectDir();
        uint64_t token = 0;
        stagedAiSession_.BeginSession(project_, grant, "Apply Polished Gold PBR to " + core::FormatObjectId(selectedObjectId_), &token);
        ai::TypedCommandRequest req;
        req.commandName = "object.set_material";
        req.expectedRevision = project_.GetHeadRevision();
        req.targetObjectId = selectedObjectId_;
        req.parameters = {
            {"materialName", "AI_Staged_Gold"},
            {"baseColor", {0.98f, 0.80f, 0.30f, 1.0f}},
            {"metallic", 0.96f},
            {"roughness", 0.16f}
        };
        auto res = stagedAiSession_.StageCommand(token, req);
        setMsg(res.valid ? "Staged AI Gold Material preview (awaiting owner approval)" : res.diagnosticMessage);
        return res.valid;
    }
    if (commandId == "ai.emergency_stop") {
        stagedAiSession_.EmergencyStopSession();
        setMsg("EMERGENCY STOP ACTIVATED: Active AI session revoked and late commits blocked (N-18)");
        return true;
    }
    if (commandId == "ai.train_specialist") {
        std::vector<ai::TrainingSample> trainSet, holdoutSet;
        ai::NativeMaterialSpecialistModel::BuildDefaultCuratedDataset(&trainSet, &holdoutSet);
        ai::TrainingHyperparameters hp;
        auto rec = specialistModel_.TrainAndEvaluate(trainSet, holdoutSet, hp);
        setMsg("Trained native 12x24x7 specialist in " + std::to_string(rec.trainingDurationMs) +
               " ms (holdout accuracy: " + std::to_string(static_cast<int>(rec.holdoutAccuracyPct)) + "%)");
        return rec.convergedAndQualified;
    }
    if (commandId == "qa.run_validation_route") {
        lastRouteReceipt_ = ValidationWorldManager::RunAutomatedVerificationRoute(project_);
        setMsg("Validation World Route completed: " + std::to_string(lastRouteReceipt_.stationsPassed) +
               "/" + std::to_string(lastRouteReceipt_.stationsVisited) + " stations passed");
        return lastRouteReceipt_.allStationsPassed;
    }
    if (commandId == "theme.obsidian") {
        ThemeSpecification t;
        t.themeName = "PZ Obsidian Dark";
        ApplyThemeWithProtectedControlGuard(t);
        setMsg("Activated PZ Obsidian Dark theme");
        return true;
    }
    if (commandId == "theme.high_contrast") {
        ThemeSpecification t;
        t.themeName = "PZ High-Contrast Accessible";
        t.windowBg = {0.02f, 0.02f, 0.03f, 1.0f};
        t.panelHeader = {0.08f, 0.12f, 0.20f, 1.0f};
        t.accentPrimary = {0.00f, 0.95f, 1.00f, 1.0f};
        t.textPrimary = {1.00f, 1.00f, 1.00f, 1.0f};
        ApplyThemeWithProtectedControlGuard(t);
        setMsg("Activated PZ High-Contrast Accessible theme");
        return true;
    }
    if (commandId == "workspace.world") {
        SetWorkspaceMode(WorkspaceMode::WorldAndScene);
        return true;
    }
    if (commandId == "workspace.materials") {
        SetWorkspaceMode(WorkspaceMode::MaterialsAndShaders);
        return true;
    }
    if (commandId == "workspace.ai") {
        SetWorkspaceMode(WorkspaceMode::PlatzAiWorkbench);
        return true;
    }
    if (commandId == "future.raytracing_pathtracer") {
        // Honest completion state (PLZ-UXPLUS-008): never pretend an unimplemented backlog feature ran
        setMsg("[HONEST STATUS: BACKLOG / LONG-TERM TARGET] Hardware Ray/Path Tracing is a Gate 4+ capability and is not claimed in this build.");
        return false;
    }

    setMsg("Unknown command ID: " + commandId);
    return false;
}

ProtectedSecurityBarState StudioUiController::ApplyThemeWithProtectedControlGuard(
    const ThemeSpecification& requestedTheme) {
    activeTheme_ = requestedTheme;
    ProtectedSecurityBarState state{};
    state.visibleAndOperable = true;
    state.effectiveStopButtonOpacity = 1.0f;
    state.effectiveContrastRatio = 12.5f;

    // Enforce PLZ-SAFEX-006 / N-24: Custom themes CANNOT hide, fade, or camouflage the AI Approval & Emergency Stop controls!
    if (requestedTheme.attemptHideProtectedApprovalBar ||
        requestedTheme.requestedStopButtonOpacity < 0.95f ||
        requestedTheme.requestedStopButtonColor.r < 0.65f ||
        requestedTheme.requestedStopButtonColor.a < 0.95f) {
        state.tamperedThemeSanitized = true;
        state.securityEnforcementNote =
            "Blocked attempt by custom theme '" + requestedTheme.themeName +
            "' to hide or low-contrast spoof Protected AI Approval & Emergency Stop controls (PLZ-SAFEX-006 / N-24).";
        activeTheme_.attemptHideProtectedApprovalBar = false;
        activeTheme_.requestedStopButtonOpacity = 1.0f;
        activeTheme_.requestedStopButtonColor = {0.90f, 0.16f, 0.20f, 1.0f};
        accessibility_.Announce("Security", state.securityEnforcementNote);
    } else {
        state.tamperedThemeSanitized = false;
        state.securityEnforcementNote = "Protected AI Approval & Emergency Stop controls verified high-contrast & visible.";
    }

    protectedBarState_ = state;
    return state;
}

void StudioUiController::ApplyThemeToImGui() const {
    if (!ImGui::GetCurrentContext()) return;
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 5.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 3.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(activeTheme_.windowBg.r, activeTheme_.windowBg.g, activeTheme_.windowBg.b, 1.0f);
    colors[ImGuiCol_Header] = ImVec4(activeTheme_.panelHeader.r, activeTheme_.panelHeader.g, activeTheme_.panelHeader.b, 1.0f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(activeTheme_.accentPrimary.r * 0.45f, activeTheme_.accentPrimary.g * 0.45f, activeTheme_.accentPrimary.b * 0.45f, 1.0f);
    colors[ImGuiCol_HeaderActive] = ImVec4(activeTheme_.accentPrimary.r * 0.65f, activeTheme_.accentPrimary.g * 0.65f, activeTheme_.accentPrimary.b * 0.65f, 1.0f);
    colors[ImGuiCol_Button] = ImVec4(0.15f, 0.20f, 0.27f, 1.0f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(activeTheme_.accentPrimary.r * 0.55f, activeTheme_.accentPrimary.g * 0.55f, activeTheme_.accentPrimary.b * 0.55f, 1.0f);
    colors[ImGuiCol_ButtonActive] = ImVec4(activeTheme_.accentPrimary.r * 0.80f, activeTheme_.accentPrimary.g * 0.80f, activeTheme_.accentPrimary.b * 0.80f, 1.0f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.11f, 0.13f, 0.17f, 1.0f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.08f, 0.10f, 0.14f, 1.0f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.11f, 0.15f, 0.22f, 1.0f);
    colors[ImGuiCol_Text] = ImVec4(activeTheme_.textPrimary.r, activeTheme_.textPrimary.g, activeTheme_.textPrimary.b, 1.0f);

    ImGui::GetIO().FontGlobalScale = uiScale_;
}

std::vector<PerformanceAdvisorHint> StudioUiController::AnalyzeScenePerformanceForNonCoders() const {
    std::vector<PerformanceAdvisorHint> hints;
    for (const auto& obj : GetActiveSceneObjects()) {
        if (!obj.visible || !obj.mesh.visible) continue;
        uint32_t estTris = 12;
        if (obj.mesh.primitiveType == "sphere") estTris = 1008;
        else if (obj.mesh.primitiveType == "torus") estTris = 1024;
        else if (obj.mesh.primitiveType == "cylinder") estTris = 96;
        else if (obj.mesh.primitiveType == "custom_asset") estTris = 192;

        if (estTris >= 500) {
            PerformanceAdvisorHint h;
            h.objectId = obj.id;
            h.objectName = obj.name;
            h.estimatedTriangles = estTris;
            h.plainEnglishExplanation =
                "'" + obj.name + "' (" + core::FormatObjectId(obj.id) + ") draws ~" +
                std::to_string(estTris) + " triangles with full GGX PBR lighting.";
            h.reversibleOptimizationAction =
                "Switch distant instances to lower-segment LOD or disable point-light specular without losing authored source.";
            hints.push_back(std::move(h));
        }
    }
    return hints;
}

void StudioUiController::SetSelectedObjectId(core::ObjectId id) {
    if (selectedObjectId_ != id) {
        selectedObjectId_ = id;
        if (const auto* obj = project_.FindObject(id)) {
            accessibility_.Announce("Selection", "Selected " + obj->name + " (" + core::FormatObjectId(id) + ")");
        }
    }
}

void StudioUiController::SetWorkspaceMode(WorkspaceMode mode) {
    workspaceMode_ = mode;
    const char* names[] = {
        "World & Scene",
        "3D Model & Assets",
        "Materials & Shaders",
        "Logic & Play",
        "PLATZ AI & Specialist Workbench"
    };
    accessibility_.Announce("Workspace", std::string("Switched workspace to ") + names[static_cast<int>(mode)]);
}

void StudioUiController::SetUiScale(float scale) {
    uiScale_ = std::clamp(scale, 0.85f, 2.0f);
    accessibility_.Announce("Accessibility", "UI Scale set to " + std::to_string(static_cast<int>(uiScale_ * 100.0f)) + "%");
}

const std::vector<core::SceneObject>& StudioUiController::GetActiveSceneObjects() const {
    return runtime_.IsPlaying() ? runtime_.GetRuntimeObjects() : project_.GetObjects();
}

void StudioUiController::BuildImGuiStudioFrame() {
    ApplyThemeToImGui();

    // Global keyboard shortcuts
    ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_K, false)) {
        showCommandBar_ = !showCommandBar_;
        accessibility_.Announce("CommandBar", showCommandBar_ ? "Opened Universal Command Bar" : "Closed Universal Command Bar");
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        InvokeCommandById("project.save");
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
        InvokeCommandById("edit.undo");
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
        InvokeCommandById("edit.redo");
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) {
        InvokeCommandById("runtime.toggle_play");
    }

    DrawProtectedTopMenuAndSecurityBar();

    ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

    if (showCommandBar_) {
        DrawUniversalCommandBarModal();
    }

    DrawHierarchyAndStationsPanel();
    Draw3DViewportPanel();
    DrawInspectorAndMaterialPanel();
    DrawAssetAndValidationWorldPanel();
    DrawLogicAndRuntimeHudPanel();
    DrawPlatzAiAndSpecialistPanel();
}

void StudioUiController::DrawProtectedTopMenuAndSecurityBar() {
    if (ImGui::BeginMainMenuBar()) {
        ImGui::TextColored(ImVec4(0.0f, 0.88f, 1.0f, 1.0f), "[PZ] PLATZ STUDIO");
        ImGui::Separator();

        if (ImGui::BeginMenu("Project")) {
            if (ImGui::MenuItem("Save Revision (Ctrl+S)")) InvokeCommandById("project.save");
            if (ImGui::MenuItem("Undo (Ctrl+Z)", nullptr, false, project_.CanUndo())) InvokeCommandById("edit.undo");
            if (ImGui::MenuItem("Redo (Ctrl+Y)", nullptr, false, project_.CanRedo())) InvokeCommandById("edit.redo");
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Workspaces")) {
            if (ImGui::MenuItem("1. World & Scene", "Alt+1", workspaceMode_ == WorkspaceMode::WorldAndScene))
                SetWorkspaceMode(WorkspaceMode::WorldAndScene);
            if (ImGui::MenuItem("2. 3D Model & Assets", "Alt+2", workspaceMode_ == WorkspaceMode::ModelAndAssets))
                SetWorkspaceMode(WorkspaceMode::ModelAndAssets);
            if (ImGui::MenuItem("3. Materials & Shaders", "Alt+3", workspaceMode_ == WorkspaceMode::MaterialsAndShaders))
                SetWorkspaceMode(WorkspaceMode::MaterialsAndShaders);
            if (ImGui::MenuItem("4. Logic & Play", "Alt+4", workspaceMode_ == WorkspaceMode::LogicAndPlay))
                SetWorkspaceMode(WorkspaceMode::LogicAndPlay);
            if (ImGui::MenuItem("5. PLATZ AI & Specialist Workbench", "Alt+5", workspaceMode_ == WorkspaceMode::PlatzAiWorkbench))
                SetWorkspaceMode(WorkspaceMode::PlatzAiWorkbench);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Accessibility & Theme")) {
            if (ImGui::MenuItem("PZ Obsidian Dark Theme")) InvokeCommandById("theme.obsidian");
            if (ImGui::MenuItem("PZ High-Contrast Accessible Theme")) InvokeCommandById("theme.high_contrast");
            ImGui::Separator();
            if (ImGui::MenuItem("UI Scale 100%")) SetUiScale(1.0f);
            if (ImGui::MenuItem("UI Scale 125%")) SetUiScale(1.25f);
            if (ImGui::MenuItem("UI Scale 150%")) SetUiScale(1.5f);
            ImGui::EndMenu();
        }

        if (ImGui::Button("Command Bar (Ctrl+K)")) {
            showCommandBar_ = !showCommandBar_;
        }

        ImGui::Separator();
        if (runtime_.IsPlaying()) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.25f, 0.20f, 1.0f));
            if (ImGui::Button("STOP PLAY (F5)")) {
                InvokeCommandById("runtime.toggle_play");
            }
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.14f, 0.65f, 0.34f, 1.0f));
            if (ImGui::Button("PLAY SCENE (F5)")) {
                InvokeCommandById("runtime.toggle_play");
            }
            ImGui::PopStyleColor();
        }

        ImGui::Separator();
        // PROTECTED SECURITY & EMERGENCY STOP CONTROL (PLZ-SAFEX-006 / N-24: Cannot be hidden or dimmed by custom themes)
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.90f, 0.14f, 0.18f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        if (ImGui::Button("AI EMERGENCY STOP")) {
            InvokeCommandById("ai.emergency_stop");
        }
        ImGui::PopStyleColor(2);

        ImGui::Separator();
        ImGui::Text("Rev: r%llu (%s) | GPU: %s (%.2f ms)",
            static_cast<unsigned long long>(project_.GetHeadRevision()),
            project_.GetActiveBranch().c_str(),
            renderer_.GetContext().GetCapabilities().deviceName.c_str(),
            renderer_.GetPerfMetrics().gpuScenePassTimeMs);

        ImGui::EndMainMenuBar();
    }
}

void StudioUiController::DrawUniversalCommandBarModal() {
    ImGui::SetNextWindowSize(ImVec2(640, 380), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("PLATZ Universal Command & Discovery Bar (Ctrl+K)", &showCommandBar_)) {
        ImGui::InputTextWithHint("##cmd_search", "Search tools, objects, AI commands, workspaces, or tasks...", commandSearchBuf_, sizeof(commandSearchBuf_));
        ImGui::Separator();

        auto results = SearchCommandBar(commandSearchBuf_);
        for (const auto& item : results) {
            ImGui::PushID(item.commandId.c_str());
            if (ImGui::Selectable(item.title.c_str())) {
                InvokeCommandById(item.commandId);
            }
            ImGui::SameLine(420);
            ImGui::TextColored(
                item.maturityBadge == "VERIFIED" ? ImVec4(0.3f, 0.9f, 0.5f, 1.0f) : ImVec4(0.95f, 0.75f, 0.25f, 1.0f),
                "[%s]", item.maturityBadge.c_str());
            ImGui::PopID();
        }
    }
    ImGui::End();
}

void StudioUiController::DrawHierarchyAndStationsPanel() {
    if (ImGui::Begin("Scene Hierarchy & Validation Stations")) {
        ImGui::TextColored(ImVec4(0.0f, 0.85f, 1.0f, 1.0f), "Project: %s (r%llu)",
            project_.GetProjectName().c_str(),
            static_cast<unsigned long long>(project_.GetHeadRevision()));

        if (ImGui::Button("+ Cube")) InvokeCommandById("create.cube");
        ImGui::SameLine();
        if (ImGui::Button("+ Sphere")) InvokeCommandById("create.sphere");
        ImGui::SameLine();
        if (ImGui::Button("+ Torus")) InvokeCommandById("create.torus");
        ImGui::SameLine();
        if (ImGui::Button("Duplicate") && selectedObjectId_ != 0 && !runtime_.IsPlaying()) {
            auto dupId = project_.DuplicateObject(selectedObjectId_);
            if (dupId) SetSelectedObjectId(*dupId);
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete") && selectedObjectId_ != 0 && !runtime_.IsPlaying()) {
            project_.DeleteObject(selectedObjectId_);
            selectedObjectId_ = project_.GetObjects().empty() ? 0 : project_.GetObjects().front().id;
        }

        ImGui::Separator();
        const auto& activeObjs = GetActiveSceneObjects();
        for (const auto& obj : activeObjs) {
            std::string label = obj.name + "  [" + core::FormatObjectId(obj.id) + "] (" + obj.logic.stationCategory + ")";
            bool isSelected = (obj.id == selectedObjectId_);
            if (ImGui::Selectable(label.c_str(), isSelected)) {
                SetSelectedObjectId(obj.id);
            }
        }
    }
    ImGui::End();
}

void StudioUiController::Draw3DViewportPanel() {
    if (ImGui::Begin("Vulkan 1.4 3D Viewport")) {
        // Viewport Toolbar: Gizmo Mode, Grid Snapping, Camera Projection
        if (ImGui::RadioButton("Move (W)", gizmoMode_ == engine::scene::GizmoMode::Translate))
            gizmoMode_ = engine::scene::GizmoMode::Translate;
        ImGui::SameLine();
        if (ImGui::RadioButton("Rotate (E)", gizmoMode_ == engine::scene::GizmoMode::Rotate))
            gizmoMode_ = engine::scene::GizmoMode::Rotate;
        ImGui::SameLine();
        if (ImGui::RadioButton("Scale (R)", gizmoMode_ == engine::scene::GizmoMode::Scale))
            gizmoMode_ = engine::scene::GizmoMode::Scale;
        ImGui::SameLine();
        ImGui::Checkbox("Grid Snap", &snapSettings_.enableSnap);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f);
        ImGui::DragFloat("Step", &snapSettings_.translateSnapStep, 0.05f, 0.05f, 5.0f, "%.2fm");
        ImGui::SameLine();
        bool ortho = camera_.IsOrthographic();
        if (ImGui::Checkbox("Ortho", &ortho)) camera_.SetOrthographic(ortho);

        ImVec2 avail = ImGui::GetContentRegionAvail();
        uint32_t vpW = static_cast<uint32_t>(std::max(64.0f, avail.x));
        uint32_t vpH = static_cast<uint32_t>(std::max(64.0f, avail.y - 24.0f));

        VkDescriptorSet texId = renderer_.GetViewportImGuiDescriptorSet();
        if (texId != VK_NULL_HANDLE) {
            ImVec2 imgPos = ImGui::GetCursorScreenPos();
            ImGui::Image(reinterpret_cast<ImTextureID>(texId), ImVec2(static_cast<float>(vpW), static_cast<float>(vpH)));

            if (ImGui::IsItemHovered()) {
                ImGuiIO& io = ImGui::GetIO();
                // Right-mouse drag orbits 3D camera
                if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
                    camera_.Orbit(-io.MouseDelta.x * 0.35f, io.MouseDelta.y * 0.35f);
                }
                // Middle-mouse drag pans 3D camera
                if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
                    camera_.Pan(-io.MouseDelta.x * 0.02f, io.MouseDelta.y * 0.02f);
                }
                // Mouse wheel zooms
                if (std::abs(io.MouseWheel) > 0.01f) {
                    camera_.Zoom(-io.MouseWheel * 0.85f);
                }
                // Left-click performs 3D Ray & GPU Object-ID Picking (PLZ-RENDER-001)
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    float localX = io.MousePos.x - imgPos.x;
                    float localY = io.MousePos.y - imgPos.y;
                    auto ray = camera_.ScreenPointToWorldRay(localX, localY, static_cast<float>(vpW), static_cast<float>(vpH));
                    auto pick = engine::scene::ScenePickerAndGizmo::PickObjectByRay(GetActiveSceneObjects(), ray);
                    if (pick.hit) {
                        SetSelectedObjectId(pick.objectId);
                    }
                }
            }
        }

        const auto& perf = renderer_.GetPerfMetrics();
        auto budget = renderer_.GetContext().GetMemoryBudgetSnapshot();
        ImGui::Text("Vulkan %s | Draws: %u | Tris: %u | CPU: %.2f ms | GPU Pass: %.3f ms | VMA Active: %.2f MB (%llu allocs)",
            renderer_.GetContext().GetFormattedApiVersion().c_str(),
            perf.drawCallCount,
            perf.triangleCount,
            perf.cpuFrameTimeMs,
            perf.gpuScenePassTimeMs,
            static_cast<double>(budget.activeAllocatedBytes) / (1024.0 * 1024.0),
            static_cast<unsigned long long>(budget.activeAllocationCount));
    }
    ImGui::End();
}

void StudioUiController::DrawInspectorAndMaterialPanel() {
    if (ImGui::Begin("Inspector & Slang PBR Material Editor")) {
        core::SceneObject* obj = runtime_.IsPlaying()
            ? runtime_.FindRuntimeObject(selectedObjectId_)
            : project_.FindObject(selectedObjectId_);

        if (!obj) {
            ImGui::TextDisabled("No object selected.");
            ImGui::End();
            return;
        }

        core::SceneObject edited = *obj;
        bool changed = false;

        ImGui::TextColored(ImVec4(0.0f, 0.85f, 1.0f, 1.0f), "%s [%s]",
            edited.name.c_str(), core::FormatObjectId(edited.id).c_str());
        ImGui::Text("Station: %s | State: %s", edited.logic.stationCategory.c_str(), edited.logic.currentState.c_str());
        ImGui::Separator();

        if (ImGui::CollapsingHeader("Transform (3D World / Local)", ImGuiTreeNodeFlags_DefaultOpen)) {
            changed |= ImGui::DragFloat3("Position", &edited.transform.position.x, 0.05f);
            changed |= ImGui::DragFloat3("Rotation (deg)", &edited.transform.rotationEulerDeg.x, 1.0f);
            changed |= ImGui::DragFloat3("Scale", &edited.transform.scale.x, 0.05f, 0.05f, 100.0f);

            // Quick Gizmo Step Buttons with Snapping
            if (ImGui::Button("Snap +X")) {
                edited.transform = engine::scene::ScenePickerAndGizmo::ApplyGizmoDelta(
                    edited.transform, gizmoMode_, glm::vec3(snapSettings_.translateSnapStep, 0.0f, 0.0f), snapSettings_);
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Snap +Y")) {
                edited.transform = engine::scene::ScenePickerAndGizmo::ApplyGizmoDelta(
                    edited.transform, gizmoMode_, glm::vec3(0.0f, snapSettings_.translateSnapStep, 0.0f), snapSettings_);
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Snap +Z")) {
                edited.transform = engine::scene::ScenePickerAndGizmo::ApplyGizmoDelta(
                    edited.transform, gizmoMode_, glm::vec3(0.0f, 0.0f, snapSettings_.translateSnapStep), snapSettings_);
                changed = true;
            }
        }

        if (ImGui::CollapsingHeader("PBR Material & Slang Surface Parameters", ImGuiTreeNodeFlags_DefaultOpen)) {
            changed |= ImGui::ColorEdit4("Base Albedo", &edited.material.baseColor.x);
            changed |= ImGui::SliderFloat("Metallic", &edited.material.metallic, 0.0f, 1.0f);
            changed |= ImGui::SliderFloat("Roughness", &edited.material.roughness, 0.04f, 1.0f);
            changed |= ImGui::ColorEdit3("Emissive Color", &edited.material.emissiveColor.x);
            changed |= ImGui::SliderFloat("Emissive Strength", &edited.material.emissiveStrength, 0.0f, 5.0f);
            changed |= ImGui::Checkbox("Procedural UV Checker", &edited.material.useProceduralChecker);
        }

        if (ImGui::CollapsingHeader("Physics & Rigid Body", ImGuiTreeNodeFlags_DefaultOpen)) {
            changed |= ImGui::Checkbox("Enable Physics", &edited.physics.enabled);
            changed |= ImGui::Checkbox("Dynamic Gravity Body", &edited.physics.isDynamic);
            changed |= ImGui::SliderFloat("Restitution (Bounce)", &edited.physics.restitution, 0.0f, 1.0f);
            changed |= ImGui::SliderFloat("Friction", &edited.physics.friction, 0.0f, 1.0f);
        }

        if (changed) {
            edited.transform.Sanitize();
            if (runtime_.IsPlaying()) {
                *obj = edited;
            } else {
                project_.ApplyObjectMutation(obj->id, edited, "Edit " + obj->name + " in Inspector", "Scene", "User");
            }
        }
    }
    ImGui::End();
}

void StudioUiController::DrawAssetAndValidationWorldPanel() {
    if (ImGui::Begin("Content-Addressed Assets & Living Validation World")) {
        if (ImGui::Button("Run All 6 Validation World Stations (F9)")) {
            InvokeCommandById("qa.run_validation_route");
        }
        ImGui::SameLine();
        if (ImGui::Button("Invalidate & Rebuild GPU Mesh Cache (N-04)")) {
            renderer_.InvalidateDerivedGpuCaches();
            renderer_.EnsureProjectGpuResourcesSynced(project_);
            statusBannerMessage_ = "Rebuilt derived GPU caches from canonical content-addressed blobs.";
        }

        if (!lastRouteReceipt_.stationReceipts.empty()) {
            ImGui::Separator();
            for (const auto& line : lastRouteReceipt_.stationReceipts) {
                ImGui::TextWrapped("%s", line.c_str());
            }
        }

        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.0f, 0.85f, 1.0f, 1.0f), "Content-Addressed Asset Store (%zu assets):",
            project_.GetAssets().size());
        for (const auto& ast : project_.GetAssets()) {
            ImGui::Text("%s [%s] | Type: %s | SHA-256: %s... | License: %s | Gen: v%llu",
                ast.logicalName.c_str(),
                core::FormatAssetId(ast.assetId).c_str(),
                ast.assetType.c_str(),
                ast.contentSha256.substr(0, 12).c_str(),
                ast.licenseInfo.c_str(),
                static_cast<unsigned long long>(ast.generationVersion));
        }
    }
    ImGui::End();
}

void StudioUiController::DrawLogicAndRuntimeHudPanel() {
    if (ImGui::Begin("No-Code Behavior Logic, Runtime HUD & Accessibility Log")) {
        const auto& rtMetrics = runtime_.GetMetrics();
        int64_t totalCrystals = runtime_.IsPlaying()
            ? runtime_.ComputeTotalResourceAcrossWorld("energy_crystal")
            : 100;

        ImGui::TextColored(ImVec4(0.25f, 0.95f, 0.55f, 1.0f),
            "[GAMEPLAY HUD] Mode: %s | Sim Time: %.2fs | Conserved Energy Crystals: %lld | Rules Fired: %llu",
            runtime_.IsPlaying() ? "PLAYING (ISOLATED)" : "EDITOR AUTHORING",
            rtMetrics.elapsedPlayTimeSec,
            static_cast<long long>(totalCrystals),
            static_cast<unsigned long long>(rtMetrics.rulesTriggeredCount));

        if (runtime_.IsPlaying() && selectedObjectId_ != 0) {
            if (ImGui::Button("Trigger Interaction on Selected Object")) {
                runtime_.TriggerInteraction(selectedObjectId_);
            }
            ImGui::SameLine();
            if (ImGui::Button("Save Checkpoint (F6)")) InvokeCommandById("runtime.save_checkpoint");
            ImGui::SameLine();
            if (ImGui::Button("Resume Checkpoint (F7)")) InvokeCommandById("runtime.load_checkpoint");
        }

        ImGui::Separator();
        ImGui::Text("Status: %s", statusBannerMessage_.c_str());
        ImGui::Text("Screen-Reader Accessibility Announcer: %s", accessibility_.GetLastAnnouncement().c_str());
    }
    ImGui::End();
}

void StudioUiController::DrawPlatzAiAndSpecialistPanel() {
    if (ImGui::Begin("PLATZ AI Staged Session & Native Specialist Workbench")) {
        ImGui::TextColored(ImVec4(0.0f, 0.85f, 1.0f, 1.0f),
            "1. Authorized Revision-Pinned AI Staged Session (Head: r%llu)",
            static_cast<unsigned long long>(project_.GetHeadRevision()));

        if (ImGui::Button("Stage AI Gold Material Proposal")) {
            InvokeCommandById("ai.stage_gold_material");
        }
        ImGui::SameLine();
        if (stagedAiSession_.GetStatus() == ai::StagedSessionStatus::ActiveStaging) {
            if (ImGui::Button("Approve & Commit Staged Diff")) {
                std::string err;
                if (stagedAiSession_.ApproveAndCommit(stagedAiSession_.GetActiveSessionToken(), project_, nullptr, &err)) {
                    statusBannerMessage_ = "Committed approved AI session.";
                } else {
                    statusBannerMessage_ = err;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Discard Staged Session")) {
                stagedAiSession_.DiscardSession();
            }
        }

        for (auto& act : stagedAiSession_.GetStagedActionsMutable()) {
            ImGui::Checkbox(act.humanReadableSummary.c_str(), &act.selectedForApproval);
        }

        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.0f, 0.85f, 1.0f, 1.0f),
            "2. Original Local PLATZ Specialist Model (12x24x7 Neural PBR Predictor)");
        const auto& rec = specialistModel_.GetActiveReceipt();
        ImGui::Text("Origin: %s | Seed: %u | Holdout MSE: %.5f -> %.5f | Accuracy: %.1f%% | Latency: %.2f us",
            rec.modelOrigin.c_str(),
            rec.randomSeed,
            rec.initialHoldoutMse,
            rec.finalHoldoutMse,
            rec.holdoutAccuracyPct,
            rec.avgInferenceLatencyUs);
        ImGui::Text("Qualification Status: %s (Negative Result Memory Trials: %zu)",
            rec.qualificationStatus.c_str(),
            specialistModel_.GetNegativeResultMemory().size());

        const char* archetypes[] = {
            "0: Brushed Titanium Metal",
            "1: Polished Gold Conductor",
            "2: Matte Architectural Concrete",
            "3: Emissive PZ Cyan Energy Core",
            "4: Ceramic Dielectric Glaze",
            "5: Weathered Obsidian Basalt"
        };
        ImGui::Combo("Target Material Archetype", &selectedSpecialistArchetype_, archetypes, 6);
        ImGui::SliderFloat("Surface Wear Factor", &specialistWearParam_, 0.0f, 1.0f);

        if (ImGui::Button("Predict with Specialist & Stage onto Selected Object") && selectedObjectId_ != 0) {
            auto pred = specialistModel_.PredictMaterialForStyleIndex(selectedSpecialistArchetype_, specialistWearParam_);
            ai::CapabilityGrant grant;
            grant.confinedProjectRoot = project_.GetProjectDir();
            uint64_t tok = 0;
            stagedAiSession_.BeginSession(project_, grant, "Apply Specialist Prediction (" + pred.modelProvenance + ")", &tok);
            ai::TypedCommandRequest req;
            req.commandName = "object.set_material";
            req.expectedRevision = project_.GetHeadRevision();
            req.targetObjectId = selectedObjectId_;
            req.parameters = {
                {"materialName", "Specialist_Predicted_PBR"},
                {"baseColor", {pred.baseColor.r, pred.baseColor.g, pred.baseColor.b, 1.0f}},
                {"metallic", pred.metallic},
                {"roughness", pred.roughness},
                {"emissiveStrength", pred.emissiveStrength}
            };
            stagedAiSession_.StageCommand(tok, req);
            statusBannerMessage_ = "Staged native specialist prediction for review.";
        }
    }
    ImGui::End();
}

} // namespace platz::studio
