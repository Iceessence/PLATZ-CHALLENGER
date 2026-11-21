#pragma once

#include "ai/command_registry.h"
#include "ai/specialist_model.h"
#include "ai/staged_session.h"
#include "core/project_state.h"
#include "creative/texture_generator.h"
#include "engine/render/vulkan/vulkan_renderer.h"
#include "engine/runtime/runtime_session.h"
#include "engine/scene/render_scene.h"
#include "studio/validation_world.h"

#include <cstdint>
#include <string>
#include <vector>

namespace platz::studio {

enum class WorkspaceMode {
    WorldAndScene = 0,
    ModelAndAssets = 1,
    MaterialsAndShaders = 2,
    PixelArtAndSprites = 3,
    PlatzAiWorkbench = 4
};

enum class ComplexityTier {
    GuidedBeginner = 0,
    ProCreator = 1,
    EngineDeveloper = 2
};

struct ThemeSpecification {
    std::string themeName{"PZ Obsidian Dark"};
    glm::vec4 windowBg{0.065f, 0.075f, 0.100f, 1.0f};
    glm::vec4 panelHeader{0.115f, 0.145f, 0.205f, 1.0f};
    glm::vec4 accentPrimary{0.00f, 0.78f, 0.96f, 1.0f}; // PZ Cyan
    glm::vec4 textPrimary{0.94f, 0.96f, 0.99f, 1.0f};

    // Attempted custom overrides on protected security controls (tested by PLZ-SAFEX-006 / N-24)
    bool attemptHideProtectedApprovalBar{false};
    float requestedStopButtonOpacity{1.0f};
    glm::vec4 requestedStopButtonColor{0.88f, 0.18f, 0.22f, 1.0f};
};

struct ProtectedSecurityBarState {
    bool visibleAndOperable{true};
    float effectiveStopButtonOpacity{1.0f};
    float effectiveContrastRatio{12.5f};
    bool tamperedThemeSanitized{false};
    std::string securityEnforcementNote;
};

struct CommandPaletteEntry {
    std::string commandId;
    std::string title;
    std::string category;
    std::string keywords;
    std::string shortcut;
    std::string maturityBadge{"VERIFIED"}; // PLZ-UXPLUS-008
};

struct PerformanceAdvisorHint {
    core::ObjectId objectId{0};
    std::string objectName;
    uint32_t estimatedTriangles{0};
    std::string plainEnglishExplanation;
    std::string reversibleOptimizationAction;
};

// User-composed custom toolbar button binding (PLZ-UX-005, PLZ-CREATEX-009)
struct CustomUiBindingButton {
    std::string label;
    std::string boundCommandId;
};

class AccessibilityAnnouncer {
public:
    void Announce(const std::string& category, const std::string& message);
    const std::vector<std::string>& GetRecentAnnouncements() const { return announcements_; }
    const std::string& GetLastAnnouncement() const { return lastAnnouncement_; }

private:
    std::vector<std::string> announcements_;
    std::string lastAnnouncement_{"PLATZ Studio Ready. Press Ctrl+K for Universal Command Bar."};
};

class StudioUiController {
public:
    StudioUiController(
        core::ProjectState& project,
        engine::vulkan::VulkanRenderer& renderer,
        engine::runtime::RuntimeSession& runtime,
        ai::CommandRegistry& commandRegistry,
        ai::StagedAiSession& stagedAiSession,
        ai::NativeMaterialSpecialistModel& specialistModel);

    // Draws the complete Dear ImGui Docking Studio workspace (called inside RenderInteractiveFrame)
    void BuildImGuiStudioFrame();

    // Programmatic DockBuilder layout reset / recovery (PLZ-UX-003, PLZ-UX-004, PLZ-CREATEX2-007)
    void RequestDockLayoutReset() { dockLayoutNeedsBuild_ = true; }

    // Universal Command Bar search & invocation (PLZ-UX-001, PLZ-UXPLUS-001)
    std::vector<CommandPaletteEntry> SearchCommandBar(const std::string& query) const;
    bool InvokeCommandById(const std::string& commandId, std::string* outStatusMessage = nullptr);

    // Theme & Protected Security Control enforcement (PLZ-UX-004, PLZ-UX-013, PLZ-SAFEX-006, N-24)
    ProtectedSecurityBarState ApplyThemeWithProtectedControlGuard(const ThemeSpecification& requestedTheme);
    void ApplyThemeToImGui() const;
    const ProtectedSecurityBarState& GetProtectedBarState() const { return protectedBarState_; }

    // Non-coder performance explanations linked to real scene objects (PLZ-UXPLUS-006, PLZ-WORLDX-008)
    std::vector<PerformanceAdvisorHint> AnalyzeScenePerformanceForNonCoders() const;

    // Selection, Camera, Workspace, and Accessibility accessors
    core::ObjectId GetSelectedObjectId() const { return selectedObjectId_; }
    void SetSelectedObjectId(core::ObjectId id);

    engine::scene::ViewportCamera& GetCamera() { return camera_; }
    const engine::scene::ViewportCamera& GetCamera() const { return camera_; }

    WorkspaceMode GetWorkspaceMode() const { return workspaceMode_; }
    void SetWorkspaceMode(WorkspaceMode mode);

    ComplexityTier GetComplexityTier() const { return complexityTier_; }
    void SetComplexityTier(ComplexityTier tier) { complexityTier_ = tier; }

    float GetUiScale() const { return uiScale_; }
    void SetUiScale(float scale);

    AccessibilityAnnouncer& GetAccessibility() { return accessibility_; }
    const AccessibilityAnnouncer& GetAccessibility() const { return accessibility_; }

    const std::vector<core::SceneObject>& GetActiveSceneObjects() const;
    creative::PixelArtCanvas& GetPixelArtCanvas() { return pixelCanvas_; }
    creative::MaterialGraphDocument& GetMaterialGraph() { return materialGraph_; }
    ai::NativeGameplayBalanceSpecialistModel& GetGameplaySpecialist() { return gameplaySpecialist_; }

private:
    void RegisterDefaultCommandEntries();
    void RebuildProgrammaticDockLayout(uint32_t dockspaceId);
    void DrawProtectedTopMenuAndSecurityBar();
    void DrawUniversalCommandBarModal();
    void DrawHierarchyAndStationsPanel();
    void Draw3DViewportPanel();
    void Draw3DGizmoAndSelectionOverlay(
        const glm::vec2& viewportScreenMin,
        const glm::vec2& viewportSize);
    void DrawInspectorAndMaterialPanel();
    void DrawAssetAndValidationWorldPanel();
    void DrawMaterialGraphAndSlangPanel();
    void DrawPixelArtAndSpriteStudioPanel();
    void DrawLogicAndRuntimeHudPanel();
    void DrawPlatzAiAndSpecialistPanel();

    core::ProjectState& project_;
    engine::vulkan::VulkanRenderer& renderer_;
    engine::runtime::RuntimeSession& runtime_;
    ai::CommandRegistry& commandRegistry_;
    ai::StagedAiSession& stagedAiSession_;
    ai::NativeMaterialSpecialistModel& specialistModel_;
    ai::NativeGameplayBalanceSpecialistModel gameplaySpecialist_;

    core::ObjectId selectedObjectId_{0};
    engine::scene::ViewportCamera camera_;
    engine::scene::GizmoMode gizmoMode_{engine::scene::GizmoMode::Translate};
    engine::scene::SnapSettings snapSettings_{};
    WorkspaceMode workspaceMode_{WorkspaceMode::WorldAndScene};
    ComplexityTier complexityTier_{ComplexityTier::ProCreator};

    ThemeSpecification activeTheme_{};
    ProtectedSecurityBarState protectedBarState_{};
    float uiScale_{1.0f};
    bool dockLayoutNeedsBuild_{true};
    std::string pendingFocusWindowName_;
    bool showCommandBar_{false};
    char commandSearchBuf_[128]{};
    char hierarchyFilterBuf_[128]{};

    // Interactive 3D viewport gizmo drag state
    int activeGizmoDragAxis_{-1}; // -1 = none, 0 = X, 1 = Y, 2 = Z
    glm::vec2 gizmoDragStartMouse_{0.0f};
    core::TransformComponent gizmoDragStartTransform_{};

    // Material Graph & 2D Pixel Art live authoring states
    creative::MaterialGraphDocument materialGraph_;
    std::string lastCompiledSlangPreview_;
    std::string materialGraphDiagnostic_{"Material graph ready (4 nodes connected to PbrOutput)."};

    creative::PixelArtCanvas pixelCanvas_{16, 16};
    uint8_t activePixelPaletteIdx_{1};
    bool pixelMirrorSymmetry_{true};

    // Time-travel visual logic debugger state
    int debuggerTimelineScrubIndex_{-1};

    // PLATZ AI & Specialist state
    int selectedSpecialistArchetype_{1};
    int selectedGameplayGenreIndex_{2};
    float specialistWearParam_{0.22f};
    ai::ThreeRoleWorkflowReceipt lastThreeRoleReceipt_{};

    // Visual UI customizer user-bound toolbar items (PLZ-UX-005)
    std::vector<CustomUiBindingButton> customToolbarBindings_;
    char newCustomBtnLabel_[64]{"Quick Gold"};
    int selectedBindCommandIdx_{11};

    std::string statusBannerMessage_{"PLATZ Studio initialized on Vulkan 1.4."};
    std::vector<CommandPaletteEntry> commandEntries_;
    ValidationRouteReceipt lastRouteReceipt_{};
    AccessibilityAnnouncer accessibility_;
};

} // namespace platz::studio
