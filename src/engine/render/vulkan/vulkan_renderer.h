#pragma once

#include "core/project_state.h"
#include "creative/mesh_generator.h"
#include "creative/texture_generator.h"
#include "engine/render/vulkan/shader_compiler.h"
#include "engine/render/vulkan/vulkan_context.h"
#include "engine/scene/render_scene.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

struct ImFont;

namespace platz::platform {
class SdlWindow;
}

namespace platz::engine::vulkan {

// Matches shaders/pbr_surface.slang SceneUniforms (std140 aligned)
struct alignas(16) GpuSceneUniforms {
    glm::mat4 viewProj{1.0f};
    glm::vec4 cameraPos{0.0f, 5.0f, 10.0f, 1.15f};       // xyz = pos, w = exposure
    glm::vec4 sunDirection{0.45f, 0.82f, 0.35f, 2.8f};   // xyz = dir, w = intensity
    glm::vec4 sunColor{1.0f, 0.97f, 0.92f, 0.35f};       // rgb = color, a = ambient
    glm::vec4 pointLightPos{0.0f, 4.0f, 0.0f, 25.0f};    // xyz = pos, w = radius
    glm::vec4 pointLightColor{0.35f, 0.85f, 1.0f, 2.2f}; // rgb = color, a = intensity
    glm::vec4 skyTopColor{0.10f, 0.16f, 0.28f, 1.0f};
    glm::vec4 skyHorizonColor{0.24f, 0.32f, 0.44f, 1.0f};
};

// Matches shaders/pbr_surface.slang ObjectPushConstants (128 bytes)
struct alignas(16) GpuObjectPushConstants {
    glm::mat4 modelMatrix{1.0f};
    glm::vec4 baseColor{0.8f, 0.8f, 0.85f, 1.0f};
    glm::vec4 pbrParams{0.15f, 0.42f, 0.0f, 1.0f}; // metallic, roughness, emissiveStrength, normalScale
    glm::vec4 emissiveColor{0.0f, 0.0f, 0.0f, 0.0f}; // rgb = emissive, w = isSelected
    uint32_t objectIdLow{0};
    uint32_t objectIdHigh{0};
    uint32_t textureFlags{0};
    uint32_t pad0{0};
};

struct GpuMeshResource {
    VulkanBuffer vertexBuffer;
    VulkanBuffer indexBuffer;
    uint32_t vertexCount{0};
    uint32_t indexCount{0};
    uint64_t assetGeneration{0};
    std::string modifierSignature;
};

struct GpuTextureResource {
    VulkanImage image;
    VkSampler sampler{VK_NULL_HANDLE};
    VkDescriptorSet imguiDescriptorSet{VK_NULL_HANDLE};
    uint64_t assetGeneration{0};
};

struct FramePerfMetrics {
    double cpuFrameTimeMs{0.0};
    double gpuScenePassTimeMs{0.0};
    uint32_t drawCallCount{0};
    uint32_t triangleCount{0};
    uint64_t renderedFrameCount{0};
    uint32_t viewportWidth{0};
    uint32_t viewportHeight{0};
    uint32_t rebuiltCacheCount{0};
};

class VulkanRenderer {
public:
    VulkanRenderer() = default;
    ~VulkanRenderer();

    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;

    bool Initialize(
        platform::SdlWindow* window,
        uint32_t initialViewportWidth,
        uint32_t initialViewportHeight,
        bool enableValidation,
        std::string* outError = nullptr);

    void Shutdown();

    bool IsInitialized() const { return initialized_; }
    VulkanContext& GetContext() { return ctx_; }
    const VulkanContext& GetContext() const { return ctx_; }
    ShaderCompiler& GetShaderCompiler() { return shaderCompiler_; }
    const ShaderCompiler& GetShaderCompiler() const { return shaderCompiler_; }

    bool ResizeOffscreenViewport(uint32_t newWidth, uint32_t newHeight, std::string* outError = nullptr);

    // Renders the 3D scene into the offscreen Vulkan 1.4 dynamic rendering target (Color + R32_UINT Object ID + D32 Depth)
    bool RenderSceneOffscreen(
        const core::ProjectState& project,
        const std::vector<core::SceneObject>& activeObjects,
        const scene::ViewportCamera& camera,
        core::ObjectId selectedObjectId,
        std::string* outError = nullptr);

    // Renders a complete interactive Studio frame (Offscreen 3D Viewport + SDL3 Swapchain + Dear ImGui Docking UI)
    bool RenderInteractiveFrame(
        const core::ProjectState& project,
        const std::vector<core::SceneObject>& activeObjects,
        const scene::ViewportCamera& camera,
        core::ObjectId selectedObjectId,
        const std::function<void()>& buildImGuiCallback,
        std::string* outError = nullptr);

    // Read back rendered offscreen 3D viewport pixels from GPU to CPU
    bool ReadbackViewportColorRgba(creative::ImageData2D* outImage, std::string* outError = nullptr);

    // Read back the full composed Studio application window from the last presented swapchain image
    bool ReadbackSwapchainWindowRgba(creative::ImageData2D* outImage, std::string* outError = nullptr);

    // Read back exact 32-bit ObjectId from the GPU picking attachment at (pixelX, pixelY) (PLZ-RENDER-001, N-01)
    bool ReadbackPickedObjectIdAtPixel(
        uint32_t pixelX,
        uint32_t pixelY,
        core::ObjectId* outPickedId,
        std::string* outError = nullptr);

    // Corrupt / clear derived GPU mesh & texture caches to verify transparent rebuild from canonical source (N-04, N-05)
    void InvalidateDerivedGpuCaches();
    bool EnsureProjectGpuResourcesSynced(
        const core::ProjectState& project,
        const std::vector<core::SceneObject>& activeObjects,
        std::string* outError = nullptr);

    bool EnsureProjectGpuResourcesSynced(const core::ProjectState& project, std::string* outError = nullptr) {
        return EnsureProjectGpuResourcesSynced(project, project.GetObjects(), outError);
    }

    // Hot-reload PBR surface shader from Slang source code with safe fallback on compile error (N-13)
    bool HotReloadPbrShaderFromSlangSource(const std::string& slangSourceCode, std::string* outDiagnostic = nullptr);

    // Upload or update live 2D Pixel-Art Sprite and Material Graph preview textures for UI & scene display
    bool UpdatePixelArtPreviewTexture(const creative::ImageData2D& spriteImg, std::string* outError = nullptr);
    bool UpdateMaterialGraphPreviewTexture(const creative::ImageData2D& graphImg, std::string* outError = nullptr);

    VkDescriptorSet GetViewportImGuiDescriptorSet() const { return viewportImGuiDescriptorSet_; }
    VkDescriptorSet GetPixelArtImGuiDescriptorSet() const { return pixelArtPreviewTexture_.imguiDescriptorSet; }
    VkDescriptorSet GetMaterialGraphImGuiDescriptorSet() const { return materialGraphPreviewTexture_.imguiDescriptorSet; }

    ImFont* GetFontRegular() const { return fontRegular_; }
    ImFont* GetFontBold() const { return fontBold_; }
    ImFont* GetFontMono() const { return fontMono_; }

    float GetSunAzimuthDeg() const { return sunAzimuthDeg_; }
    void SetSunAzimuthDeg(float az) { sunAzimuthDeg_ = az; }
    float GetSunElevationDeg() const { return sunElevationDeg_; }
    void SetSunElevationDeg(float el) { sunElevationDeg_ = el; }

    uint32_t GetViewportWidth() const { return viewportWidth_; }
    uint32_t GetViewportHeight() const { return viewportHeight_; }
    const FramePerfMetrics& GetPerfMetrics() const { return perfMetrics_; }

private:
    bool CreateOffscreenTargets(uint32_t width, uint32_t height, std::string* outError);
    void DestroyOffscreenTargets();

    bool CreateSwapchain(std::string* outError);
    void DestroySwapchain();

    bool CreateDescriptorsAndPipelines(std::string* outError);
    bool BuildGraphicsPipelinesFromStages(
        const CompiledShaderStage& pbrVert,
        const CompiledShaderStage& pbrFrag,
        const CompiledShaderStage& gridVert,
        const CompiledShaderStage& gridFrag,
        std::string* outError);
    void DestroyPipelines();

    bool UploadMeshToGpu(const creative::MeshData& mesh, GpuMeshResource* outResource, std::string* outError);
    bool UploadTextureToGpu(const creative::ImageData2D& img, GpuTextureResource* outResource, std::string* outError);

    void RecordOffscreenScenePass(
        VkCommandBuffer cmd,
        uint32_t frameSlot,
        const std::vector<core::SceneObject>& activeObjects,
        const core::ProjectState& project,
        const scene::ViewportCamera& camera,
        core::ObjectId selectedObjectId);

    platform::SdlWindow* window_{nullptr};
    VulkanContext ctx_;
    ShaderCompiler shaderCompiler_;
    bool initialized_{false};
    bool imguiInitialized_{false};

    uint32_t viewportWidth_{1280};
    uint32_t viewportHeight_{720};

    // Offscreen 3D viewport targets
    VulkanImage offscreenColor_;
    VulkanImage offscreenObjectId_;
    VulkanImage offscreenDepth_;
    VkSampler linearSampler_{VK_NULL_HANDLE};
    VkSampler nearestSampler_{VK_NULL_HANDLE};
    VkDescriptorSet viewportImGuiDescriptorSet_{VK_NULL_HANDLE};
    bool offscreenLayoutInitialized_{false};

    // Swapchain for windowed Studio mode
    VkSwapchainKHR swapchain_{VK_NULL_HANDLE};
    VkFormat swapchainFormat_{VK_FORMAT_B8G8R8A8_UNORM};
    VkExtent2D swapchainExtent_{0, 0};
    bool swapchainSupportsTransferSrc_{false};
    uint32_t lastPresentedSwapchainImageIndex_{0};
    bool hasPresentedSwapchainFrame_{false};
    VulkanBuffer lastSwapchainCaptureBuffer_{};
    uint32_t lastSwapchainCaptureWidth_{0};
    uint32_t lastSwapchainCaptureHeight_{0};
    std::vector<VkImage> swapchainImages_;
    std::vector<VkImageView> swapchainImageViews_;
    std::vector<VkSemaphore> swapchainRenderFinishedSemaphores_;

    // Frame synchronization & command buffers
    VkCommandPool frameCommandPool_{VK_NULL_HANDLE};
    VkCommandBuffer frameCommandBuffers_[VulkanContext::kMaxFramesInFlight]{};
    VkFence frameFences_[VulkanContext::kMaxFramesInFlight]{};
    VkSemaphore imageAvailableSemaphores_[VulkanContext::kMaxFramesInFlight]{};
    VulkanBuffer sceneUniformBuffers_[VulkanContext::kMaxFramesInFlight]{};
    VkQueryPool timestampQueryPool_{VK_NULL_HANDLE};
    uint32_t currentFrameSlot_{0};

    // Descriptors & Pipelines
    VkDescriptorPool descriptorPool_{VK_NULL_HANDLE};
    VkDescriptorSetLayout sceneDescriptorLayout_{VK_NULL_HANDLE};
    VkDescriptorSet sceneDescriptorSets_[VulkanContext::kMaxFramesInFlight]{};
    VkPipelineLayout pbrPipelineLayout_{VK_NULL_HANDLE};
    VkPipeline pbrPipeline_{VK_NULL_HANDLE};
    VkPipeline gridPipeline_{VK_NULL_HANDLE};

    // Built-in & Project-derived GPU caches
    std::map<std::string, creative::MeshData> cpuPrimitiveBaseMeshes_;
    std::map<std::string, GpuMeshResource> primitiveMeshes_;
    std::map<core::AssetId, GpuMeshResource> customAssetMeshes_;
    std::map<core::ObjectId, GpuMeshResource> modifiedObjectMeshes_;
    GpuMeshResource worldGridMesh_;
    GpuTextureResource defaultCalibrationTexture_;
    GpuTextureResource pixelArtPreviewTexture_;
    GpuTextureResource materialGraphPreviewTexture_;

    ImFont* fontRegular_{nullptr};
    ImFont* fontBold_{nullptr};
    ImFont* fontMono_{nullptr};

    float sunAzimuthDeg_{38.0f};
    float sunElevationDeg_{56.0f};

    FramePerfMetrics perfMetrics_{};
};

} // namespace platz::engine::vulkan
