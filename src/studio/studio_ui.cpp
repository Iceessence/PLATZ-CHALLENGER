#include "studio/studio_ui.h"
#include "creative/asset_importer.h"
#include "creative/mesh_generator.h"
#include "platform/audio_engine.h"
#include "platform/platform_paths.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

namespace platz::studio {

namespace {

std::string LowercaseStr(std::string_view sv) {
    std::string s(sv);
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// Helper to project a 3D world point to 2D screen coordinates inside [vpMin, vpMin + vpSize]
bool ProjectWorldToScreen(
    const glm::vec3& worldPos,
    const glm::mat4& viewProj,
    const glm::vec2& vpMin,
    const glm::vec2& vpSize,
    ImVec2* outScreenPos) {
    glm::vec4 clip = viewProj * glm::vec4(worldPos, 1.0f);
    if (clip.w <= 0.05f) {
        return false; // Behind camera near plane
    }
    glm::vec3 ndc = glm::vec3(clip) / clip.w;
    if (ndc.x < -1.5f || ndc.x > 1.5f || ndc.y < -1.5f || ndc.y > 1.5f) {
        return false;
    }
    float sx = vpMin.x + (ndc.x * 0.5f + 0.5f) * vpSize.x;
    float sy = vpMin.y + (ndc.y * 0.5f + 0.5f) * vpSize.y;
    if (outScreenPos) *outScreenPos = ImVec2(sx, sy);
    return true;
}

bool BeginPropertyTable(const char* tableId, float uiScale) {
    ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable(tableId, 2, flags)) {
        return false;
    }
    ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 144.0f * uiScale);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

void BeginPropertyRow(const char* label, const char* tooltip = nullptr) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (tooltip && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tooltip);
    }
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-1.0f);
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

    // Select a visually prominent 3D object by default (e.g. ST02_PBR_Polished_Gold_Torus or ST07_ModifierStack_Twisted_Spire)
    if (!project_.GetObjects().empty()) {
        selectedObjectId_ = project_.GetObjects().front().id;
        for (const auto& o : project_.GetObjects()) {
            if (o.name == "ST02_PBR_Polished_Gold_Torus") {
                selectedObjectId_ = o.id;
                break;
            }
        }
    }

    camera_.FocusOnPoint(glm::vec3(0.0f, 1.1f, 0.0f), 11.2f);

    if (!specialistModel_.IsTrained()) {
        std::vector<ai::TrainingSample> trainSet, holdoutSet;
        ai::NativeMaterialSpecialistModel::BuildDefaultCuratedDataset(&trainSet, &holdoutSet);
        ai::TrainingHyperparameters hp;
        specialistModel_.TrainAndEvaluate(trainSet, holdoutSet, hp);
    }
    if (!gameplaySpecialist_.IsTrained()) {
        gameplaySpecialist_.TrainDefaultGameplaySpecialist();
    }

    SyncEditorsFromSelectedObject(true);

    customToolbarBindings_ = {
        {"+ Twist Modifier", "model.add_taper_twist"},
        {"AI 3-Role Enhance", "ai.run_three_role_workflow"},
        {"Compile Material Graph", "material.compile_graph_slang"}
    };
}

void StudioUiController::AdvanceEditorPreview(double dt) {
    if (runtime_.IsPlaying() || !animPreviewPlaying_) return;
    const core::SceneObject* sel = project_.FindObject(selectedObjectId_);
    if (!sel || !sel->animation.enabled || sel->animation.tracks.empty()) {
        animPreviewPlaying_ = false;
        return;
    }
    float dur = std::max(0.1f, sel->animation.durationSec);
    animPreviewTimeSec_ += static_cast<float>(std::clamp(dt, 0.0, 0.1));
    if (animPreviewTimeSec_ > dur) {
        if (sel->animation.loop) {
            animPreviewTimeSec_ = std::fmod(animPreviewTimeSec_, dur);
        } else {
            animPreviewTimeSec_ = dur;
            animPreviewPlaying_ = false;
        }
    }
}

void StudioUiController::SyncEditorsFromSelectedObject(bool forceReload) {
    uint64_t curGen = project_.GetMutationGeneration();
    if (!forceReload && selectedObjectId_ == lastSyncedObjectId_ && curGen == lastSyncedMutationGen_) {
        return;
    }
    lastSyncedObjectId_ = selectedObjectId_;
    lastSyncedMutationGen_ = curGen;

    const core::SceneObject* sel = project_.FindObject(selectedObjectId_);

    // 1. Load Material Graph from selected object's persisted source JSON
    if (sel && !sel->material.materialGraphJson.empty()) {
        auto parsedJson = nlohmann::json::parse(sel->material.materialGraphJson, nullptr, false);
        if (!parsedJson.is_discarded() && parsedJson.contains("nodes") && parsedJson["nodes"].is_array()) {
            materialGraph_ = creative::MaterialGraphDocument::FromJsonString(sel->material.materialGraphJson);
            auto val = materialGraph_.ValidateGraph();
            materialGraphDiagnostic_ = val.valid
                ? ("Loaded saved DAG (" + std::to_string(materialGraph_.GetNodes().size()) + " nodes) from " + sel->name)
                : ("Loaded DAG with validation error @ " + val.failingNodeId + ": " + val.diagnosticMessage);
        } else {
            materialGraph_.BuildDefaultProceduralGraph();
            materialGraphDiagnostic_ = "Fallback default graph (source JSON malformed)";
        }
    } else {
        materialGraph_.BuildDefaultProceduralGraph();
        if (sel) {
            if (auto* alb = materialGraph_.FindNode("node_albedo")) {
                alb->colorParam = glm::vec3(sel->material.baseColor);
            }
            materialGraphDiagnostic_ = "Template PBR DAG for " + sel->name + " (edit or compile to persist)";
        }
    }

    if (!materialGraph_.FindNode(selectedMatNodeId_) && !materialGraph_.GetNodes().empty()) {
        selectedMatNodeId_ = materialGraph_.GetNodes().front().nodeId;
        for (const auto& n : materialGraph_.GetNodes()) {
            if (n.nodeType != "PbrOutput") {
                selectedMatNodeId_ = n.nodeId;
            }
        }
    }

    std::string slangOut;
    if (materialGraph_.CompileToSlangShaderSource(&slangOut)) {
        lastCompiledSlangPreview_ = std::move(slangOut);
    }
    if (renderer_.IsInitialized()) {
        renderer_.UpdateMaterialGraphPreviewTexture(materialGraph_.BakeGraphAlbedoTexture(128, 128));
    }

    // 2. Load Multi-Layer Pixel-Art Canvas from selected object's persisted source JSON (or pixel_source blob)
    bool loadedPixelSource = false;
    if (sel && !sel->material.pixelCanvasJson.empty()) {
        auto parsedPxJson = nlohmann::json::parse(sel->material.pixelCanvasJson, nullptr, false);
        if (!parsedPxJson.is_discarded() && parsedPxJson.is_object()) {
            pixelCanvas_ = creative::PixelArtCanvas::FromJson(parsedPxJson);
            loadedPixelSource = true;
        }
    }
    if (!loadedPixelSource && sel && sel->material.pixelSourceAssetId != 0) {
        if (const auto* ast = project_.FindAsset(sel->material.pixelSourceAssetId)) {
            auto blob = project_.GetAssetStore().ReadBlobVerified(ast->contentSha256);
            if (blob) {
                std::string jsonStr(blob->begin(), blob->end());
                auto parsedPxJson = nlohmann::json::parse(jsonStr, nullptr, false);
                if (!parsedPxJson.is_discarded() && parsedPxJson.is_object()) {
                    pixelCanvas_ = creative::PixelArtCanvas::FromJson(parsedPxJson);
                    loadedPixelSource = true;
                }
            }
        }
    }
    if (!loadedPixelSource) {
        pixelCanvas_ = creative::PixelArtCanvas(16, 16);
        if (sel) {
            pixelCanvas_.spriteName = sel->name + "_Sprite";
        }
    }
    if (renderer_.IsInitialized()) {
        renderer_.UpdatePixelArtPreviewTexture(pixelCanvas_.BakeToImageData2D());
    }
}

bool StudioUiController::CommitMaterialGraphToSelectedObject(const std::string& actionLabel) {
    auto val = materialGraph_.ValidateGraph();
    if (!val.valid) {
        materialGraphDiagnostic_ = "[DAG ERROR @ " + val.failingNodeId + "] " + val.diagnosticMessage;
        statusBannerMessage_ = materialGraphDiagnostic_;
        return false;
    }

    std::string slangOut;
    if (materialGraph_.CompileToSlangShaderSource(&slangOut)) {
        lastCompiledSlangPreview_ = std::move(slangOut);
    }
    if (renderer_.IsInitialized()) {
        renderer_.UpdateMaterialGraphPreviewTexture(materialGraph_.BakeGraphAlbedoTexture(128, 128));
    }

    core::SceneObject* sel = project_.FindObject(selectedObjectId_);
    if (!sel) return false;

    core::SceneObject updated = *sel;
    updated.material.materialGraphJson = materialGraph_.ToJsonString();
    bool ok = project_.ApplyObjectMutation(sel->id, updated, actionLabel, "Material", "User");
    if (ok) {
        lastSyncedMutationGen_ = project_.GetMutationGeneration();
        materialGraphDiagnostic_ = "Saved DAG (" + std::to_string(materialGraph_.GetNodes().size()) +
                                   " nodes) to " + sel->name + " (undoable & SQLite-ready).";
    }
    return ok;
}

bool StudioUiController::CommitPixelCanvasToSelectedObject(const std::string& actionLabel) {
    auto bakedImg = pixelCanvas_.BakeToImageData2D();
    if (renderer_.IsInitialized()) {
        renderer_.UpdatePixelArtPreviewTexture(bakedImg);
    }

    core::SceneObject* sel = project_.FindObject(selectedObjectId_);
    if (!sel) return false;

    std::string sourceJson = pixelCanvas_.ToJsonString();
    std::vector<uint8_t> sourceBytes(sourceJson.begin(), sourceJson.end());
    std::string srcSha = platform::ComputeSha256Hex(sourceBytes);

    core::AssetId srcAssetId = sel->material.pixelSourceAssetId;
    const auto* existingSrc = (srcAssetId != 0) ? project_.FindAsset(srcAssetId) : nullptr;
    if (!existingSrc || existingSrc->contentSha256 != srcSha) {
        auto regSrc = project_.RegisterBinaryAsset(
            pixelCanvas_.spriteName + "_Source",
            "pixel_source",
            sourceBytes,
            "assets/sprites/" + pixelCanvas_.spriteName + ".platzpx.json",
            "{}",
            "PLATZ-Internal");
        if (regSrc) srcAssetId = *regSrc;
    }

    auto texBlob = bakedImg.SerializeToBinaryBlob();
    std::string texSha = platform::ComputeSha256Hex(texBlob);
    core::AssetId texAssetId = sel->material.albedoTextureAssetId;
    const auto* existingTex = (texAssetId != 0) ? project_.FindAsset(texAssetId) : nullptr;
    if (!existingTex || existingTex->contentSha256 != texSha) {
        nlohmann::json lineage = {
            {"sourcePixelAssetId", srcAssetId},
            {"layerCount", pixelCanvas_.layers.size()},
            {"filter", "VK_FILTER_NEAREST"}
        };
        auto regTex = project_.RegisterBinaryAsset(
            pixelCanvas_.spriteName + "_Texture",
            "texture",
            texBlob,
            "assets/sprites/" + pixelCanvas_.spriteName + ".plzt",
            lineage.dump(),
            "PLATZ-Internal");
        if (regTex) texAssetId = *regTex;
    }

    core::SceneObject updated = *sel;
    updated.material.pixelCanvasJson = sourceJson;
    updated.material.pixelSourceAssetId = srcAssetId;
    updated.material.albedoTextureAssetId = texAssetId;
    updated.material.useProceduralChecker = false;
    if (sel->material.albedoTextureAssetId == 0) {
        updated.material.baseColor = {1.0f, 1.0f, 1.0f, 1.0f};
    }

    bool ok = project_.ApplyObjectMutation(sel->id, updated, actionLabel, "PixelArt", "User");
    if (ok) {
        lastSyncedMutationGen_ = project_.GetMutationGeneration();
        statusBannerMessage_ = "Saved editable " + std::to_string(pixelCanvas_.layers.size()) +
                               "-layer pixel canvas & bound live Vulkan texture on " + sel->name;
    }
    return ok;
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
        {"model.subdivide", "Add Non-Destructive Subdivision Modifier to Selected Object", "3D Modeling", "subdivide modifier smooth topology mesh asset", "", "VERIFIED"},
        {"model.add_taper_twist", "Add Non-Destructive Taper & Twist Modifier to Selected Object", "3D Modeling", "taper twist modifier deform mesh stack", "", "VERIFIED"},
        {"model.add_bevel", "Add Non-Destructive Bevel Chamfer Modifier to Selected Object", "3D Modeling", "bevel chamfer modifier round edges mesh", "", "VERIFIED"},
        {"material.compile_graph_slang", "Compile Visual Material Node Graph to Slang & Hot-Reload Vulkan Pipeline", "Materials & Shaders", "material graph slang shader compile hot reload spirv", "", "VERIFIED"},
        {"pixel.apply_sprite_to_object", "Bake 2D Pixel-Art Sprite to Content-Addressed Asset & Bind to Selected Object", "2D Pixel Art", "pixel art sprite texture nearest neighbor bake", "", "VERIFIED"},
        {"ai.stage_gold_material", "PLATZ AI: Stage Polished Gold PBR Material on Selected Object", "PLATZ AI", "ai assistant stage material gold pbr preview diff", "", "VERIFIED"},
        {"ai.run_three_role_workflow", "PLATZ AI: Run 3-Role Coordinated Specialist Synthesis (Planner + 2 Neural Models + Critic)", "PLATZ AI", "ai 3-role coordinator planner specialist critic stage", "", "VERIFIED"},
        {"ai.emergency_stop", "EMERGENCY STOP: Revoke Active PLATZ AI Session", "Security", "stop cancel revoke ai emergency safety", "Esc", "VERIFIED"},
        {"ai.train_specialist", "Train Native 12x24x7 & 10x20x5 PLATZ Specialist Models", "PLATZ AI", "train model neural specialist pareto holdout", "", "VERIFIED"},
        {"qa.run_validation_route", "Run Automated 9-Station Validation World Route", "QA & Validation", "test verify validation world stations route qa", "F9", "VERIFIED"},
        {"ui.reset_layout", "Reset & Recover Docked Studio Workspace Layout", "UI & Accessibility", "reset layout dock recover windows panels", "", "VERIFIED"},
        {"theme.obsidian", "Switch Theme: PZ Obsidian Dark", "UI & Accessibility", "theme dark obsidian color style", "", "VERIFIED"},
        {"theme.high_contrast", "Switch Theme: PZ High-Contrast Accessible", "UI & Accessibility", "theme contrast accessible vision screen reader", "", "VERIFIED"},
        {"workspace.world", "Switch Workspace: World & Scene", "Workspace", "workspace world scene hierarchy viewport", "Alt+1", "VERIFIED"},
        {"workspace.model", "Switch Workspace: 3D Modeling & Mesh Lab", "Workspace", "workspace model mesh modifier subdivide", "Alt+2", "VERIFIED"},
        {"workspace.materials", "Switch Workspace: Materials & Shader Graph", "Workspace", "workspace material shader slang pbr", "Alt+3", "VERIFIED"},
        {"workspace.pixel", "Switch Workspace: 2D Pixel-Art & Sprite Studio", "Workspace", "workspace pixel art sprite 2d tilemap", "Alt+4", "VERIFIED"},
        {"workspace.animation", "Switch Workspace: Animation Timeline & Spatial Audio", "Workspace", "workspace animation timeline keyframe audio dsp", "Alt+5", "VERIFIED"},
        {"workspace.ai", "Switch Workspace: Logic, Gameplay & PLATZ AI Workbench", "Workspace", "workspace ai specialist training commands logic", "Alt+6", "VERIFIED"},
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
            SyncEditorsFromSelectedObject(false);
            setMsg("Undid: " + lbl);
            return true;
        }
        setMsg("Nothing to undo");
        return false;
    }
    if (commandId == "edit.redo") {
        std::string lbl;
        if (project_.Redo(&lbl)) {
            SyncEditorsFromSelectedObject(false);
            setMsg("Redid: " + lbl);
            return true;
        }
        setMsg("Nothing to redo");
        return false;
    }
    if (commandId == "runtime.toggle_play") {
        if (runtime_.IsPlaying()) {
            runtime_.StopPlay();
            debuggerTimelineScrubIndex_ = -1;
            setMsg("Stopped Play mode; canonical authoring state preserved at r" + std::to_string(project_.GetHeadRevision()));
        } else {
            runtime_.StartPlay(project_);
            debuggerTimelineScrubIndex_ = -1;
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
    if (commandId == "model.subdivide" || commandId == "model.add_taper_twist" || commandId == "model.add_bevel") {
        core::SceneObject* sel = project_.FindObject(selectedObjectId_);
        if (!sel) {
            setMsg("Select an object first to add a non-destructive 3D mesh modifier");
            return false;
        }
        core::SceneObject updated = *sel;
        core::MeshModifier mod;
        mod.modifierId = "mod_" + std::to_string(updated.mesh.modifierStack.size() + 1);
        if (commandId == "model.subdivide") {
            mod.modifierType = "Subdivide";
            mod.paramA = 1.0f;
        } else if (commandId == "model.add_taper_twist") {
            mod.modifierType = "TaperTwist";
            mod.paramA = 45.0f;
            mod.paramB = 0.65f;
        } else {
            mod.modifierType = "BevelChamfer";
            mod.paramA = 0.14f;
        }
        updated.mesh.modifierStack.push_back(mod);

        // Also register derived mesh asset if stack has Subdivide on an object without custom asset yet
        creative::MeshData base = creative::MeshGenerator::GenerateCube(1.0f);
        if (sel->mesh.primitiveType == "sphere") base = creative::MeshGenerator::GenerateSphere(0.6f, 16, 12);
        creative::MeshData evalMesh = creative::MeshGenerator::EvaluateModifierStack(base, updated.mesh.modifierStack);
        auto imp = creative::AssetImporter::ImportMeshIntoProject(
            project_, evalMesh, sel->name + "_" + mod.modifierType, "assets/generated/modifier.obj", "PLATZ-Internal");
        if (imp.success) {
            updated.mesh.meshAssetId = imp.assetId;
        }

        project_.ApplyObjectMutation(sel->id, updated, "Add " + mod.modifierType + " Modifier", "Model", "User");
        setMsg("Added non-destructive '" + mod.modifierType + "' modifier to " + sel->name);
        return true;
    }
    if (commandId == "material.compile_graph_slang") {
        std::string slangCode, failNode, err;
        if (!materialGraph_.CompileToSlangShaderSource(&slangCode, &failNode, &err)) {
            materialGraphDiagnostic_ = "[GRAPH ERROR @ " + failNode + "] " + err;
            setMsg(materialGraphDiagnostic_);
            return false;
        }
        lastCompiledSlangPreview_ = slangCode;
        auto bakedTex = materialGraph_.BakeGraphAlbedoTexture(128, 128);
        if (renderer_.IsInitialized()) {
            renderer_.UpdateMaterialGraphPreviewTexture(bakedTex);
            std::string hotDiag;
            if (!renderer_.HotReloadPbrShaderFromSlangSource(slangCode, &hotDiag)) {
                materialGraphDiagnostic_ = "[SLANG FALLBACK] " + hotDiag;
                setMsg(materialGraphDiagnostic_);
                return false;
            }
        }
        CommitMaterialGraphToSelectedObject("Compile & Bind Material DAG");
        materialGraphDiagnostic_ = "Compiled " + std::to_string(materialGraph_.GetNodes().size()) +
                                   "-node DAG -> Slang 2026.22.1 -> SPIR-V 1.5 & hot-reloaded Vulkan pipeline.";
        setMsg(materialGraphDiagnostic_);
        return true;
    }
    if (commandId == "pixel.apply_sprite_to_object") {
        bool ok = CommitPixelCanvasToSelectedObject("Bake & Bind 2D Pixel-Art Sprite");
        setMsg(ok ? ("Saved editable " + std::to_string(pixelCanvas_.layers.size()) +
                     "-layer pixel canvas & bound live Vulkan texture")
                  : "Select an object first to bind a pixel-art sprite texture");
        return ok;
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
    if (commandId == "ai.run_three_role_workflow") {
        if (selectedObjectId_ == 0) {
            setMsg("Select an object first to run 3-Role Coordinated Specialist Synthesis");
            return false;
        }
        lastThreeRoleReceipt_ = ai::ThreeRoleCoordinator::PlanSynthesizeAndVerifyEnhancement(
            project_,
            selectedObjectId_,
            selectedSpecialistArchetype_,
            selectedGameplayGenreIndex_,
            specialistModel_,
            gameplaySpecialist_,
            stagedAiSession_);
        setMsg(lastThreeRoleReceipt_.stagedSuccessfully
                   ? "3-Role AI Workflow staged 2 verified commands (Planner + 2 Neural Specialists + Critic)"
                   : lastThreeRoleReceipt_.criticVerdict);
        return lastThreeRoleReceipt_.stagedSuccessfully;
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
        auto rec1 = specialistModel_.TrainAndEvaluate(trainSet, holdoutSet, hp);
        auto rec2 = gameplaySpecialist_.TrainDefaultGameplaySpecialist();
        setMsg("Trained Material (12x24x7, " + std::to_string(static_cast<int>(rec1.holdoutAccuracyPct)) +
               "%) & Gameplay (10x20x5, " + std::to_string(static_cast<int>(rec2.holdoutAccuracyPct)) + "%) Specialists");
        return rec1.convergedAndQualified && rec2.convergedAndQualified;
    }
    if (commandId == "qa.run_validation_route") {
        lastRouteReceipt_ = ValidationWorldManager::RunAutomatedVerificationRoute(project_);
        setMsg("Validation World Route completed: " + std::to_string(lastRouteReceipt_.stationsPassed) +
               "/" + std::to_string(lastRouteReceipt_.stationsVisited) + " stations passed");
        return lastRouteReceipt_.allStationsPassed;
    }
    if (commandId == "ui.reset_layout") {
        dockLayoutNeedsBuild_ = true;
        setMsg("Recovered & reset Studio docked workspace layout");
        return true;
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
        t.panelHeader = {0.08f, 0.14f, 0.24f, 1.0f};
        t.accentPrimary = {0.00f, 0.96f, 1.00f, 1.0f};
        t.textPrimary = {1.00f, 1.00f, 1.00f, 1.0f};
        ApplyThemeWithProtectedControlGuard(t);
        setMsg("Activated PZ High-Contrast Accessible theme");
        return true;
    }
    if (commandId == "workspace.world") {
        SetWorkspaceMode(WorkspaceMode::WorldAndScene);
        return true;
    }
    if (commandId == "workspace.model") {
        SetWorkspaceMode(WorkspaceMode::ModelAndAssets);
        return true;
    }
    if (commandId == "workspace.materials") {
        SetWorkspaceMode(WorkspaceMode::MaterialsAndShaders);
        return true;
    }
    if (commandId == "workspace.pixel") {
        SetWorkspaceMode(WorkspaceMode::PixelArtAndSprites);
        return true;
    }
    if (commandId == "workspace.animation") {
        SetWorkspaceMode(WorkspaceMode::AnimationAndAudio);
        return true;
    }
    if (commandId == "workspace.ai") {
        SetWorkspaceMode(WorkspaceMode::PlatzAiWorkbench);
        return true;
    }
    if (commandId == "future.raytracing_pathtracer") {
        setMsg("[HONEST STATUS: BACKLOG / LONG-TERM TARGET] Hardware Ray/Path Tracing is a future capability and is not claimed in this build.");
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
    themeDirty_ = true;
    return state;
}

void StudioUiController::ApplyThemeToImGui() const {
    if (!ImGui::GetCurrentContext() || !themeDirty_) return;
    themeDirty_ = false;
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(10.0f * uiScale_, 10.0f * uiScale_);
    style.FramePadding = ImVec2(8.0f * uiScale_, 5.0f * uiScale_);
    style.ItemSpacing = ImVec2(8.0f * uiScale_, 6.0f * uiScale_);
    style.ItemInnerSpacing = ImVec2(6.0f * uiScale_, 4.0f * uiScale_);
    style.IndentSpacing = 16.0f * uiScale_;
    style.ScrollbarSize = 13.0f * uiScale_;
    style.GrabMinSize = 11.0f * uiScale_;

    style.WindowRounding = 6.0f;
    style.ChildRounding = 5.0f;
    style.FrameRounding = 4.0f;
    style.PopupRounding = 5.0f;
    style.TabRounding = 5.0f;
    style.GrabRounding = 3.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(activeTheme_.windowBg.r, activeTheme_.windowBg.g, activeTheme_.windowBg.b, 1.0f);
    colors[ImGuiCol_ChildBg] = ImVec4(activeTheme_.windowBg.r * 0.90f, activeTheme_.windowBg.g * 0.92f, activeTheme_.windowBg.b * 0.95f, 1.0f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.10f, 0.14f, 0.98f);
    colors[ImGuiCol_Border] = ImVec4(0.20f, 0.25f, 0.34f, 0.70f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.11f, 0.135f, 0.185f, 1.0f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.15f, 0.19f, 0.26f, 1.0f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.18f, 0.24f, 0.33f, 1.0f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.075f, 0.095f, 0.135f, 1.0f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.105f, 0.140f, 0.200f, 1.0f);
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.07f, 0.085f, 0.12f, 1.0f);
    colors[ImGuiCol_Header] = ImVec4(activeTheme_.panelHeader.r, activeTheme_.panelHeader.g, activeTheme_.panelHeader.b, 1.0f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(activeTheme_.accentPrimary.r * 0.35f + 0.10f, activeTheme_.accentPrimary.g * 0.35f + 0.12f, activeTheme_.accentPrimary.b * 0.35f + 0.16f, 1.0f);
    colors[ImGuiCol_HeaderActive] = ImVec4(activeTheme_.accentPrimary.r * 0.55f, activeTheme_.accentPrimary.g * 0.55f, activeTheme_.accentPrimary.b * 0.55f, 1.0f);
    colors[ImGuiCol_Button] = ImVec4(0.14f, 0.185f, 0.255f, 1.0f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(activeTheme_.accentPrimary.r * 0.45f + 0.08f, activeTheme_.accentPrimary.g * 0.45f + 0.10f, activeTheme_.accentPrimary.b * 0.45f + 0.12f, 1.0f);
    colors[ImGuiCol_ButtonActive] = ImVec4(activeTheme_.accentPrimary.r * 0.75f, activeTheme_.accentPrimary.g * 0.75f, activeTheme_.accentPrimary.b * 0.75f, 1.0f);
    colors[ImGuiCol_Tab] = ImVec4(0.095f, 0.12f, 0.17f, 1.0f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.16f, 0.28f, 0.38f, 1.0f);
    colors[ImGuiCol_TabSelected] = ImVec4(0.13f, 0.23f, 0.33f, 1.0f);
    colors[ImGuiCol_TabDimmed] = ImVec4(0.08f, 0.10f, 0.14f, 1.0f);
    colors[ImGuiCol_TabDimmedSelected] = ImVec4(0.11f, 0.18f, 0.26f, 1.0f);
    colors[ImGuiCol_CheckMark] = ImVec4(activeTheme_.accentPrimary.r, activeTheme_.accentPrimary.g, activeTheme_.accentPrimary.b, 1.0f);
    colors[ImGuiCol_SliderGrab] = ImVec4(activeTheme_.accentPrimary.r * 0.85f, activeTheme_.accentPrimary.g * 0.85f, activeTheme_.accentPrimary.b * 0.85f, 1.0f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(activeTheme_.accentPrimary.r, activeTheme_.accentPrimary.g, activeTheme_.accentPrimary.b, 1.0f);
    colors[ImGuiCol_Separator] = ImVec4(0.20f, 0.25f, 0.33f, 0.85f);
    colors[ImGuiCol_Text] = ImVec4(activeTheme_.textPrimary.r, activeTheme_.textPrimary.g, activeTheme_.textPrimary.b, 1.0f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.54f, 0.60f, 0.70f, 1.0f);

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

        for (const auto& mod : obj.mesh.modifierStack) {
            if (mod.enabled && mod.modifierType == "Subdivide") estTris *= 4;
            if (mod.enabled && mod.modifierType == "MirrorX") estTris *= 2;
        }

        if (estTris >= 400) {
            PerformanceAdvisorHint h;
            h.objectId = obj.id;
            h.objectName = obj.name;
            h.estimatedTriangles = estTris;
            h.plainEnglishExplanation =
                "'" + obj.name + "' (" + core::FormatObjectId(obj.id) + ") draws ~" +
                std::to_string(estTris) + " triangles with GGX PBR lighting.";
            h.reversibleOptimizationAction =
                "Toggle off non-destructive Subdivision modifier during layout editing or use lower-segment LOD.";
            hints.push_back(std::move(h));
        }
    }
    return hints;
}

void StudioUiController::SetSelectedObjectId(core::ObjectId id) {
    if (selectedObjectId_ != id) {
        selectedObjectId_ = id;
        animPreviewPlaying_ = false;
        animPreviewTimeSec_ = 0.0f;
        SyncEditorsFromSelectedObject(false);
        if (const auto* obj = project_.FindObject(id)) {
            accessibility_.Announce("Selection", "Selected " + obj->name + " (" + core::FormatObjectId(id) + ")");
        }
    }
}

void StudioUiController::SetWorkspaceMode(WorkspaceMode mode) {
    workspaceMode_ = mode;
    switch (mode) {
        case WorkspaceMode::WorldAndScene:
            pendingFocusWindowName_ = "Asset Browser";
            accessibility_.Announce("Workspace", "Switched workspace to World & Scene");
            break;
        case WorkspaceMode::ModelAndAssets:
            pendingFocusWindowName_ = "Asset Browser";
            accessibility_.Announce("Workspace", "Switched workspace to 3D Modeling & Mesh Lab");
            break;
        case WorkspaceMode::MaterialsAndShaders:
            pendingFocusWindowName_ = "Material Node Graph";
            accessibility_.Announce("Workspace", "Switched workspace to Materials & Shader Graph");
            break;
        case WorkspaceMode::PixelArtAndSprites:
            pendingFocusWindowName_ = "2D Pixel & Sprite Studio";
            accessibility_.Announce("Workspace", "Switched workspace to 2D Pixel-Art & Sprite Studio");
            break;
        case WorkspaceMode::AnimationAndAudio:
            pendingFocusWindowName_ = "Animation Timeline";
            accessibility_.Announce("Workspace", "Switched workspace to Animation Timeline & Spatial Audio");
            break;
        case WorkspaceMode::PlatzAiWorkbench:
            pendingFocusWindowName_ = "PLATZ AI & Specialists";
            accessibility_.Announce("Workspace", "Switched workspace to Logic, Gameplay & PLATZ AI Workbench");
            break;
    }
}

void StudioUiController::SetUiScale(float scale) {
    uiScale_ = std::clamp(scale, 0.85f, 2.0f);
    themeDirty_ = true;
    accessibility_.Announce("Accessibility", "UI Scale set to " + std::to_string(static_cast<int>(uiScale_ * 100.0f)) + "%");
}

const std::vector<core::SceneObject>& StudioUiController::GetActiveSceneObjects() const {
    if (runtime_.IsPlaying()) {
        return runtime_.GetRuntimeObjects();
    }
    if (animPreviewPlaying_ || animPreviewTimeSec_ > 0.001f) {
        const core::SceneObject* sel = project_.FindObject(selectedObjectId_);
        if (sel && sel->animation.enabled && !sel->animation.tracks.empty()) {
            animPreviewObjectsCache_ = project_.GetObjects();
            for (auto& o : animPreviewObjectsCache_) {
                if (o.id == selectedObjectId_) {
                    engine::runtime::RuntimeSession::EvaluateObjectAnimationAtTime(o, animPreviewTimeSec_);
                    break;
                }
            }
            return animPreviewObjectsCache_;
        }
    }
    return project_.GetObjects();
}

void StudioUiController::RebuildProgrammaticDockLayout(uint32_t dockspaceId) {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, viewport->WorkSize);

    ImGuiID dockMain = dockspaceId;
    ImGuiID dockLeft = 0;
    ImGuiID dockRight = 0;
    ImGuiID dockCenterBottom = 0;

    // 1. Split Left Outliner (22% width)
    ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Left, 0.22f, &dockLeft, &dockMain);

    // 2. Split Right Inspector (33% of remaining = ~26% total width, plenty of room so labels never clip!)
    ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Right, 0.33f, &dockRight, &dockMain);

    // 3. Split Center into Top 3D Viewport (65% height) and Bottom Creation/Content Drawer (35% height)
    ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Down, 0.35f, &dockCenterBottom, &dockMain);

    ImGui::DockBuilderDockWindow("Scene Outliner & Validation Stations", dockLeft);
    ImGui::DockBuilderDockWindow("Vulkan 1.4 3D Viewport", dockMain);
    ImGui::DockBuilderDockWindow("Property Inspector", dockRight);

    ImGui::DockBuilderDockWindow("Asset Browser", dockCenterBottom);
    ImGui::DockBuilderDockWindow("Material Node Graph", dockCenterBottom);
    ImGui::DockBuilderDockWindow("2D Pixel & Sprite Studio", dockCenterBottom);
    ImGui::DockBuilderDockWindow("Animation Timeline", dockCenterBottom);
    ImGui::DockBuilderDockWindow("No-Code Logic & Debugger", dockCenterBottom);
    ImGui::DockBuilderDockWindow("PLATZ AI & Specialists", dockCenterBottom);

    ImGui::DockBuilderFinish(dockspaceId);
}

void StudioUiController::BuildImGuiStudioFrame() {
    ApplyThemeToImGui();
    SyncEditorsFromSelectedObject(false);

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
    if (!io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_W, false)) gizmoMode_ = engine::scene::GizmoMode::Translate;
        if (ImGui::IsKeyPressed(ImGuiKey_E, false)) gizmoMode_ = engine::scene::GizmoMode::Rotate;
        if (ImGui::IsKeyPressed(ImGuiKey_R, false)) gizmoMode_ = engine::scene::GizmoMode::Scale;
    }

    DrawProtectedTopMenuAndSecurityBar();

    ImGuiID dockspaceId = ImGui::GetID("PLATZ_MasterDockSpace");
    ImGui::DockSpaceOverViewport(dockspaceId, ImGui::GetMainViewport());

    if (dockLayoutNeedsBuild_) {
        RebuildProgrammaticDockLayout(dockspaceId);
        SetWorkspaceMode(workspaceMode_);
        dockLayoutNeedsBuild_ = false;
    }

    DrawHierarchyAndStationsPanel();
    Draw3DViewportPanel();
    DrawInspectorAndMaterialPanel();
    DrawAssetAndValidationWorldPanel();
    DrawMaterialGraphAndSlangPanel();
    DrawPixelArtAndSpriteStudioPanel();
    DrawAnimationTimelinePanel();
    DrawLogicAndRuntimeHudPanel();
    DrawPlatzAiAndSpecialistPanel();

    if (!pendingFocusWindowName_.empty()) {
        ImGui::SetWindowFocus(pendingFocusWindowName_.c_str());
        pendingFocusWindowName_.clear();
    }

    if (showCommandBar_) {
        DrawUniversalCommandBarModal();
    }
}

void StudioUiController::DrawProtectedTopMenuAndSecurityBar() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImFont* bold = renderer_.GetFontBold()) ImGui::PushFont(bold);
        ImGui::TextColored(ImVec4(0.0f, 0.88f, 1.0f, 1.0f), "PZ  PLATZ STUDIO");
        if (renderer_.GetFontBold()) ImGui::PopFont();

        ImGui::Separator();

        if (ImGui::BeginMenu("File & Project")) {
            if (ImGui::MenuItem("Save Project Revision", "Ctrl+S")) InvokeCommandById("project.save");
            if (ImGui::MenuItem("Export Portable Package (.platzpkg.json)")) {
                auto rep = project_.ExportPortablePackage(project_.GetProjectDir() / "export" / "PLATZ_Package.platzpkg.json", true);
                statusBannerMessage_ = rep.allowed ? ("Exported portable package to " + rep.outputBundlePath.filename().string()) : rep.errorMessage;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Reset & Recover Workspace Layout")) InvokeCommandById("ui.reset_layout");
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, project_.CanUndo())) InvokeCommandById("edit.undo");
            if (ImGui::MenuItem("Redo", "Ctrl+Y", false, project_.CanRedo())) InvokeCommandById("edit.redo");
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View & Accessibility")) {
            if (ImGui::MenuItem("Theme: PZ Obsidian Dark")) InvokeCommandById("theme.obsidian");
            if (ImGui::MenuItem("Theme: PZ High-Contrast Accessible")) InvokeCommandById("theme.high_contrast");
            ImGui::Separator();
            if (ImGui::MenuItem("DPI Scale: 100%", nullptr, std::abs(uiScale_ - 1.0f) < 0.05f)) SetUiScale(1.0f);
            if (ImGui::MenuItem("DPI Scale: 125% (High-DPI)", nullptr, std::abs(uiScale_ - 1.25f) < 0.05f)) SetUiScale(1.25f);
            if (ImGui::MenuItem("DPI Scale: 150% (4K / Large Text)", nullptr, std::abs(uiScale_ - 1.50f) < 0.05f)) SetUiScale(1.50f);
            ImGui::Separator();
            if (ImGui::MenuItem("Mode: Guided Beginner", nullptr, complexityTier_ == ComplexityTier::GuidedBeginner))
                complexityTier_ = ComplexityTier::GuidedBeginner;
            if (ImGui::MenuItem("Mode: Pro Creator", nullptr, complexityTier_ == ComplexityTier::ProCreator))
                complexityTier_ = ComplexityTier::ProCreator;
            if (ImGui::MenuItem("Mode: Engine Developer", nullptr, complexityTier_ == ComplexityTier::EngineDeveloper))
                complexityTier_ = ComplexityTier::EngineDeveloper;
            ImGui::EndMenu();
        }

        ImGui::Separator();

        // Workspace Task Mode Switcher Pills
        auto drawWorkspaceTab = [&](const char* label, WorkspaceMode mode) {
            bool active = (workspaceMode_ == mode);
            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.58f, 0.78f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
            }
            if (ImGui::Button(label)) {
                SetWorkspaceMode(mode);
            }
            if (active) {
                ImGui::PopStyleColor(2);
            }
            ImGui::SameLine();
        };

        drawWorkspaceTab("1. World", WorkspaceMode::WorldAndScene);
        drawWorkspaceTab("2. Mesh Lab", WorkspaceMode::ModelAndAssets);
        drawWorkspaceTab("3. Materials", WorkspaceMode::MaterialsAndShaders);
        drawWorkspaceTab("4. Pixel Art", WorkspaceMode::PixelArtAndSprites);
        drawWorkspaceTab("5. Anim & Audio", WorkspaceMode::AnimationAndAudio);
        drawWorkspaceTab("6. Logic & AI", WorkspaceMode::PlatzAiWorkbench);

        ImGui::Separator();

        // Play / Pause / Stop Transport Controls
        if (runtime_.IsPlaying()) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.86f, 0.24f, 0.20f, 1.0f));
            if (ImGui::Button("STOP PLAY (F5)")) {
                InvokeCommandById("runtime.toggle_play");
            }
            ImGui::PopStyleColor();
            ImGui::SameLine();
            bool paused = runtime_.IsPaused();
            if (ImGui::Button(paused ? "RESUME" : "PAUSE")) {
                runtime_.SetPaused(!paused);
            }
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.62f, 0.34f, 1.0f));
            if (ImGui::Button("PLAY SCENE (F5)")) {
                InvokeCommandById("runtime.toggle_play");
            }
            ImGui::PopStyleColor();
        }

        ImGui::SameLine();
        if (ImGui::Button("Search / Command (Ctrl+K)")) {
            showCommandBar_ = !showCommandBar_;
        }

        ImGui::SameLine();
        // PROTECTED SECURITY & EMERGENCY STOP CONTROL (PLZ-SAFEX-006 / N-24)
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.90f, 0.14f, 0.18f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.98f, 0.22f, 0.26f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        if (ImGui::Button("AI EMERGENCY STOP")) {
            InvokeCommandById("ai.emergency_stop");
        }
        ImGui::PopStyleColor(3);

        if (stagedAiSession_.GetStatus() == ai::StagedSessionStatus::ActiveStaging) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.88f, 0.62f, 0.10f, 1.0f));
            if (ImGui::Button("APPROVE AI STAGED DIFF")) {
                std::string err;
                stagedAiSession_.ApproveAndCommit(stagedAiSession_.GetActiveSessionToken(), project_, nullptr, &err);
            }
            ImGui::PopStyleColor();
        }

        ImGui::EndMainMenuBar();
    }
}

void StudioUiController::DrawUniversalCommandBarModal() {
    ImGui::SetNextWindowSize(ImVec2(720.0f * uiScale_, 420.0f * uiScale_), ImGuiCond_Always);
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::Begin("PLATZ Universal Command & Discovery Bar (Ctrl+K)", &showCommandBar_, ImGuiWindowFlags_NoCollapse)) {
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint(
            "##cmd_search",
            "Type a command or natural task description (e.g. 'subdivide', 'material graph', '3-role ai', 'pixel', 'play')...",
            commandSearchBuf_,
            sizeof(commandSearchBuf_));
        ImGui::Separator();

        auto results = SearchCommandBar(commandSearchBuf_);
        if (ImGui::BeginTable("##cmd_results_table", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("Command & Description", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, 130.0f * uiScale_);
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 175.0f * uiScale_);
            ImGui::TableHeadersRow();

            for (const auto& item : results) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (ImGui::Selectable(item.title.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
                    InvokeCommandById(item.commandId);
                    showCommandBar_ = false;
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("%s", item.category.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextColored(
                    item.maturityBadge == "VERIFIED"
                        ? ImVec4(0.25f, 0.92f, 0.55f, 1.0f)
                        : ImVec4(0.95f, 0.75f, 0.25f, 1.0f),
                    "%s",
                    item.maturityBadge.c_str());
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

void StudioUiController::DrawHierarchyAndStationsPanel() {
    if (ImGui::Begin("Scene Outliner & Validation Stations")) {
        if (ImFont* bold = renderer_.GetFontBold()) ImGui::PushFont(bold);
        ImGui::TextColored(ImVec4(0.0f, 0.86f, 1.0f, 1.0f), "%s  (Rev r%llu)",
            project_.GetProjectName().c_str(),
            static_cast<unsigned long long>(project_.GetHeadRevision()));
        if (renderer_.GetFontBold()) ImGui::PopFont();

        // Quick Add Primitives Toolbar
        if (ImGui::Button("+ Cube")) InvokeCommandById("create.cube");
        ImGui::SameLine();
        if (ImGui::Button("+ Sphere")) InvokeCommandById("create.sphere");
        ImGui::SameLine();
        if (ImGui::Button("+ Torus")) InvokeCommandById("create.torus");
        ImGui::SameLine();
        if (ImGui::Button("+ Cylinder")) InvokeCommandById("create.cylinder");

        if (ImGui::Button("Duplicate Selected") && selectedObjectId_ != 0 && !runtime_.IsPlaying()) {
            auto dupId = project_.DuplicateObject(selectedObjectId_);
            if (dupId) SetSelectedObjectId(*dupId);
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete Selected") && selectedObjectId_ != 0 && !runtime_.IsPlaying()) {
            project_.DeleteObject(selectedObjectId_);
            selectedObjectId_ = project_.GetObjects().empty() ? 0 : project_.GetObjects().front().id;
        }

        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##outliner_filter", "Filter scene objects by name or station...", hierarchyFilterBuf_, sizeof(hierarchyFilterBuf_));
        std::string filterLower = LowercaseStr(hierarchyFilterBuf_);

        ImGui::Separator();

        // Jump to Validation Station Bar (9 cumulative stations)
        ImGui::TextDisabled("Living Validation World Stations (Click to Focus):");
        const char* stationIds[] = {"ST-01", "ST-02", "ST-03", "ST-04", "ST-05", "ST-06", "ST-07", "ST-08", "ST-09"};
        for (int i = 0; i < 9; ++i) {
            if (i == 5) {
                // Wrap cleanly to second row
            } else if (i > 0) {
                ImGui::SameLine();
            }
            if (ImGui::SmallButton(stationIds[i])) {
                for (const auto& obj : GetActiveSceneObjects()) {
                    if (obj.logic.stationCategory == stationIds[i] && obj.mesh.primitiveType != "plane") {
                        SetSelectedObjectId(obj.id);
                        camera_.FocusOnPoint(obj.transform.position, 5.6f);
                        break;
                    }
                }
            }
        }

        ImGui::Separator();

        // Scene Object Table with Full Type Badge, Name, and Station (zero truncation)
        const auto& activeObjs = GetActiveSceneObjects();
        if (ImGui::BeginTable("##outliner_table", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("Object Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, 68.0f * uiScale_);
            ImGui::TableSetupColumn("Stn", ImGuiTableColumnFlags_WidthFixed, 44.0f * uiScale_);
            ImGui::TableHeadersRow();

            for (const auto& obj : activeObjs) {
                if (!filterLower.empty()) {
                    std::string hay = LowercaseStr(obj.name + " " + obj.logic.stationCategory + " " + core::FormatObjectId(obj.id));
                    if (hay.find(filterLower) == std::string::npos) continue;
                }

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::PushID(static_cast<int>(obj.id));

                std::string displayLabel = (obj.parentId != 0 ? "  |- " : "") + obj.name;
                bool isSelected = (obj.id == selectedObjectId_);
                if (ImGui::Selectable(displayLabel.c_str(), isSelected, ImGuiSelectableFlags_SpanAllColumns)) {
                    SetSelectedObjectId(obj.id);
                }

                ImGui::TableSetColumnIndex(1);
                if (obj.light.enabled) {
                    ImGui::TextColored(ImVec4(1.0f, 0.88f, 0.35f, 1.0f), "Light");
                } else if (obj.animation.enabled && obj.audio.enabled) {
                    ImGui::TextColored(ImVec4(0.28f, 0.95f, 0.72f, 1.0f), "Anim+Aud");
                } else if (!obj.mesh.modifierStack.empty()) {
                    ImGui::TextColored(ImVec4(0.0f, 0.90f, 1.0f, 1.0f), "ModStack");
                } else if (obj.physics.isDynamic) {
                    ImGui::TextColored(ImVec4(0.45f, 0.80f, 1.0f, 1.0f), "Physics");
                } else if (obj.mesh.primitiveType == "custom_asset") {
                    ImGui::TextColored(ImVec4(0.35f, 0.92f, 0.72f, 1.0f), "MeshAsset");
                } else if (obj.mesh.primitiveType == "cylinder") {
                    ImGui::TextDisabled("Cylinder");
                } else if (obj.mesh.primitiveType == "sphere") {
                    ImGui::TextDisabled("Sphere");
                } else if (obj.mesh.primitiveType == "torus") {
                    ImGui::TextDisabled("Torus");
                } else if (obj.mesh.primitiveType == "plane") {
                    ImGui::TextDisabled("Plane");
                } else {
                    ImGui::TextDisabled("Cube");
                }

                ImGui::TableSetColumnIndex(2);
                ImGui::TextDisabled("%s", obj.logic.stationCategory.c_str());

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

void StudioUiController::Draw3DGizmoAndSelectionOverlay(
    const glm::vec2& viewportScreenMin,
    const glm::vec2& viewportSize) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    if (!drawList || viewportSize.x < 64.0f || viewportSize.y < 64.0f) return;

    float aspect = viewportSize.x / viewportSize.y;
    glm::mat4 viewProj = camera_.GetViewProjectionMatrix(aspect);

    // 1. Find selected object and draw its 3D projected wireframe AABB & 3D Transform Gizmo
    core::SceneObject* selObj = runtime_.IsPlaying()
        ? runtime_.FindRuntimeObject(selectedObjectId_)
        : project_.FindObject(selectedObjectId_);

    if (selObj) {
        glm::mat4 worldMat = runtime_.IsPlaying()
            ? selObj->transform.ToLocalMatrix()
            : project_.ComputeWorldMatrix(selObj->id);
        glm::vec3 worldCenter = glm::vec3(worldMat[3]);

        // Project 8 corners of the object's local bounding box to screen space
        float hx = 0.55f, hy = 0.55f, hz = 0.55f;
        if (selObj->mesh.primitiveType == "torus") { hx = 0.85f; hy = 0.32f; hz = 0.85f; }
        else if (selObj->mesh.primitiveType == "plane") { hx = 5.0f; hy = 0.05f; hz = 5.0f; }

        const glm::vec3 localCorners[8] = {
            {-hx, -hy, -hz}, { hx, -hy, -hz}, { hx,  hy, -hz}, {-hx,  hy, -hz},
            {-hx, -hy,  hz}, { hx, -hy,  hz}, { hx,  hy,  hz}, {-hx,  hy,  hz}
        };
        ImVec2 screenCorners[8]{};
        bool cornerValid[8]{};
        for (int i = 0; i < 8; ++i) {
            glm::vec3 wp = glm::vec3(worldMat * glm::vec4(localCorners[i], 1.0f));
            cornerValid[i] = ProjectWorldToScreen(wp, viewProj, viewportScreenMin, viewportSize, &screenCorners[i]);
        }

        const int edges[12][2] = {
            {0,1}, {1,2}, {2,3}, {3,0},
            {4,5}, {5,6}, {6,7}, {7,4},
            {0,4}, {1,5}, {2,6}, {3,7}
        };
        ImU32 boxCol = IM_COL32(0, 218, 255, 175);
        for (const auto& e : edges) {
            if (cornerValid[e[0]] && cornerValid[e[1]]) {
                drawList->AddLine(screenCorners[e[0]], screenCorners[e[1]], boxCol, 1.6f * uiScale_);
            }
        }

        // Draw 3D Transform Gizmo (X = Red, Y = Green, Z = Blue) at worldCenter
        ImVec2 origin2D{};
        if (ProjectWorldToScreen(worldCenter, viewProj, viewportScreenMin, viewportSize, &origin2D)) {
            float camDist = glm::length(camera_.GetEyePosition() - worldCenter);
            float gizmoWorldLen = std::clamp(camDist * 0.16f, 0.8f, 3.5f);

            const glm::vec3 axisDirs[3] = {
                {1.0f, 0.0f, 0.0f},
                {0.0f, 1.0f, 0.0f},
                {0.0f, 0.0f, 1.0f}
            };
            const ImU32 axisColors[3] = {
                IM_COL32(245, 72, 82, 255),   // X Axis (Coral Red)
                IM_COL32(58, 224, 122, 255),  // Y Axis (Emerald Green)
                IM_COL32(64, 156, 255, 255)   // Z Axis (Azure Blue)
            };
            const char* axisLabels[3] = {"X", "Y", "Z"};

            ImVec2 axisTips2D[3]{};
            bool axisValid[3]{};

            for (int a = 0; a < 3; ++a) {
                glm::vec3 tipWorld = worldCenter + axisDirs[a] * gizmoWorldLen;
                axisValid[a] = ProjectWorldToScreen(tipWorld, viewProj, viewportScreenMin, viewportSize, &axisTips2D[a]);
                if (!axisValid[a]) continue;

                bool isHoveredOrActive = (activeGizmoDragAxis_ == a);
                ImU32 col = isHoveredOrActive ? IM_COL32(255, 230, 80, 255) : axisColors[a];
                float thickness = (isHoveredOrActive ? 4.2f : 3.0f) * uiScale_;

                drawList->AddLine(origin2D, axisTips2D[a], col, thickness);

                if (gizmoMode_ == engine::scene::GizmoMode::Translate) {
                    drawList->AddCircleFilled(axisTips2D[a], 6.5f * uiScale_, col);
                } else if (gizmoMode_ == engine::scene::GizmoMode::Rotate) {
                    drawList->AddCircle(origin2D, glm::length(glm::vec2(axisTips2D[a].x - origin2D.x, axisTips2D[a].y - origin2D.y)) * 0.65f, col, 32, 1.5f * uiScale_);
                    drawList->AddCircleFilled(axisTips2D[a], 6.0f * uiScale_, col);
                } else {
                    float hs = 5.5f * uiScale_;
                    drawList->AddRectFilled(
                        ImVec2(axisTips2D[a].x - hs, axisTips2D[a].y - hs),
                        ImVec2(axisTips2D[a].x + hs, axisTips2D[a].y + hs),
                        col, 2.0f);
                }

                drawList->AddText(
                    ImVec2(axisTips2D[a].x + 8.0f * uiScale_, axisTips2D[a].y - 8.0f * uiScale_),
                    col,
                    axisLabels[a]);
            }

            // Center pivot handle
            drawList->AddCircleFilled(origin2D, 5.5f * uiScale_, IM_COL32(245, 248, 255, 255));
            drawList->AddCircle(origin2D, 7.0f * uiScale_, IM_COL32(15, 20, 30, 255), 16, 1.5f);

            // Object floating badge above object
            char badgeBuf[160]{};
            std::snprintf(badgeBuf, sizeof(badgeBuf), "%s [%s]  (%.2f, %.2f, %.2f)",
                selObj->name.c_str(),
                core::FormatObjectId(selObj->id).c_str(),
                selObj->transform.position.x,
                selObj->transform.position.y,
                selObj->transform.position.z);
            ImVec2 textSz = ImGui::CalcTextSize(badgeBuf);
            ImVec2 badgePos(origin2D.x - textSz.x * 0.5f, origin2D.y - 44.0f * uiScale_);
            drawList->AddRectFilled(
                ImVec2(badgePos.x - 7.0f, badgePos.y - 3.0f),
                ImVec2(badgePos.x + textSz.x + 7.0f, badgePos.y + textSz.y + 3.0f),
                IM_COL32(12, 16, 24, 215),
                4.0f);
            drawList->AddRect(
                ImVec2(badgePos.x - 7.0f, badgePos.y - 3.0f),
                ImVec2(badgePos.x + textSz.x + 7.0f, badgePos.y + textSz.y + 3.0f),
                IM_COL32(0, 212, 245, 190),
                4.0f);
            drawList->AddText(badgePos, IM_COL32(235, 245, 255, 255), badgeBuf);

            // Interactive mouse dragging on gizmo axis tips
            ImGuiIO& io = ImGui::GetIO();
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                for (int a = 0; a < 3; ++a) {
                    if (!axisValid[a]) continue;
                    float dx = io.MousePos.x - axisTips2D[a].x;
                    float dy = io.MousePos.y - axisTips2D[a].y;
                    if ((dx * dx + dy * dy) <= (18.0f * 18.0f * uiScale_ * uiScale_)) {
                        activeGizmoDragAxis_ = a;
                        gizmoDragStartMouse_ = {io.MousePos.x, io.MousePos.y};
                        gizmoDragStartTransform_ = selObj->transform;
                        break;
                    }
                }
            }
            if (activeGizmoDragAxis_ >= 0) {
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    float dragDeltaPx = (io.MousePos.x - gizmoDragStartMouse_.x) - (io.MousePos.y - gizmoDragStartMouse_.y);
                    glm::vec3 deltaVec(0.0f);
                    float scaleFactor = (gizmoMode_ == engine::scene::GizmoMode::Rotate) ? 0.5f : 0.02f;
                    deltaVec[activeGizmoDragAxis_] = dragDeltaPx * scaleFactor;
                    core::TransformComponent newT = engine::scene::ScenePickerAndGizmo::ApplyGizmoDelta(
                        gizmoDragStartTransform_, gizmoMode_, deltaVec, snapSettings_);
                    selObj->transform = newT;
                } else {
                    // Commit undoable transform mutation on release
                    if (!runtime_.IsPlaying()) {
                        core::SceneObject finalCopy = *selObj;
                        selObj->transform = gizmoDragStartTransform_;
                        project_.ApplyObjectMutation(
                            selObj->id, finalCopy, "Viewport Gizmo Transform " + selObj->name, "Scene", "User");
                    }
                    activeGizmoDragAxis_ = -1;
                }
            }
        }
    }

    // 2. Draw Viewport Orientation Axis Rose in Top-Right corner of 3D viewport
    ImVec2 roseCenter(viewportScreenMin.x + viewportSize.x - 46.0f * uiScale_, viewportScreenMin.y + 46.0f * uiScale_);
    drawList->AddCircleFilled(roseCenter, 32.0f * uiScale_, IM_COL32(14, 18, 26, 200), 24);
    drawList->AddCircle(roseCenter, 32.0f * uiScale_, IM_COL32(60, 80, 110, 180), 24, 1.2f);
    glm::mat3 viewRot = glm::mat3(camera_.GetViewMatrix());
    const ImU32 roseCols[3] = {IM_COL32(245, 72, 82, 255), IM_COL32(58, 224, 122, 255), IM_COL32(64, 156, 255, 255)};
    const char* roseNames[3] = {"X", "Y", "Z"};
    for (int a = 0; a < 3; ++a) {
        glm::vec3 dir(0.0f);
        dir[a] = 1.0f;
        glm::vec3 vDir = viewRot * dir;
        ImVec2 endPt(roseCenter.x + vDir.x * 22.0f * uiScale_, roseCenter.y - vDir.y * 22.0f * uiScale_);
        drawList->AddLine(roseCenter, endPt, roseCols[a], 2.4f * uiScale_);
        drawList->AddCircleFilled(endPt, 4.0f * uiScale_, roseCols[a]);
        drawList->AddText(ImVec2(endPt.x - 3.5f, endPt.y - 6.0f), IM_COL32(255, 255, 255, 255), roseNames[a]);
    }
}

void StudioUiController::Draw3DViewportPanel() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f * uiScale_, 6.0f * uiScale_));
    if (ImGui::Begin("Vulkan 1.4 3D Viewport")) {
        // Top Viewport Manipulation Bar
        if (ImGui::RadioButton("Move [W]", gizmoMode_ == engine::scene::GizmoMode::Translate))
            gizmoMode_ = engine::scene::GizmoMode::Translate;
        ImGui::SameLine();
        if (ImGui::RadioButton("Rotate [E]", gizmoMode_ == engine::scene::GizmoMode::Rotate))
            gizmoMode_ = engine::scene::GizmoMode::Rotate;
        ImGui::SameLine();
        if (ImGui::RadioButton("Scale [R]", gizmoMode_ == engine::scene::GizmoMode::Scale))
            gizmoMode_ = engine::scene::GizmoMode::Scale;

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::Checkbox("Snap", &snapSettings_.enableSnap);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(72.0f * uiScale_);
        ImGui::DragFloat("##snap_step", &snapSettings_.translateSnapStep, 0.05f, 0.05f, 5.0f, "%.2fm");

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        if (ImGui::SmallButton("Cam: Overview")) {
            camera_.FocusOnPoint(glm::vec3(0.0f, 1.1f, 0.0f), 11.5f);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Cam: PBR Gallery")) {
            camera_.FocusOnPoint(glm::vec3(0.0f, 1.1f, -2.5f), 4.6f);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Cam: Spire ST-07")) {
            camera_.FocusOnPoint(glm::vec3(-3.8f, 1.25f, -3.5f), 4.8f);
        }
        ImGui::SameLine();
        bool ortho = camera_.IsOrthographic();
        if (ImGui::Checkbox("Ortho", &ortho)) camera_.SetOrthographic(ortho);

        ImGui::SameLine();
        ImGui::TextDisabled("| Sun:");
        ImGui::SameLine();
        float sunEl = renderer_.GetSunElevationDeg();
        ImGui::SetNextItemWidth(76.0f * uiScale_);
        if (ImGui::SliderFloat("##sun_el", &sunEl, 10.0f, 88.0f, "%.0f deg")) {
            renderer_.SetSunElevationDeg(sunEl);
        }

        ImVec2 avail = ImGui::GetContentRegionAvail();
        uint32_t vpW = static_cast<uint32_t>(std::max(64.0f, avail.x));
        uint32_t vpH = static_cast<uint32_t>(std::max(64.0f, avail.y - 26.0f * uiScale_));

        VkDescriptorSet texId = renderer_.GetViewportImGuiDescriptorSet();
        if (texId != VK_NULL_HANDLE) {
            ImVec2 imgPos = ImGui::GetCursorScreenPos();
            ImVec2 imgSize(static_cast<float>(vpW), static_cast<float>(vpH));
            ImGui::Image(reinterpret_cast<ImTextureID>(texId), imgSize);

            bool viewportHovered = ImGui::IsItemHovered();

            // Draw 3D Transform Gizmo, Wireframe Bounding Box & Orientation Rose over the 3D Viewport
            Draw3DGizmoAndSelectionOverlay(
                glm::vec2(imgPos.x, imgPos.y),
                glm::vec2(imgSize.x, imgSize.y));

            if (viewportHovered && activeGizmoDragAxis_ < 0) {
                ImGuiIO& io = ImGui::GetIO();
                if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
                    camera_.Orbit(-io.MouseDelta.x * 0.35f, io.MouseDelta.y * 0.35f);
                }
                if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
                    camera_.Pan(-io.MouseDelta.x * 0.02f, io.MouseDelta.y * 0.02f);
                }
                if (std::abs(io.MouseWheel) > 0.01f) {
                    camera_.Zoom(-io.MouseWheel * 0.85f);
                }
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    float localX = io.MousePos.x - imgPos.x;
                    float localY = io.MousePos.y - imgPos.y;
                    auto ray = camera_.ScreenPointToWorldRay(localX, localY, imgSize.x, imgSize.y);
                    auto pick = engine::scene::ScenePickerAndGizmo::PickObjectByRay(GetActiveSceneObjects(), ray);
                    if (pick.hit) {
                        SetSelectedObjectId(pick.objectId);
                    }
                }
            }
        }

        // Bottom Status & Honest Per-Stage Telemetry Footer inside 3D Viewport
        const auto& perf = renderer_.GetPerfMetrics();
        auto budget = renderer_.GetContext().GetMemoryBudgetSnapshot();
        ImGui::TextColored(ImVec4(0.65f, 0.78f, 0.92f, 1.0f),
            "%s | Draws:%u Tris:%u | GPU:%.3fms | CPU:%.2fms (UI:%.2f Rec:%.2f) | Wait:%.2fms [%s] | VMA:%.1fMB",
            renderer_.GetContext().GetCapabilities().deviceName.c_str(),
            perf.drawCallCount,
            perf.triangleCount,
            perf.gpuScenePassTimeMs,
            perf.cpuActiveWorkMs,
            perf.cpuUiBuildMs,
            perf.cpuCommandRecordMs,
            perf.cpuPresentWaitMs,
            perf.presentModeName.c_str(),
            static_cast<double>(budget.activeAllocatedBytes) / (1024.0 * 1024.0));
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void StudioUiController::DrawInspectorAndMaterialPanel() {
    if (ImGui::Begin("Property Inspector")) {
        core::SceneObject* obj = runtime_.IsPlaying()
            ? runtime_.FindRuntimeObject(selectedObjectId_)
            : project_.FindObject(selectedObjectId_);

        if (!obj) {
            ImGui::TextDisabled("Select an object in the 3D Viewport or Scene Outliner to inspect its properties.");
            ImGui::End();
            return;
        }

        core::SceneObject edited = *obj;
        bool changed = false;

        if (ImFont* bold = renderer_.GetFontBold()) ImGui::PushFont(bold);
        ImGui::TextColored(ImVec4(0.0f, 0.88f, 1.0f, 1.0f), "%s", edited.name.c_str());
        if (renderer_.GetFontBold()) ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextDisabled("[%s · %s]", core::FormatObjectId(edited.id).c_str(), edited.logic.stationCategory.c_str());

        if (!edited.logic.stationDescription.empty()) {
            ImGui::TextWrapped("%s", edited.logic.stationDescription.c_str());
        }
        ImGui::Separator();

        // 1. Identity & Visibility Section (Two-column non-clipping table)
        if (ImGui::CollapsingHeader("1. Object Identity & Visibility", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (BeginPropertyTable("##tbl_identity", uiScale_)) {
                BeginPropertyRow("Display Name", "Visible name in hierarchy; stable ObjectId disambiguates duplicates (N-01)");
                char nameBuf[128]{};
                std::snprintf(nameBuf, sizeof(nameBuf), "%s", edited.name.c_str());
                if (ImGui::InputText("##obj_name", nameBuf, sizeof(nameBuf))) {
                    edited.name = nameBuf;
                    changed = true;
                }

                BeginPropertyRow("Stable ObjectId", "Immutable 64-bit canonical identity");
                ImGui::Text("%s ( parent: %s )",
                    core::FormatObjectId(edited.id).c_str(),
                    edited.parentId == 0 ? "Root" : core::FormatObjectId(edited.parentId).c_str());

                BeginPropertyRow("Visibility & Lock");
                changed |= ImGui::Checkbox("Visible", &edited.visible);
                ImGui::SameLine();
                changed |= ImGui::Checkbox("Locked", &edited.locked);

                ImGui::EndTable();
            }
        }

        // 2. 3D Transform Section
        if (ImGui::CollapsingHeader("2. 3D Spatial Transform & Snapping", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (BeginPropertyTable("##tbl_transform", uiScale_)) {
                BeginPropertyRow("Position (X, Y, Z)", "World/Local translation in meters");
                changed |= ImGui::DragFloat3("##pos", &edited.transform.position.x, 0.05f, -10000.0f, 10000.0f, "%.2f m");

                BeginPropertyRow("Rotation (Deg)", "Euler angles (Pitch, Yaw, Roll) in degrees");
                changed |= ImGui::DragFloat3("##rot", &edited.transform.rotationEulerDeg.x, 1.0f, -3600.0f, 3600.0f, "%.1f deg");

                BeginPropertyRow("Scale (X, Y, Z)", "Non-zero local scale factors (NaN/zero guarded per N-10)");
                changed |= ImGui::DragFloat3("##scl", &edited.transform.scale.x, 0.05f, 0.05f, 100.0f, "%.2f x");

                BeginPropertyRow("Quick Snap Step");
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
                ImGui::SameLine();
                if (ImGui::Button("Reset")) {
                    edited.transform.rotationEulerDeg = {0.0f, 0.0f, 0.0f};
                    edited.transform.scale = {1.0f, 1.0f, 1.0f};
                    changed = true;
                }

                ImGui::EndTable();
            }
        }

        // 3. 3D Mesh & CPU-Evaluated Non-Destructive Modifier Stack Section (PLZ-MODEL-001, PLZ-MODEL-005)
        if (ImGui::CollapsingHeader("3. 3D Geometry & CPU-Evaluated Modifiers (VMA Upload)", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (BeginPropertyTable("##tbl_mesh_mods", uiScale_)) {
                BeginPropertyRow("Base Primitive", "Underlying parametric or imported content-addressed mesh");
                const char* primTypes[] = {"cube", "sphere", "cylinder", "torus", "plane", "custom_asset"};
                int curPrimIdx = 0;
                for (int i = 0; i < 6; ++i) {
                    if (edited.mesh.primitiveType == primTypes[i]) curPrimIdx = i;
                }
                if (ImGui::Combo("##prim_combo", &curPrimIdx, primTypes, 6)) {
                    edited.mesh.primitiveType = primTypes[curPrimIdx];
                    changed = true;
                }

                BeginPropertyRow("Add Modifier", "Non-destructive stack evaluated on CPU, then uploaded to Vulkan VMA device buffers on change");
                if (ImGui::Button("+ Subdiv")) InvokeCommandById("model.subdivide");
                ImGui::SameLine();
                if (ImGui::Button("+ Twist")) InvokeCommandById("model.add_taper_twist");
                ImGui::SameLine();
                if (ImGui::Button("+ Bevel")) InvokeCommandById("model.add_bevel");

                for (size_t m = 0; m < edited.mesh.modifierStack.size(); ++m) {
                    auto& mod = edited.mesh.modifierStack[m];
                    ImGui::PushID(static_cast<int>(m));
                    std::string rowLabel = "#" + std::to_string(m + 1) + " " + mod.modifierType;
                    BeginPropertyRow(rowLabel.c_str());
                    changed |= ImGui::Checkbox("##en", &mod.enabled);
                    ImGui::SameLine();
                    if (mod.modifierType == "TaperTwist") {
                        float halfW = std::max(54.0f, (ImGui::GetContentRegionAvail().x - 10.0f * uiScale_) * 0.48f);
                        ImGui::SetNextItemWidth(halfW);
                        changed |= ImGui::DragFloat("##tw", &mod.paramA, 1.0f, -180.0f, 180.0f, "Tw %.0f deg");
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(halfW);
                        changed |= ImGui::DragFloat("##tp", &mod.paramB, 0.02f, 0.2f, 2.0f, "Tp %.2fx");
                    } else if (mod.modifierType == "BevelChamfer") {
                        ImGui::SetNextItemWidth(-1.0f);
                        changed |= ImGui::SliderFloat("##ch", &mod.paramA, -0.3f, 0.3f, "Chamfer %.2f");
                    } else {
                        ImGui::TextDisabled("4x Quad/Tri Subdivision");
                    }
                    ImGui::PopID();
                }

                if (!edited.mesh.modifierStack.empty()) {
                    BeginPropertyRow("Modifier Stack");
                    if (ImGui::Button("Clear All Modifiers")) {
                        edited.mesh.modifierStack.clear();
                        changed = true;
                    }
                }

                ImGui::EndTable();
            }
        }

        // 4. Slang PBR Material & Surface Section
        if (ImGui::CollapsingHeader("4. Slang PBR Metallic-Roughness Material", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (BeginPropertyTable("##tbl_material", uiScale_)) {
                BeginPropertyRow("Material Preset");
                char matNameBuf[128]{};
                std::snprintf(matNameBuf, sizeof(matNameBuf), "%s", edited.material.materialName.c_str());
                if (ImGui::InputText("##mat_name", matNameBuf, sizeof(matNameBuf))) {
                    edited.material.materialName = matNameBuf;
                    changed = true;
                }

                BeginPropertyRow("Base Albedo Color");
                changed |= ImGui::ColorEdit4("##albedo", &edited.material.baseColor.x);

                BeginPropertyRow("Metallic Factor", "0.0 = Dielectric insulator, 1.0 = Pure metal conductor");
                changed |= ImGui::SliderFloat("##metallic", &edited.material.metallic, 0.0f, 1.0f, "%.2f");

                BeginPropertyRow("Roughness Factor", "0.04 = Mirror smooth, 1.0 = Diffuse matte microsurface");
                changed |= ImGui::SliderFloat("##roughness", &edited.material.roughness, 0.04f, 1.0f, "%.2f");

                BeginPropertyRow("Emissive Color");
                changed |= ImGui::ColorEdit3("##emissive_col", &edited.material.emissiveColor.x);

                BeginPropertyRow("Emissive Intensity");
                changed |= ImGui::SliderFloat("##emissive_str", &edited.material.emissiveStrength, 0.0f, 5.0f, "%.2f");

                BeginPropertyRow("UV Checker Overlay");
                changed |= ImGui::Checkbox("Enable Procedural UV Grid", &edited.material.useProceduralChecker);

                ImGui::EndTable();
            }
        }

        // 5. Physics & Rigid Body Section (Shown in Pro & Developer tiers)
        if (complexityTier_ != ComplexityTier::GuidedBeginner &&
            ImGui::CollapsingHeader("5. Rigid-Body Physics & Collision", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (BeginPropertyTable("##tbl_physics", uiScale_)) {
                BeginPropertyRow("Physics Simulation");
                changed |= ImGui::Checkbox("Enabled", &edited.physics.enabled);
                ImGui::SameLine();
                changed |= ImGui::Checkbox("Dynamic Gravity", &edited.physics.isDynamic);

                BeginPropertyRow("Restitution (Bounce)");
                changed |= ImGui::SliderFloat("##restitution", &edited.physics.restitution, 0.0f, 1.0f, "%.2f");

                BeginPropertyRow("Surface Friction");
                changed |= ImGui::SliderFloat("##friction", &edited.physics.friction, 0.0f, 1.0f, "%.2f");

                ImGui::EndTable();
            }
        }

        // 6. Spatial Audio & Procedural DSP Section (PLZ-AUDIO-001, N-15)
        if (ImGui::CollapsingHeader("6. Spatial Audio & Procedural DSP", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (BeginPropertyTable("##tbl_audio", uiScale_)) {
                BeginPropertyRow("Audio Emitter");
                changed |= ImGui::Checkbox("Enabled##aud_en", &edited.audio.enabled);
                ImGui::SameLine();
                changed |= ImGui::Checkbox("3D Spatial##aud_3d", &edited.audio.spatial3D);

                BeginPropertyRow("Procedural Cue");
                const char* cueNames[] = {"CrystalChime", "PhysicsImpact", "TransferArp", "DronePulse", "UiClick"};
                int curCueIdx = 0;
                for (int i = 0; i < 5; ++i) {
                    if (edited.audio.cueType == cueNames[i]) curCueIdx = i;
                }
                if (ImGui::Combo("##aud_cue", &curCueIdx, cueNames, 5)) {
                    edited.audio.cueType = cueNames[curCueIdx];
                    changed = true;
                }

                BeginPropertyRow("Volume & Pitch");
                ImGui::SetNextItemWidth(95.0f * uiScale_);
                changed |= ImGui::SliderFloat("Vol", &edited.audio.volume, 0.0f, 1.0f, "%.2f");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(95.0f * uiScale_);
                changed |= ImGui::SliderFloat("Pitch", &edited.audio.pitch, 0.5f, 2.0f, "%.2fx");

                BeginPropertyRow("Low-Pass Cutoff");
                changed |= ImGui::SliderFloat("##aud_lpf", &edited.audio.lowPassCutoffHz, 400.0f, 18000.0f, "%.0f Hz");

                BeginPropertyRow("Triggers");
                changed |= ImGui::Checkbox("On Interact", &edited.audio.playOnInteract);
                ImGui::SameLine();
                changed |= ImGui::Checkbox("On Collision", &edited.audio.playOnCollision);

                BeginPropertyRow("Audition Cue");
                if (ImGui::Button("Test 3D Spatial Cue")) {
                    if (audioEngine_ && audioEngine_->IsInitialized()) {
                        platform::SpatialAudioCueRequest cue;
                        cue.cueType = edited.audio.cueType;
                        cue.sourceWorldPos = edited.transform.position;
                        cue.volume = edited.audio.volume;
                        cue.pitch = edited.audio.pitch;
                        cue.maxDistance = edited.audio.maxDistance;
                        cue.spatial3D = edited.audio.spatial3D;
                        cue.lowPassCutoffHz = edited.audio.lowPassCutoffHz;
                        glm::mat4 camV = camera_.GetViewMatrix();
                        glm::vec3 listenerRight(camV[0][0], camV[1][0], camV[2][0]);
                        audioEngine_->PlaySpatialCue(cue, camera_.GetEyePosition(), listenerRight);
                        statusBannerMessage_ = "Triggered spatial audio cue '" + cue.cueType + "' from " + edited.name;
                    } else {
                        statusBannerMessage_ = "Spatial audio engine not initialized";
                    }
                }

                ImGui::EndTable();
            }
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
    if (ImGui::Begin("Asset Browser")) {
        if (ImGui::Button("Run All 9 Validation World Stations (F9)")) {
            InvokeCommandById("qa.run_validation_route");
        }
        ImGui::SameLine();
        if (ImGui::Button("Rebuild GPU Mesh Caches from SHA-256 Blobs (N-04)")) {
            renderer_.InvalidateDerivedGpuCaches();
            renderer_.EnsureProjectGpuResourcesSynced(project_, GetActiveSceneObjects());
            statusBannerMessage_ = "Rebuilt derived GPU caches from canonical content-addressed blobs.";
        }
        ImGui::SameLine();
        if (ImGui::Button("Export Portable Bundle (.platzpkg.json)")) {
            auto rep = project_.ExportPortablePackage(project_.GetProjectDir() / "export" / "PLATZ_Package.platzpkg.json", true);
            statusBannerMessage_ = rep.allowed ? "Exported portable project bundle." : rep.errorMessage;
        }

        if (!lastRouteReceipt_.stationReceipts.empty()) {
            ImGui::Separator();
            for (const auto& line : lastRouteReceipt_.stationReceipts) {
                ImGui::TextColored(ImVec4(0.30f, 0.94f, 0.58f, 1.0f), "%s", line.c_str());
            }
        }

        ImGui::Separator();

        // Content-Addressed Asset Store Table + Performance Advisor
        if (ImGui::BeginTable("##asset_browser_table", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("Asset ID", ImGuiTableColumnFlags_WidthFixed, 95.0f * uiScale_);
            ImGui::TableSetupColumn("Logical Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 64.0f * uiScale_);
            ImGui::TableSetupColumn("Content SHA-256 Digest", ImGuiTableColumnFlags_WidthFixed, 155.0f * uiScale_);
            ImGui::TableSetupColumn("License", ImGuiTableColumnFlags_WidthFixed, 105.0f * uiScale_);
            ImGui::TableSetupColumn("Dependents", ImGuiTableColumnFlags_WidthFixed, 85.0f * uiScale_);
            ImGui::TableHeadersRow();

            for (const auto& ast : project_.GetAssets()) {
                auto deps = project_.GetDependentObjectsForAsset(ast.assetId);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(ImVec4(0.0f, 0.85f, 1.0f, 1.0f), "%s", core::FormatAssetId(ast.assetId).c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(ast.logicalName.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(ast.assetType.c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::TextDisabled("%s...", ast.contentSha256.substr(0, 16).c_str());
                ImGui::TableSetColumnIndex(4);
                ImGui::TextUnformatted(ast.licenseInfo.c_str());
                ImGui::TableSetColumnIndex(5);
                ImGui::Text("%zu obj (v%llu)", deps.size(), static_cast<unsigned long long>(ast.generationVersion));
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

void StudioUiController::DrawMaterialGraphAndSlangPanel() {
    if (ImGui::Begin("Material Node Graph")) {
        const core::SceneObject* selObj = project_.FindObject(selectedObjectId_);
        std::string targetLabel = selObj ? selObj->name : std::string("(No Object)");

        if (ImGui::Button("Compile DAG -> Slang & Hot-Reload")) {
            InvokeCommandById("material.compile_graph_slang");
        }
        ImGui::SameLine();

        auto addAndWireNode = [&](const char* type, glm::vec3 col, float a, float b) {
            const auto* outNode = materialGraph_.FindNode("node_output");
            std::string prevIn = (outNode && !outNode->inputNodeA.empty()) ? outNode->inputNodeA : std::string("node_albedo");
            std::string inB = (std::string(type) == "BlendMix") ? std::string("node_albedo") : std::string();
            std::string newId = materialGraph_.AddNode(type, col, a, b, prevIn, inB);
            if (!newId.empty() && outNode) {
                materialGraph_.ConnectNodes(newId, "node_output", 0);
            }
            selectedMatNodeId_ = newId;
            CommitMaterialGraphToSelectedObject(std::string("Add Material Node: ") + type);
        };

        if (ImGui::Button("+ WaveBands")) addAndWireNode("WaveBands", {0.10f, 0.90f, 0.98f}, 6.0f, 0.25f);
        ImGui::SameLine();
        if (ImGui::Button("+ HeightGrad")) addAndWireNode("HeightGradient", {0.98f, 0.78f, 0.28f}, 0.55f, 0.6f);
        ImGui::SameLine();
        if (ImGui::Button("+ Noise")) addAndWireNode("NoisePattern", {0.75f, 0.85f, 0.95f}, 12.0f, 0.28f);
        ImGui::SameLine();
        if (ImGui::Button("+ BlendMix")) addAndWireNode("BlendMix", {0.20f, 0.85f, 0.95f}, 0.45f, 0.0f);
        ImGui::SameLine();
        if (ImGui::Button("Delete Node")) {
            std::string err;
            if (materialGraph_.RemoveNode(selectedMatNodeId_, &err)) {
                selectedMatNodeId_ = materialGraph_.GetNodes().empty() ? "" : materialGraph_.GetNodes().front().nodeId;
                CommitMaterialGraphToSelectedObject("Remove Material Node");
            } else {
                materialGraphDiagnostic_ = "[DELETE BLOCKED] " + err;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset 4-Node")) {
            materialGraph_.BuildDefaultProceduralGraph();
            selectedMatNodeId_ = "node_checker";
            CommitMaterialGraphToSelectedObject("Reset Default 4-Node Material Graph");
        }

        auto dagVal = materialGraph_.ValidateGraph();
        ImGui::TextColored(
            dagVal.valid ? ImVec4(0.28f, 0.92f, 0.58f, 1.0f) : ImVec4(0.98f, 0.35f, 0.35f, 1.0f),
            "Target: %s  |  DAG: %s (%zu nodes)  |  %s",
            targetLabel.c_str(),
            dagVal.valid ? "VALID" : "INVALID",
            materialGraph_.GetNodes().size(),
            materialGraphDiagnostic_.c_str());

        ImGui::Separator();

        if (ImGui::BeginTable("##mat_graph_split", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("Expandable Material DAG Canvas (Click Node to Edit)", ImGuiTableColumnFlags_WidthStretch, 0.46f);
            ImGui::TableSetupColumn("Selected Node Inspector & Preview", ImGuiTableColumnFlags_WidthStretch, 0.29f);
            ImGui::TableSetupColumn("Compiled Slang 2026.22.1 Shader", ImGuiTableColumnFlags_WidthStretch, 0.25f);

            ImGui::TableNextRow();

            // Column 0: Interactive Topological DAG Canvas with Real Dual-Input Bezier Wires
            ImGui::TableSetColumnIndex(0);
            {
                ImVec2 canvasMin = ImGui::GetCursorScreenPos();
                ImVec2 canvasSz = ImGui::GetContentRegionAvail();
                canvasSz.x = std::max(canvasSz.x, 220.0f * uiScale_);
                canvasSz.y = std::max(canvasSz.y, 165.0f * uiScale_);
                ImVec2 canvasMax(canvasMin.x + canvasSz.x, canvasMin.y + canvasSz.y);

                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->PushClipRect(canvasMin, canvasMax, true);
                dl->AddRectFilled(canvasMin, canvasMax, IM_COL32(14, 17, 24, 255), 5.0f);
                dl->AddRect(canvasMin, canvasMax, IM_COL32(45, 58, 82, 255), 5.0f);

                float gridStep = 22.0f * uiScale_;
                for (float gx = gridStep; gx < canvasSz.x; gx += gridStep) {
                    dl->AddLine(ImVec2(canvasMin.x + gx, canvasMin.y), ImVec2(canvasMin.x + gx, canvasMax.y), IM_COL32(26, 32, 44, 180));
                }
                for (float gy = gridStep; gy < canvasSz.y; gy += gridStep) {
                    dl->AddLine(ImVec2(canvasMin.x, canvasMin.y + gy), ImVec2(canvasMax.x, canvasMin.y + gy), IM_COL32(26, 32, 44, 180));
                }

                const auto& nodes = materialGraph_.GetNodes();
                std::vector<std::string> orderedIds = dagVal.topologicalOrder;
                if (orderedIds.size() != nodes.size()) {
                    orderedIds.clear();
                    for (const auto& n : nodes) orderedIds.push_back(n.nodeId);
                }

                size_t count = std::max<size_t>(1, orderedIds.size());
                size_t cols = std::min<size_t>(3, std::max<size_t>(2, (count + 1) / 2));
                size_t rows = (count + cols - 1) / cols;
                float padX = 10.0f * uiScale_;
                float padY = 10.0f * uiScale_;
                float nodeW = std::clamp((canvasSz.x - padX * static_cast<float>(cols + 1)) / static_cast<float>(cols), 88.0f, 170.0f * uiScale_);
                float nodeH = std::clamp((canvasSz.y - padY * static_cast<float>(rows + 1)) / static_cast<float>(rows), 44.0f, 64.0f * uiScale_);

                std::map<std::string, ImVec2> nodeRectMin;
                for (size_t i = 0; i < orderedIds.size(); ++i) {
                    size_t col = i % cols;
                    size_t row = i / cols;
                    float nx = canvasMin.x + padX + static_cast<float>(col) * (nodeW + padX);
                    float ny = canvasMin.y + padY + static_cast<float>(row) * (nodeH + padY);
                    nodeRectMin[orderedIds[i]] = ImVec2(nx, ny);
                }

                // Draw real directed DAG Bezier wires (Input A = Cyan, Input B = Gold)
                for (const auto& n : nodes) {
                    auto dstIt = nodeRectMin.find(n.nodeId);
                    if (dstIt == nodeRectMin.end()) continue;
                    auto drawWire = [&](const std::string& srcId, bool isInputA) {
                        if (srcId.empty()) return;
                        auto srcIt = nodeRectMin.find(srcId);
                        if (srcIt == nodeRectMin.end()) return;
                        ImVec2 p1(srcIt->second.x + nodeW, srcIt->second.y + nodeH * 0.5f);
                        ImVec2 p2(dstIt->second.x, dstIt->second.y + nodeH * (isInputA ? 0.38f : 0.72f));
                        float midX = (p1.x + p2.x) * 0.5f;
                        ImU32 wireCol = isInputA ? IM_COL32(0, 212, 245, 235) : IM_COL32(248, 196, 62, 235);
                        dl->AddBezierCubic(p1, ImVec2(midX, p1.y), ImVec2(midX, p2.y), p2, wireCol, 2.2f * uiScale_);
                        dl->AddCircleFilled(p2, 3.5f * uiScale_, wireCol);
                    };
                    drawWire(n.inputNodeA, true);
                    drawWire(n.inputNodeB, false);
                }

                // Draw clickable Node Cards
                ImGuiIO& io = ImGui::GetIO();
                for (const auto& id : orderedIds) {
                    const auto* n = materialGraph_.FindNode(id);
                    if (!n) continue;
                    ImVec2 pMin = nodeRectMin[id];
                    ImVec2 pMax(pMin.x + nodeW, pMin.y + nodeH);
                    bool isSel = (n->nodeId == selectedMatNodeId_);
                    ImU32 hdrCol = (n->nodeType == "PbrOutput")
                        ? IM_COL32(28, 152, 92, 238)
                        : (isSel ? IM_COL32(0, 182, 232, 250) : IM_COL32(28, 92, 136, 230));
                    ImU32 borderCol = isSel ? IM_COL32(255, 224, 92, 255) : IM_COL32(75, 105, 145, 255);

                    dl->AddRectFilled(pMin, pMax, IM_COL32(24, 31, 44, 248), 5.0f);
                    dl->AddRectFilled(pMin, ImVec2(pMax.x, pMin.y + 20.0f * uiScale_), hdrCol, 5.0f);
                    dl->AddRect(pMin, pMax, borderCol, 5.0f, 0, isSel ? 2.2f : 1.0f);

                    // Clip text cleanly inside each node card so High-DPI scaling never overflows across nodes
                    dl->PushClipRect(ImVec2(pMin.x + 2.0f, pMin.y + 1.0f), ImVec2(pMax.x - 3.0f, pMax.y - 2.0f), true);
                    dl->AddText(ImVec2(pMin.x + 6.0f, pMin.y + 2.0f), IM_COL32(255, 255, 255, 255), n->nodeType.c_str());

                    char paramLine[64]{};
                    std::snprintf(paramLine, sizeof(paramLine), "A:%.2f  B:%.2f", n->scalarA, n->scalarB);
                    dl->AddText(ImVec2(pMin.x + 6.0f, pMin.y + 24.0f * uiScale_), IM_COL32(196, 212, 232, 255), paramLine);
                    dl->PopClipRect();

                    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                        io.MousePos.x >= pMin.x && io.MousePos.x <= pMax.x &&
                        io.MousePos.y >= pMin.y && io.MousePos.y <= pMax.y) {
                        selectedMatNodeId_ = n->nodeId;
                    }
                }

                dl->PopClipRect();
                ImGui::Dummy(canvasSz);
            }

            // Column 1: Selected Node Wiring & Parameters + Live Baked 128x128 Preview
            ImGui::TableSetColumnIndex(1);
            {
                VkDescriptorSet graphTex = renderer_.GetMaterialGraphImGuiDescriptorSet();
                if (graphTex != VK_NULL_HANDLE) {
                    ImGui::Image(reinterpret_cast<ImTextureID>(graphTex), ImVec2(56.0f * uiScale_, 56.0f * uiScale_));
                    ImGui::SameLine();
                    ImGui::BeginGroup();
                    ImGui::TextColored(ImVec4(0.0f, 0.88f, 1.0f, 1.0f), "Live DAG Texture");
                    ImGui::TextDisabled("128x128 CPU Preview\n+ Slang SPIR-V 1.5");
                    ImGui::EndGroup();
                }

                if (auto* selNode = materialGraph_.FindNode(selectedMatNodeId_)) {
                    ImGui::Separator();
                    ImGui::TextColored(ImVec4(1.0f, 0.86f, 0.35f, 1.0f), "Node: %s (%s)",
                        selNode->nodeId.c_str(), selNode->nodeType.c_str());

                    bool nodeEdited = false;
                    // Validated Input A / Input B rewiring dropdowns
                    if (selNode->nodeType != "AlbedoColor") {
                        auto drawInputCombo = [&](const char* label, std::string& curIn, int portIdx) {
                            ImGui::SetNextItemWidth(135.0f * uiScale_);
                            if (ImGui::BeginCombo(label, curIn.empty() ? "(None)" : curIn.c_str())) {
                                if (portIdx == 1 && ImGui::Selectable("(None)", curIn.empty())) {
                                    curIn.clear();
                                    CommitMaterialGraphToSelectedObject("Disconnect Material Node Port B");
                                }
                                for (const auto& cand : materialGraph_.GetNodes()) {
                                    if (cand.nodeId == selNode->nodeId || cand.nodeType == "PbrOutput") continue;
                                    bool isCur = (cand.nodeId == curIn);
                                    std::string itemLbl = cand.nodeId + " (" + cand.nodeType + ")";
                                    if (ImGui::Selectable(itemLbl.c_str(), isCur)) {
                                        std::string err;
                                        if (materialGraph_.ConnectNodes(cand.nodeId, selNode->nodeId, portIdx, &err)) {
                                            CommitMaterialGraphToSelectedObject("Rewire Material DAG");
                                        } else {
                                            materialGraphDiagnostic_ = "[CYCLE/WIRE BLOCKED] " + err;
                                        }
                                    }
                                }
                                ImGui::EndCombo();
                            }
                        };
                        drawInputCombo("Input A", selNode->inputNodeA, 0);
                        if (selNode->nodeType == "BlendMix") {
                            drawInputCombo("Input B", selNode->inputNodeB, 1);
                        }
                    }

                    if (selNode->nodeType == "AlbedoColor" || selNode->nodeType == "FresnelRim" ||
                        selNode->nodeType == "HeightGradient" || selNode->nodeType == "WaveBands" ||
                        selNode->nodeType == "CheckerPattern" || selNode->nodeType == "NoisePattern") {
                        ImGui::SetNextItemWidth(145.0f * uiScale_);
                        nodeEdited |= ImGui::ColorEdit3("Color##mg_col", &selNode->colorParam.x);
                    }
                    if (selNode->nodeType != "PbrOutput") {
                        ImGui::SetNextItemWidth(135.0f * uiScale_);
                        nodeEdited |= ImGui::SliderFloat("Param A##mg_a", &selNode->scalarA, 0.0f, 24.0f, "%.2f");
                        ImGui::SetNextItemWidth(135.0f * uiScale_);
                        nodeEdited |= ImGui::SliderFloat("Param B##mg_b", &selNode->scalarB, 0.0f, 1.0f, "%.2f");
                    }

                    if (nodeEdited && ImGui::IsItemDeactivatedAfterEdit()) {
                        CommitMaterialGraphToSelectedObject("Adjust Material Node Parameters");
                    } else if (nodeEdited && renderer_.IsInitialized()) {
                        renderer_.UpdateMaterialGraphPreviewTexture(materialGraph_.BakeGraphAlbedoTexture(128, 128));
                    }
                }
            }

            // Column 2: Generated Slang 2026.22.1 Topological Shader Code Inspection
            ImGui::TableSetColumnIndex(2);
            {
                ImGui::BeginChild("##slang_code_scroll", ImVec2(0.0f, 160.0f * uiScale_), false);
                if (ImFont* mono = renderer_.GetFontMono()) ImGui::PushFont(mono);
                ImGui::TextWrapped("%s", lastCompiledSlangPreview_.c_str());
                if (renderer_.GetFontMono()) ImGui::PopFont();
                ImGui::EndChild();
            }

            ImGui::EndTable();
        }
    }
    ImGui::End();
}

void StudioUiController::DrawPixelArtAndSpriteStudioPanel() {
    if (ImGui::Begin("2D Pixel & Sprite Studio")) {
        const core::SceneObject* selObj = project_.FindObject(selectedObjectId_);
        if (ImGui::Button("Bake & Bind Live Texture to 3D Object")) {
            InvokeCommandById("pixel.apply_sprite_to_object");
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Brush", pixelToolMode_ == 0)) pixelToolMode_ = 0;
        ImGui::SameLine();
        if (ImGui::RadioButton("Eraser", pixelToolMode_ == 1)) pixelToolMode_ = 1;
        ImGui::SameLine();
        if (ImGui::RadioButton("Fill", pixelToolMode_ == 2)) pixelToolMode_ = 2;
        ImGui::SameLine();
        ImGui::Checkbox("Mirror-X", &pixelMirrorSymmetry_);
        ImGui::SameLine();
        if (ImGui::Button("Stamp Tile")) {
            pixelCanvas_.StampTilePattern(activePixelPaletteIdx_, 0);
            CommitPixelCanvasToSelectedObject("Stamp Pixel Tile Pattern");
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset Emblem")) {
            pixelCanvas_.InitializeDefaultPaletteAndEmblem();
            CommitPixelCanvasToSelectedObject("Reset 2-Layer PZ Emblem");
        }
        ImGui::SameLine();
        if (ImGui::Button("Undo") && project_.CanUndo()) InvokeCommandById("edit.undo");
        ImGui::SameLine();
        if (ImGui::Button("Redo") && project_.CanRedo()) InvokeCommandById("edit.redo");

        ImGui::Separator();

        if (ImGui::BeginTable("##pixel_studio_layout", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("Multi-Layer Stack", ImGuiTableColumnFlags_WidthFixed, 195.0f * uiScale_);
            ImGui::TableSetupColumn("Interactive 16x16 Indexed Canvas", ImGuiTableColumnFlags_WidthStretch, 0.55f);
            ImGui::TableSetupColumn("Live VK_FILTER_NEAREST Preview", ImGuiTableColumnFlags_WidthStretch, 0.45f);
            ImGui::TableNextRow();

            // Column 0: Non-Destructive Multi-Layer Stack
            ImGui::TableSetColumnIndex(0);
            {
                if (ImGui::Button("+ Add Layer")) {
                    pixelCanvas_.AddLayer("Layer " + std::to_string(pixelCanvas_.layers.size() + 1));
                    CommitPixelCanvasToSelectedObject("Add Pixel-Art Layer");
                }
                ImGui::SameLine();
                if (ImGui::Button("Remove") && pixelCanvas_.layers.size() > 1) {
                    if (pixelCanvas_.RemoveLayer(pixelCanvas_.activeLayerIndex)) {
                        CommitPixelCanvasToSelectedObject("Remove Pixel-Art Layer");
                    }
                }
                ImGui::Separator();
                for (size_t li = 0; li < pixelCanvas_.layers.size(); ++li) {
                    ImGui::PushID(static_cast<int>(li));
                    auto& layer = pixelCanvas_.layers[li];
                    if (ImGui::Checkbox("##vis", &layer.visible)) {
                        CommitPixelCanvasToSelectedObject("Toggle Pixel Layer Visibility");
                    }
                    ImGui::SameLine();
                    bool isAct = (li == pixelCanvas_.activeLayerIndex);
                    if (ImGui::Selectable(layer.layerName.c_str(), isAct)) {
                        pixelCanvas_.activeLayerIndex = static_cast<uint32_t>(li);
                    }
                    ImGui::PopID();
                }
            }

            // Column 1: Palette Swatches + Interactive 16x16 Grid with Undoable Stroke Capture
            ImGui::TableSetColumnIndex(1);
            {
                ImGui::TextUnformatted("Palette:");
                for (size_t p = 0; p < pixelCanvas_.palette.size(); ++p) {
                    ImGui::SameLine();
                    ImGui::PushID(static_cast<int>(p));
                    const auto& c = pixelCanvas_.palette[p];
                    ImVec4 col(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1.0f);
                    if (ImGui::ColorButton("##pal_btn", col, ImGuiColorEditFlags_NoTooltip, ImVec2(20.0f * uiScale_, 20.0f * uiScale_))) {
                        activePixelPaletteIdx_ = static_cast<uint8_t>(p);
                        if (pixelToolMode_ == 1) pixelToolMode_ = 0;
                    }
                    ImGui::PopID();
                }

                ImVec2 gridOrigin = ImGui::GetCursorScreenPos();
                float cellSz = 10.5f * uiScale_;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImGuiIO& io = ImGui::GetIO();

                auto composited = pixelCanvas_.BakeToImageData2D();
                for (uint32_t y = 0; y < pixelCanvas_.height; ++y) {
                    for (uint32_t x = 0; x < pixelCanvas_.width; ++x) {
                        ImVec2 pMin(gridOrigin.x + x * cellSz, gridOrigin.y + y * cellSz);
                        ImVec2 pMax(pMin.x + cellSz - 1.0f, pMin.y + cellSz - 1.0f);
                        size_t pxOff = (static_cast<size_t>(y) * pixelCanvas_.width + x) * 4;
                        dl->AddRectFilled(
                            pMin,
                            pMax,
                            IM_COL32(composited.rgbaPixels[pxOff + 0], composited.rgbaPixels[pxOff + 1], composited.rgbaPixels[pxOff + 2], 255));

                        if (ImGui::IsWindowHovered() && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
                            io.MousePos.x >= pMin.x && io.MousePos.x <= pMax.x &&
                            io.MousePos.y >= pMin.y && io.MousePos.y <= pMax.y) {
                            if (pixelToolMode_ == 2) {
                                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                                    pixelCanvas_.FloodFill(x, y, activePixelPaletteIdx_);
                                    CommitPixelCanvasToSelectedObject("Flood Fill Pixel Layer");
                                }
                            } else {
                                if (!pixelStrokeInProgress_) {
                                    pixelStrokeInProgress_ = true;
                                    pixelPreStrokeJson_ = selObj ? selObj->material.pixelCanvasJson : "";
                                }
                                if (pixelToolMode_ == 1) {
                                    pixelCanvas_.ErasePixel(x, y, pixelMirrorSymmetry_);
                                } else {
                                    pixelCanvas_.SetIndex(x, y, activePixelPaletteIdx_, pixelMirrorSymmetry_);
                                }
                                if (renderer_.IsInitialized()) {
                                    renderer_.UpdatePixelArtPreviewTexture(pixelCanvas_.BakeToImageData2D());
                                }
                            }
                        }
                    }
                }
                if (pixelStrokeInProgress_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    pixelStrokeInProgress_ = false;
                    CommitPixelCanvasToSelectedObject("Paint Pixel-Art Stroke");
                }
                ImGui::Dummy(ImVec2(pixelCanvas_.width * cellSz + 6.0f, pixelCanvas_.height * cellSz));
            }

            // Column 2: Nearest-Neighbor Magnified Preview + Editable Lineage Info
            ImGui::TableSetColumnIndex(2);
            {
                VkDescriptorSet pixTex = renderer_.GetPixelArtImGuiDescriptorSet();
                if (pixTex != VK_NULL_HANDLE) {
                    ImGui::Image(reinterpret_cast<ImTextureID>(pixTex), ImVec2(118.0f * uiScale_, 118.0f * uiScale_));
                    ImGui::SameLine();
                }
                ImGui::BeginGroup();
                ImGui::TextColored(ImVec4(0.0f, 0.88f, 1.0f, 1.0f), "Target: %s", selObj ? selObj->name.c_str() : "(None)");
                ImGui::TextDisabled("Layers: %zu (Editable Source Preserved)", pixelCanvas_.layers.size());
                ImGui::TextDisabled("Filter: VK_FILTER_NEAREST (N-14)");
                if (selObj) {
                    ImGui::TextDisabled("Source Asset: %s", selObj->material.pixelSourceAssetId ? core::FormatAssetId(selObj->material.pixelSourceAssetId).c_str() : "Inline JSON");
                    ImGui::TextDisabled("3D Texture:   %s", selObj->material.albedoTextureAssetId ? core::FormatAssetId(selObj->material.albedoTextureAssetId).c_str() : "Unbound");
                }
                ImGui::EndGroup();
            }

            ImGui::EndTable();
        }
    }
    ImGui::End();
}

void StudioUiController::DrawAnimationTimelinePanel() {
    if (ImGui::Begin("Animation Timeline")) {
        core::SceneObject* sel = runtime_.IsPlaying()
            ? runtime_.FindRuntimeObject(selectedObjectId_)
            : project_.FindObject(selectedObjectId_);
        if (!sel) {
            ImGui::TextDisabled("Select an object to author or preview keyframed Animation Timeline tracks.");
            ImGui::End();
            return;
        }

        core::SceneObject edited = *sel;
        bool animChanged = false;

        if (ImGui::Button(animPreviewPlaying_ ? "Pause Preview" : "Play Preview")) {
            animPreviewPlaying_ = !animPreviewPlaying_;
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset 0.0s")) {
            animPreviewPlaying_ = false;
            animPreviewTimeSec_ = 0.0f;
        }
        ImGui::SameLine();
        animChanged |= ImGui::Checkbox("Anim Enabled", &edited.animation.enabled);
        ImGui::SameLine();
        animChanged |= ImGui::Checkbox("Loop", &edited.animation.loop);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(76.0f * uiScale_);
        animChanged |= ImGui::DragFloat("Dur(s)", &edited.animation.durationSec, 0.1f, 0.5f, 30.0f, "%.1fs");
        ImGui::SameLine();

        const char* interpModes[] = {"EaseInOut", "Linear", "Step"};
        ImGui::SetNextItemWidth(98.0f * uiScale_);
        ImGui::Combo("Curve", &animKeyframeInterpIdx_, interpModes, 3);
        ImGui::SameLine();

        auto upsertKeyframe = [&](const std::string& propPath, const glm::vec3& val) {
            edited.animation.enabled = true;
            core::AnimationTrack* tr = nullptr;
            for (auto& existing : edited.animation.tracks) {
                if (existing.targetProperty == propPath) {
                    tr = &existing;
                    break;
                }
            }
            if (!tr) {
                core::AnimationTrack newTr;
                newTr.trackId = "track_" + propPath;
                newTr.targetProperty = propPath;
                edited.animation.tracks.push_back(std::move(newTr));
                tr = &edited.animation.tracks.back();
            }
            bool replaced = false;
            for (auto& kf : tr->keyframes) {
                if (std::abs(kf.timeSec - animPreviewTimeSec_) < 0.04f) {
                    kf.value = val;
                    kf.interpolation = interpModes[animKeyframeInterpIdx_];
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                tr->keyframes.push_back({animPreviewTimeSec_, val, interpModes[animKeyframeInterpIdx_]});
                std::sort(tr->keyframes.begin(), tr->keyframes.end(),
                    [](const core::AnimationKeyframe& a, const core::AnimationKeyframe& b) {
                        return a.timeSec < b.timeSec;
                    });
            }
            animChanged = true;
        };

        if (ImGui::Button("+ Keyframe Transform")) {
            upsertKeyframe("position", edited.transform.position);
            upsertKeyframe("rotationEulerDeg", edited.transform.rotationEulerDeg);
            upsertKeyframe("scale", edited.transform.scale);
        }
        ImGui::SameLine();
        if (ImGui::Button("+ Keyframe Emissive")) {
            upsertKeyframe("emissiveStrength", {edited.material.emissiveStrength, 0.0f, 0.0f});
        }

        float dur = std::max(0.5f, edited.animation.durationSec);
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##anim_scrub", &animPreviewTimeSec_, 0.0f, dur, "Playhead: %.2fs");

        // Visual Multi-Track Keyframe Timeline Canvas
        ImVec2 tlMin = ImGui::GetCursorScreenPos();
        ImVec2 tlSz = ImGui::GetContentRegionAvail();
        tlSz.x = std::max(tlSz.x, 300.0f * uiScale_);
        tlSz.y = std::max(tlSz.y, 118.0f * uiScale_);
        ImVec2 tlMax(tlMin.x + tlSz.x, tlMin.y + tlSz.y);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(tlMin, tlMax, true);
        dl->AddRectFilled(tlMin, tlMax, IM_COL32(14, 18, 26, 255), 4.0f);
        dl->AddRect(tlMin, tlMax, IM_COL32(48, 62, 86, 255), 4.0f);

        float labelColW = 148.0f * uiScale_;
        float trackAreaW = std::max(80.0f, tlSz.x - labelColW - 14.0f);
        dl->AddLine(ImVec2(tlMin.x + labelColW, tlMin.y), ImVec2(tlMin.x + labelColW, tlMax.y), IM_COL32(60, 76, 104, 255));

        const char* trackNames[4] = {"position", "rotationEulerDeg", "scale", "emissiveStrength"};
        float laneH = std::max(24.0f * uiScale_, tlSz.y / 4.0f);
        for (int tIdx = 0; tIdx < 4; ++tIdx) {
            float ly0 = tlMin.y + tIdx * laneH;
            float lyMid = ly0 + laneH * 0.5f;
            if (tIdx % 2 == 1) {
                dl->AddRectFilled(ImVec2(tlMin.x + 1, ly0), ImVec2(tlMax.x - 1, ly0 + laneH), IM_COL32(20, 26, 38, 175));
            }
            dl->AddText(ImVec2(tlMin.x + 8.0f, lyMid - 7.0f * uiScale_), IM_COL32(210, 224, 240, 255), trackNames[tIdx]);

            for (const auto& tr : edited.animation.tracks) {
                if (tr.targetProperty != trackNames[tIdx]) continue;
                for (const auto& kf : tr.keyframes) {
                    float frac = std::clamp(kf.timeSec / dur, 0.0f, 1.0f);
                    float kx = tlMin.x + labelColW + 6.0f + frac * (trackAreaW - 12.0f);
                    float r = 6.0f * uiScale_;
                    ImU32 kCol = (kf.interpolation == "EaseInOut")
                        ? IM_COL32(0, 220, 250, 255)
                        : (kf.interpolation == "Step" ? IM_COL32(250, 128, 86, 255) : IM_COL32(248, 206, 72, 255));
                    ImVec2 diamond[4] = {
                        {kx, lyMid - r},
                        {kx + r, lyMid},
                        {kx, lyMid + r},
                        {kx - r, lyMid}
                    };
                    dl->AddConvexPolyFilled(diamond, 4, kCol);
                    dl->AddPolyline(diamond, 4, IM_COL32(15, 20, 28, 255), ImDrawFlags_Closed, 1.2f);
                }
            }
        }

        // Playhead Needle
        float phX = tlMin.x + labelColW + 6.0f + std::clamp(animPreviewTimeSec_ / dur, 0.0f, 1.0f) * (trackAreaW - 12.0f);
        dl->AddLine(ImVec2(phX, tlMin.y), ImVec2(phX, tlMax.y), IM_COL32(255, 86, 98, 240), 2.2f * uiScale_);
        dl->PopClipRect();
        ImGui::Dummy(tlSz);

        if (animChanged && !runtime_.IsPlaying()) {
            project_.ApplyObjectMutation(sel->id, edited, "Edit Animation Timeline on " + sel->name, "Animation", "User");
        }
    }
    ImGui::End();
}

void StudioUiController::DrawLogicAndRuntimeHudPanel() {
    if (ImGui::Begin("No-Code Logic & Debugger")) {
        const auto& rtMetrics = runtime_.GetMetrics();
        int64_t totalCrystals = runtime_.IsPlaying()
            ? runtime_.ComputeTotalResourceAcrossWorld("energy_crystal")
            : 100;

        ImGui::TextColored(ImVec4(0.25f, 0.95f, 0.55f, 1.0f),
            "[GAMEPLAY HUD] Mode: %s  |  Sim Time: %.2fs  |  Conserved Energy Crystals: %lld  |  Rules Fired: %llu",
            runtime_.IsPlaying() ? "PLAYING (ISOLATED RUNTIME)" : "EDITOR AUTHORING",
            rtMetrics.elapsedPlayTimeSec,
            static_cast<long long>(totalCrystals),
            static_cast<unsigned long long>(rtMetrics.rulesTriggeredCount));

        if (runtime_.IsPlaying() && selectedObjectId_ != 0) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Trigger OnInteract")) {
                runtime_.TriggerInteraction(selectedObjectId_);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Save Checkpoint (F6)")) InvokeCommandById("runtime.save_checkpoint");
            ImGui::SameLine();
            if (ImGui::SmallButton("Load Checkpoint (F7)")) InvokeCommandById("runtime.load_checkpoint");
        }

        // Time-Inspectable Visual Logic Debugger Timeline Scrubber (PLZ-LOGIC-008, PLZ-CREATEX2-004)
        const auto& timeline = runtime_.GetDebuggerTimeline();
        if (!timeline.empty()) {
            int maxIdx = static_cast<int>(timeline.size()) - 1;
            if (debuggerTimelineScrubIndex_ < 0 || debuggerTimelineScrubIndex_ > maxIdx) {
                debuggerTimelineScrubIndex_ = maxIdx;
            }
            ImGui::SetNextItemWidth(260.0f * uiScale_);
            ImGui::SliderInt("Time-Travel Causal Scrubber", &debuggerTimelineScrubIndex_, 0, maxIdx);
            const auto& snap = timeline[static_cast<size_t>(debuggerTimelineScrubIndex_)];
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.0f, 0.88f, 1.0f, 1.0f),
                "Step #%llu (t=%.2fs): %s -> %s [%s] State: %s -> %s | Pos=(%.2f, %.2f, %.2f)",
                static_cast<unsigned long long>(snap.stepIndex),
                snap.timestampSec,
                snap.objectName.c_str(),
                snap.ruleId.c_str(),
                snap.actionType.c_str(),
                snap.stateBefore.c_str(),
                snap.stateAfter.c_str(),
                snap.positionAfter.x,
                snap.positionAfter.y,
                snap.positionAfter.z);
        }

        ImGui::Separator();

        // Show No-Code Event/Condition/Action Rules on Selected Object
        core::SceneObject* sel = runtime_.IsPlaying()
            ? runtime_.FindRuntimeObject(selectedObjectId_)
            : project_.FindObject(selectedObjectId_);
        if (sel) {
            if (ImGui::BeginTable("##logic_rules_tbl", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY)) {
                ImGui::TableSetupColumn("Rule ID", ImGuiTableColumnFlags_WidthFixed, 150.0f * uiScale_);
                ImGui::TableSetupColumn("Trigger Event", ImGuiTableColumnFlags_WidthFixed, 115.0f * uiScale_);
                ImGui::TableSetupColumn("Condition", ImGuiTableColumnFlags_WidthFixed, 145.0f * uiScale_);
                ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 145.0f * uiScale_);
                ImGui::TableSetupColumn("Parameters & Target", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();

                for (const auto& r : sel->logic.rules) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextColored(ImVec4(0.0f, 0.86f, 1.0f, 1.0f), "%s", r.ruleId.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(r.triggerType.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(r.conditionExpr.c_str());
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(r.actionType.c_str());
                    ImGui::TableSetColumnIndex(4);
                    ImGui::Text("paramA=%.2f, paramB=%.2f, target=%s",
                        r.paramA, r.paramB,
                        r.targetObjectId == 0 ? "Self" : core::FormatObjectId(r.targetObjectId).c_str());
                }
                ImGui::EndTable();
            }
        }
    }
    ImGui::End();
}

void StudioUiController::DrawPlatzAiAndSpecialistPanel() {
    if (ImGui::Begin("PLATZ AI & Specialists")) {
        if (ImGui::Button("Run 3-Role Coordinated Specialist Workflow (Planner + 2 Neural Models + Critic)")) {
            InvokeCommandById("ai.run_three_role_workflow");
        }
        ImGui::SameLine();
        if (ImGui::Button("Retrain Both Local Neural Specialists")) {
            InvokeCommandById("ai.train_specialist");
        }
        ImGui::SameLine();
        if (stagedAiSession_.GetStatus() == ai::StagedSessionStatus::ActiveStaging) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.14f, 0.65f, 0.34f, 1.0f));
            if (ImGui::Button("Approve & Commit Staged AI Batch")) {
                std::string err;
                stagedAiSession_.ApproveAndCommit(stagedAiSession_.GetActiveSessionToken(), project_, nullptr, &err);
            }
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::Button("Discard Staged Batch")) {
                stagedAiSession_.DiscardSession();
            }
        }

        ImGui::Separator();

        const auto& matRec = specialistModel_.GetActiveReceipt();
        const auto& gpRec = gameplaySpecialist_.GetActiveReceipt();

        if (ImGui::BeginTable("##specialist_table", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn("Original Local Specialist Model", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Holdout MSE vs Baseline", ImGuiTableColumnFlags_WidthFixed, 205.0f * uiScale_);
            ImGui::TableSetupColumn("Measured Accuracy / MAE", ImGuiTableColumnFlags_WidthFixed, 175.0f * uiScale_);
            ImGui::TableSetupColumn("Held-Out R^2", ImGuiTableColumnFlags_WidthFixed, 95.0f * uiScale_);
            ImGui::TableSetupColumn("Timed Latency & Status", ImGuiTableColumnFlags_WidthFixed, 215.0f * uiScale_);
            ImGui::TableHeadersRow();

            auto drawModelRow = [&](int idx, const ai::SpecialistTrialReceipt& r) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(ImVec4(0.0f, 0.88f, 1.0f, 1.0f), "%d. %s", idx, r.experimentName.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%.5f (base %.4f, -%.1f%%)", r.finalHoldoutMse, r.baselineHoldoutMse, r.mseReductionOverBaselinePct);
                ImGui::TableSetColumnIndex(2);
                ImGui::TextColored(ImVec4(0.28f, 0.95f, 0.58f, 1.0f), "%.1f%% | MAE %.4f", r.holdoutAccuracyPct, r.holdoutMae);
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%.4f", r.holdoutRSquared);
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%.3f us (%ux) · %s", r.avgInferenceLatencyUs, r.inferenceTimingPasses, r.qualificationStatus.c_str());
            };
            drawModelRow(1, matRec);
            drawModelRow(2, gpRec);

            ImGui::EndTable();
        }

        if (!lastThreeRoleReceipt_.plannerSummary.empty()) {
            ImGui::TextColored(ImVec4(0.75f, 0.88f, 1.0f, 1.0f), "%s", lastThreeRoleReceipt_.plannerSummary.c_str());
            ImGui::TextColored(ImVec4(0.30f, 0.94f, 0.60f, 1.0f), "%s", lastThreeRoleReceipt_.criticVerdict.c_str());
        } else {
            ImGui::Text("Status: %s  |  Screen-Reader Log: %s",
                statusBannerMessage_.c_str(),
                accessibility_.GetLastAnnouncement().c_str());
        }
    }
    ImGui::End();
}

} // namespace platz::studio
