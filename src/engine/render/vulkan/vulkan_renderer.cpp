#include "engine/render/vulkan/vulkan_renderer.h"
#include "platform/platform_paths.h"
#include "platform/sdl_window.h"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <sstream>

namespace platz::engine::vulkan {

namespace {

void TransitionImageLayout2(
    VkCommandBuffer cmd,
    VkImage image,
    VkImageAspectFlags aspectMask,
    VkImageLayout oldLayout,
    VkImageLayout newLayout,
    VkPipelineStageFlags2 srcStage,
    VkAccessFlags2 srcAccess,
    VkPipelineStageFlags2 dstStage,
    VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStage;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspectMask;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkDependencyInfo depInfo{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depInfo.imageMemoryBarrierCount = 1;
    depInfo.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &depInfo);
}

std::string ComputeModifierStackSignature(
    const std::string& primitiveType,
    core::AssetId meshAssetId,
    const std::vector<core::MeshModifier>& stack) {
    std::ostringstream oss;
    oss << primitiveType << ":" << meshAssetId << ":";
    for (const auto& m : stack) {
        if (!m.enabled) continue;
        oss << m.modifierType << "(" << m.paramA << "," << m.paramB << "," << m.seed << ");";
    }
    return oss.str();
}

} // namespace

VulkanRenderer::~VulkanRenderer() {
    Shutdown();
}

bool VulkanRenderer::Initialize(
    platform::SdlWindow* window,
    uint32_t initialViewportWidth,
    uint32_t initialViewportHeight,
    bool enableValidation,
    std::string* outError) {
    Shutdown();
    window_ = window;
    viewportWidth_ = std::max(initialViewportWidth, 64u);
    viewportHeight_ = std::max(initialViewportHeight, 64u);

    if (!ctx_.Initialize(window_, enableValidation, outError)) {
        return false;
    }

    VkDevice dev = ctx_.GetDevice();

    // Samplers (linear anisotropic for 3D PBR + nearest-neighbor for pixel-exact 2D)
    VkSamplerCreateInfo sampInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampInfo.magFilter = VK_FILTER_LINEAR;
    sampInfo.minFilter = VK_FILTER_LINEAR;
    sampInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampInfo.anisotropyEnable = ctx_.GetCapabilities().supportsSamplerAnisotropy ? VK_TRUE : VK_FALSE;
    sampInfo.maxAnisotropy = ctx_.GetCapabilities().supportsSamplerAnisotropy ? 8.0f : 1.0f;
    sampInfo.maxLod = 1.0f;
    if (vkCreateSampler(dev, &sampInfo, nullptr, &linearSampler_) != VK_SUCCESS) {
        if (outError) *outError = "vkCreateSampler (linear) failed";
        Shutdown();
        return false;
    }

    sampInfo.magFilter = VK_FILTER_NEAREST;
    sampInfo.minFilter = VK_FILTER_NEAREST;
    sampInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampInfo.anisotropyEnable = VK_FALSE;
    sampInfo.maxAnisotropy = 1.0f;
    if (vkCreateSampler(dev, &sampInfo, nullptr, &nearestSampler_) != VK_SUCCESS) {
        if (outError) *outError = "vkCreateSampler (nearest) failed";
        Shutdown();
        return false;
    }

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = ctx_.GetGraphicsQueueFamily();
    if (vkCreateCommandPool(dev, &poolInfo, nullptr, &frameCommandPool_) != VK_SUCCESS) {
        if (outError) *outError = "vkCreateCommandPool failed";
        Shutdown();
        return false;
    }

    VkCommandBufferAllocateInfo cbAlloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbAlloc.commandPool = frameCommandPool_;
    cbAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbAlloc.commandBufferCount = VulkanContext::kMaxFramesInFlight;
    if (vkAllocateCommandBuffers(dev, &cbAlloc, frameCommandBuffers_) != VK_SUCCESS) {
        if (outError) *outError = "vkAllocateCommandBuffers failed";
        Shutdown();
        return false;
    }

    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};

    for (uint32_t i = 0; i < VulkanContext::kMaxFramesInFlight; ++i) {
        vkCreateFence(dev, &fenceInfo, nullptr, &frameFences_[i]);
        vkCreateSemaphore(dev, &semInfo, nullptr, &imageAvailableSemaphores_[i]);

        if (!ctx_.CreateBuffer(
                sizeof(GpuSceneUniforms),
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VMA_MEMORY_USAGE_AUTO,
                VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                "SceneUniformBuffer_" + std::to_string(i),
                &sceneUniformBuffers_[i],
                outError)) {
            Shutdown();
            return false;
        }
    }

    if (ctx_.GetCapabilities().supportsTimestampQueries) {
        VkQueryPoolCreateInfo qpInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qpInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpInfo.queryCount = VulkanContext::kMaxFramesInFlight * 2;
        vkCreateQueryPool(dev, &qpInfo, nullptr, &timestampQueryPool_);
    }

    // Upload default calibration PBR texture and primitive meshes
    creative::ImageData2D calTex = creative::TextureGenerator::GeneratePbrCalibrationTexture(256, 256);
    if (!UploadTextureToGpu(calTex, &defaultCalibrationTexture_, outError)) {
        Shutdown();
        return false;
    }

    cpuPrimitiveBaseMeshes_["cube"] = creative::MeshGenerator::GenerateCube(1.0f);
    cpuPrimitiveBaseMeshes_["sphere"] = creative::MeshGenerator::GenerateSphere(0.6f, 28, 18);
    cpuPrimitiveBaseMeshes_["cylinder"] = creative::MeshGenerator::GenerateCylinder(0.5f, 1.2f, 24);
    cpuPrimitiveBaseMeshes_["plane"] = creative::MeshGenerator::GeneratePlane(10.0f, 10.0f, 4.0f);
    cpuPrimitiveBaseMeshes_["torus"] = creative::MeshGenerator::GenerateTorus(0.65f, 0.22f, 32, 16);

    for (const auto& [name, meshData] : cpuPrimitiveBaseMeshes_) {
        if (!UploadMeshToGpu(meshData, &primitiveMeshes_[name], outError)) {
            Shutdown();
            return false;
        }
    }
    if (!UploadMeshToGpu(creative::MeshGenerator::GenerateWorldGridLines(30, 1.0f), &worldGridMesh_, outError)) {
        Shutdown();
        return false;
    }

    if (!CreateDescriptorsAndPipelines(outError)) {
        Shutdown();
        return false;
    }

    if (!CreateOffscreenTargets(viewportWidth_, viewportHeight_, outError)) {
        Shutdown();
        return false;
    }

    if (window_ && !window_->IsHeadless()) {
        if (!CreateSwapchain(outError)) {
            Shutdown();
            return false;
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        io.IniFilename = nullptr; // Programmatic DockBuilder manages consistent recoverable workspace layouts

        // Load high-DPI crisp TrueType system typography (Segoe UI + Consolas)
        ImFontConfig fontCfg{};
        fontCfg.OversampleH = 3;
        fontCfg.OversampleV = 2;
        fontCfg.PixelSnapH = true;

        std::filesystem::path winFonts("C:/Windows/Fonts");
        std::error_code ec;
        if (std::filesystem::exists(winFonts / "segoeui.ttf", ec)) {
            fontRegular_ = io.Fonts->AddFontFromFileTTF((winFonts / "segoeui.ttf").string().c_str(), 16.0f, &fontCfg);
        }
        if (std::filesystem::exists(winFonts / "segoeuib.ttf", ec)) {
            fontBold_ = io.Fonts->AddFontFromFileTTF((winFonts / "segoeuib.ttf").string().c_str(), 17.5f, &fontCfg);
        }
        if (std::filesystem::exists(winFonts / "consola.ttf", ec)) {
            fontMono_ = io.Fonts->AddFontFromFileTTF((winFonts / "consola.ttf").string().c_str(), 14.5f, &fontCfg);
        }
        if (!fontRegular_) {
            fontRegular_ = io.Fonts->AddFontDefault();
        }
        if (!fontBold_) fontBold_ = fontRegular_;
        if (!fontMono_) fontMono_ = fontRegular_;

        ImGui_ImplSDL3_InitForVulkan(window_->GetNativeHandle());

        VkPipelineRenderingCreateInfoKHR imguiDynRender{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
        imguiDynRender.colorAttachmentCount = 1;
        imguiDynRender.pColorAttachmentFormats = &swapchainFormat_;

        ImGui_ImplVulkan_InitInfo initInfo{};
        initInfo.ApiVersion = VK_API_VERSION_1_4;
        initInfo.Instance = ctx_.GetInstance();
        initInfo.PhysicalDevice = ctx_.GetPhysicalDevice();
        initInfo.Device = ctx_.GetDevice();
        initInfo.QueueFamily = ctx_.GetGraphicsQueueFamily();
        initInfo.Queue = ctx_.GetGraphicsQueue();
        initInfo.DescriptorPoolSize = 64;
        initInfo.MinImageCount = 2;
        initInfo.ImageCount = std::max(2u, static_cast<uint32_t>(swapchainImages_.size()));
        initInfo.UseDynamicRendering = true;
        initInfo.PipelineInfoMain.PipelineRenderingCreateInfo = imguiDynRender;
        initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

        if (!ImGui_ImplVulkan_Init(&initInfo)) {
            if (outError) *outError = "ImGui_ImplVulkan_Init failed";
            Shutdown();
            return false;
        }
        imguiInitialized_ = true;

        viewportImGuiDescriptorSet_ = ImGui_ImplVulkan_AddTexture(
            linearSampler_,
            offscreenColor_.view,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        // Initialize 2D Pixel-Art and Material Graph preview textures
        creative::PixelArtCanvas defaultPixelCanvas(16, 16);
        UpdatePixelArtPreviewTexture(defaultPixelCanvas.BakeToImageData2D());

        creative::MaterialGraphDocument defaultMatGraph;
        UpdateMaterialGraphPreviewTexture(defaultMatGraph.BakeGraphAlbedoTexture(128, 128));
    }

    initialized_ = true;
    return true;
}

void VulkanRenderer::Shutdown() {
    if (ctx_.IsInitialized()) {
        vkDeviceWaitIdle(ctx_.GetDevice());
    }

    if (imguiInitialized_) {
        if (viewportImGuiDescriptorSet_ != VK_NULL_HANDLE) {
            ImGui_ImplVulkan_RemoveTexture(viewportImGuiDescriptorSet_);
            viewportImGuiDescriptorSet_ = VK_NULL_HANDLE;
        }
        if (pixelArtPreviewTexture_.imguiDescriptorSet != VK_NULL_HANDLE) {
            ImGui_ImplVulkan_RemoveTexture(pixelArtPreviewTexture_.imguiDescriptorSet);
            pixelArtPreviewTexture_.imguiDescriptorSet = VK_NULL_HANDLE;
        }
        if (materialGraphPreviewTexture_.imguiDescriptorSet != VK_NULL_HANDLE) {
            ImGui_ImplVulkan_RemoveTexture(materialGraphPreviewTexture_.imguiDescriptorSet);
            materialGraphPreviewTexture_.imguiDescriptorSet = VK_NULL_HANDLE;
        }
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        imguiInitialized_ = false;
    }

    DestroySwapchain();
    DestroyOffscreenTargets();
    DestroyPipelines();

    VkDevice dev = ctx_.GetDevice();
    if (dev != VK_NULL_HANDLE) {
        for (auto& [k, m] : primitiveMeshes_) {
            ctx_.DestroyBufferImmediate(m.vertexBuffer);
            ctx_.DestroyBufferImmediate(m.indexBuffer);
        }
        primitiveMeshes_.clear();

        for (auto& [id, m] : customAssetMeshes_) {
            ctx_.DestroyBufferImmediate(m.vertexBuffer);
            ctx_.DestroyBufferImmediate(m.indexBuffer);
        }
        customAssetMeshes_.clear();

        for (auto& [id, t] : customAssetTextures_) {
            ctx_.DestroyImageImmediate(t.image);
        }
        customAssetTextures_.clear();
        lastSyncedProjectGeneration_ = 0;
        derivedCachesDirty_ = true;

        for (auto& [id, m] : modifiedObjectMeshes_) {
            ctx_.DestroyBufferImmediate(m.vertexBuffer);
            ctx_.DestroyBufferImmediate(m.indexBuffer);
        }
        modifiedObjectMeshes_.clear();

        ctx_.DestroyBufferImmediate(worldGridMesh_.vertexBuffer);
        ctx_.DestroyBufferImmediate(worldGridMesh_.indexBuffer);
        ctx_.DestroyImageImmediate(defaultCalibrationTexture_.image);
        ctx_.DestroyImageImmediate(pixelArtPreviewTexture_.image);
        ctx_.DestroyImageImmediate(materialGraphPreviewTexture_.image);
        ctx_.DestroyBufferImmediate(lastSwapchainCaptureBuffer_);

        for (uint32_t i = 0; i < VulkanContext::kMaxFramesInFlight; ++i) {
            ctx_.DestroyBufferImmediate(sceneUniformBuffers_[i]);
            if (frameFences_[i] != VK_NULL_HANDLE) {
                vkDestroyFence(dev, frameFences_[i], nullptr);
                frameFences_[i] = VK_NULL_HANDLE;
            }
            if (imageAvailableSemaphores_[i] != VK_NULL_HANDLE) {
                vkDestroySemaphore(dev, imageAvailableSemaphores_[i], nullptr);
                imageAvailableSemaphores_[i] = VK_NULL_HANDLE;
            }
        }

        if (timestampQueryPool_ != VK_NULL_HANDLE) {
            vkDestroyQueryPool(dev, timestampQueryPool_, nullptr);
            timestampQueryPool_ = VK_NULL_HANDLE;
        }
        if (descriptorPool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(dev, descriptorPool_, nullptr);
            descriptorPool_ = VK_NULL_HANDLE;
        }
        if (sceneDescriptorLayout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(dev, sceneDescriptorLayout_, nullptr);
            sceneDescriptorLayout_ = VK_NULL_HANDLE;
        }
        if (frameCommandPool_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(dev, frameCommandPool_, nullptr);
            frameCommandPool_ = VK_NULL_HANDLE;
        }
        if (linearSampler_ != VK_NULL_HANDLE) {
            vkDestroySampler(dev, linearSampler_, nullptr);
            linearSampler_ = VK_NULL_HANDLE;
        }
        if (nearestSampler_ != VK_NULL_HANDLE) {
            vkDestroySampler(dev, nearestSampler_, nullptr);
            nearestSampler_ = VK_NULL_HANDLE;
        }
    }

    ctx_.Shutdown();
    initialized_ = false;
}

bool VulkanRenderer::UpdatePixelArtPreviewTexture(const creative::ImageData2D& spriteImg, std::string* outError) {
    if (imguiInitialized_ && pixelArtPreviewTexture_.imguiDescriptorSet != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(ctx_.GetDevice());
        ImGui_ImplVulkan_RemoveTexture(pixelArtPreviewTexture_.imguiDescriptorSet);
        pixelArtPreviewTexture_.imguiDescriptorSet = VK_NULL_HANDLE;
    }
    creative::ImageData2D exactImg = creative::TextureGenerator::SampleNearestNeighborExact(spriteImg, 8);
    exactImg.pixelExactNearest = true;
    if (!UploadTextureToGpu(exactImg, &pixelArtPreviewTexture_, outError)) {
        return false;
    }
    if (imguiInitialized_) {
        pixelArtPreviewTexture_.imguiDescriptorSet = ImGui_ImplVulkan_AddTexture(
            nearestSampler_,
            pixelArtPreviewTexture_.image.view,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return true;
}

bool VulkanRenderer::UpdateMaterialGraphPreviewTexture(const creative::ImageData2D& graphImg, std::string* outError) {
    if (imguiInitialized_ && materialGraphPreviewTexture_.imguiDescriptorSet != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(ctx_.GetDevice());
        ImGui_ImplVulkan_RemoveTexture(materialGraphPreviewTexture_.imguiDescriptorSet);
        materialGraphPreviewTexture_.imguiDescriptorSet = VK_NULL_HANDLE;
    }
    if (!UploadTextureToGpu(graphImg, &materialGraphPreviewTexture_, outError)) {
        return false;
    }
    if (imguiInitialized_) {
        materialGraphPreviewTexture_.imguiDescriptorSet = ImGui_ImplVulkan_AddTexture(
            linearSampler_,
            materialGraphPreviewTexture_.image.view,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return true;
}

bool VulkanRenderer::CreateOffscreenTargets(uint32_t width, uint32_t height, std::string* outError) {
    DestroyOffscreenTargets();
    viewportWidth_ = std::max(width, 64u);
    viewportHeight_ = std::max(height, 64u);

    if (!ctx_.CreateImage2D(
            viewportWidth_,
            viewportHeight_,
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,
            "OffscreenViewportColor",
            &offscreenColor_,
            outError)) {
        return false;
    }

    if (!ctx_.CreateImage2D(
            viewportWidth_,
            viewportHeight_,
            VK_FORMAT_R32_UINT,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,
            "OffscreenViewportObjectId",
            &offscreenObjectId_,
            outError)) {
        return false;
    }

    if (!ctx_.CreateImage2D(
            viewportWidth_,
            viewportHeight_,
            VK_FORMAT_D32_SFLOAT,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT,
            "OffscreenViewportDepth",
            &offscreenDepth_,
            outError)) {
        return false;
    }

    offscreenLayoutInitialized_ = false;
    perfMetrics_.viewportWidth = viewportWidth_;
    perfMetrics_.viewportHeight = viewportHeight_;

    if (imguiInitialized_) {
        if (viewportImGuiDescriptorSet_ != VK_NULL_HANDLE) {
            ImGui_ImplVulkan_RemoveTexture(viewportImGuiDescriptorSet_);
        }
        viewportImGuiDescriptorSet_ = ImGui_ImplVulkan_AddTexture(
            linearSampler_,
            offscreenColor_.view,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    return true;
}

void VulkanRenderer::DestroyOffscreenTargets() {
    ctx_.DestroyImageImmediate(offscreenColor_);
    ctx_.DestroyImageImmediate(offscreenObjectId_);
    ctx_.DestroyImageImmediate(offscreenDepth_);
    offscreenLayoutInitialized_ = false;
}

bool VulkanRenderer::ResizeOffscreenViewport(uint32_t newWidth, uint32_t newHeight, std::string* outError) {
    newWidth = std::clamp(newWidth, 64u, 4096u);
    newHeight = std::clamp(newHeight, 64u, 4096u);
    if (newWidth == viewportWidth_ && newHeight == viewportHeight_ && offscreenColor_.image != VK_NULL_HANDLE) {
        return true;
    }
    vkDeviceWaitIdle(ctx_.GetDevice());
    return CreateOffscreenTargets(newWidth, newHeight, outError);
}

bool VulkanRenderer::CreateSwapchain(std::string* outError) {
    DestroySwapchain();
    if (!window_ || window_->IsHeadless() || ctx_.GetSurface() == VK_NULL_HANDLE) {
        return true;
    }

    VkPhysicalDevice gpu = ctx_.GetPhysicalDevice();
    VkDevice dev = ctx_.GetDevice();
    VkSurfaceKHR surface = ctx_.GetSurface();

    VkSurfaceCapabilitiesKHR surfCaps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &surfCaps);

    uint32_t fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &fmtCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fmtCount);
    if (fmtCount > 0) {
        vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &fmtCount, formats.data());
    }

    VkSurfaceFormatKHR chosenFormat{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    if (!formats.empty()) {
        chosenFormat = formats[0];
        for (const auto& f : formats) {
            if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) {
                chosenFormat = f;
                break;
            }
        }
    }
    swapchainFormat_ = chosenFormat.format;

    int winW = 1600, winH = 920;
    window_->GetPixelSize(&winW, &winH);
    if (surfCaps.currentExtent.width != UINT32_MAX && surfCaps.currentExtent.width > 0) {
        swapchainExtent_ = surfCaps.currentExtent;
    } else {
        swapchainExtent_.width = std::clamp(static_cast<uint32_t>(winW), surfCaps.minImageExtent.width, surfCaps.maxImageExtent.width);
        swapchainExtent_.height = std::clamp(static_cast<uint32_t>(winH), surfCaps.minImageExtent.height, surfCaps.maxImageExtent.height);
    }

    if (swapchainExtent_.width == 0 || swapchainExtent_.height == 0) {
        return true;
    }

    uint32_t imageCount = std::max(2u, surfCaps.minImageCount + 1);
    if (surfCaps.maxImageCount > 0 && imageCount > surfCaps.maxImageCount) {
        imageCount = surfCaps.maxImageCount;
    }

    swapchainSupportsTransferSrc_ = ((surfCaps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0);

    // Choose present mode: FIFO for comfortable VSync interactive editing, MAILBOX/IMMEDIATE when unlocked for benchmarking
    VkPresentModeKHR chosenPresentMode = VK_PRESENT_MODE_FIFO_KHR;
    perfMetrics_.presentModeName = "FIFO";
    if (!preferVsyncFifo_) {
        uint32_t pmCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface, &pmCount, nullptr);
        std::vector<VkPresentModeKHR> modes(pmCount);
        if (pmCount > 0) {
            vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface, &pmCount, modes.data());
        }
        bool hasMailbox = false;
        bool hasImmediate = false;
        for (VkPresentModeKHR pm : modes) {
            if (pm == VK_PRESENT_MODE_MAILBOX_KHR) hasMailbox = true;
            if (pm == VK_PRESENT_MODE_IMMEDIATE_KHR) hasImmediate = true;
        }
        if (hasMailbox) {
            chosenPresentMode = VK_PRESENT_MODE_MAILBOX_KHR;
            perfMetrics_.presentModeName = "MAILBOX";
        } else if (hasImmediate) {
            chosenPresentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
            perfMetrics_.presentModeName = "IMMEDIATE";
        }
    }

    VkSwapchainCreateInfoKHR scInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    scInfo.surface = surface;
    scInfo.minImageCount = imageCount;
    scInfo.imageFormat = chosenFormat.format;
    scInfo.imageColorSpace = chosenFormat.colorSpace;
    scInfo.imageExtent = swapchainExtent_;
    scInfo.imageArrayLayers = 1;
    scInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                        (swapchainSupportsTransferSrc_ ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
    scInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    scInfo.preTransform = surfCaps.currentTransform;
    scInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    scInfo.presentMode = chosenPresentMode;
    scInfo.clipped = VK_TRUE;

    if (vkCreateSwapchainKHR(dev, &scInfo, nullptr, &swapchain_) != VK_SUCCESS) {
        if (outError) *outError = "vkCreateSwapchainKHR failed";
        return false;
    }

    uint32_t actualCount = 0;
    vkGetSwapchainImagesKHR(dev, swapchain_, &actualCount, nullptr);
    swapchainImages_.resize(actualCount);
    vkGetSwapchainImagesKHR(dev, swapchain_, &actualCount, swapchainImages_.data());

    swapchainImageViews_.resize(actualCount, VK_NULL_HANDLE);
    swapchainRenderFinishedSemaphores_.resize(actualCount, VK_NULL_HANDLE);
    VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < actualCount; ++i) {
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = swapchainImages_[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = swapchainFormat_;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(dev, &viewInfo, nullptr, &swapchainImageViews_[i]) != VK_SUCCESS) {
            if (outError) *outError = "vkCreateImageView for swapchain failed";
            return false;
        }
        if (vkCreateSemaphore(dev, &semInfo, nullptr, &swapchainRenderFinishedSemaphores_[i]) != VK_SUCCESS) {
            if (outError) *outError = "vkCreateSemaphore for swapchain image failed";
            return false;
        }
    }

    hasPresentedSwapchainFrame_ = false;
    return true;
}

void VulkanRenderer::DestroySwapchain() {
    VkDevice dev = ctx_.GetDevice();
    if (dev == VK_NULL_HANDLE) return;
    for (VkSemaphore s : swapchainRenderFinishedSemaphores_) {
        if (s != VK_NULL_HANDLE) vkDestroySemaphore(dev, s, nullptr);
    }
    swapchainRenderFinishedSemaphores_.clear();
    for (VkImageView v : swapchainImageViews_) {
        if (v != VK_NULL_HANDLE) vkDestroyImageView(dev, v, nullptr);
    }
    swapchainImageViews_.clear();
    swapchainImages_.clear();
    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(dev, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
    hasPresentedSwapchainFrame_ = false;
}

bool VulkanRenderer::CreateDescriptorsAndPipelines(std::string* outError) {
    VkDevice dev = ctx_.GetDevice();

    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(dev, &layoutInfo, nullptr, &sceneDescriptorLayout_) != VK_SUCCESS) {
        if (outError) *outError = "vkCreateDescriptorSetLayout failed";
        return false;
    }

    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = 256;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = 256;

    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 256;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    if (vkCreateDescriptorPool(dev, &poolInfo, nullptr, &descriptorPool_) != VK_SUCCESS) {
        if (outError) *outError = "vkCreateDescriptorPool failed";
        return false;
    }

    VkDescriptorSetLayout layouts[VulkanContext::kMaxFramesInFlight] = {
        sceneDescriptorLayout_,
        sceneDescriptorLayout_
    };
    VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.descriptorPool = descriptorPool_;
    allocInfo.descriptorSetCount = VulkanContext::kMaxFramesInFlight;
    allocInfo.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(dev, &allocInfo, sceneDescriptorSets_) != VK_SUCCESS) {
        if (outError) *outError = "vkAllocateDescriptorSets failed";
        return false;
    }

    for (uint32_t i = 0; i < VulkanContext::kMaxFramesInFlight; ++i) {
        VkDescriptorBufferInfo bufInfo{};
        bufInfo.buffer = sceneUniformBuffers_[i].buffer;
        bufInfo.offset = 0;
        bufInfo.range = sizeof(GpuSceneUniforms);

        VkDescriptorImageInfo imgInfo{};
        imgInfo.sampler = linearSampler_;
        imgInfo.imageView = defaultCalibrationTexture_.image.view;
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = sceneDescriptorSets_[i];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &bufInfo;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = sceneDescriptorSets_[i];
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = &imgInfo;

        vkUpdateDescriptorSets(dev, 2, writes, 0, nullptr);
    }

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(GpuObjectPushConstants);

    VkPipelineLayoutCreateInfo plInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &sceneDescriptorLayout_;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(dev, &plInfo, nullptr, &pbrPipelineLayout_) != VK_SUCCESS) {
        if (outError) *outError = "vkCreatePipelineLayout failed";
        return false;
    }

    std::filesystem::path binShaderDir =
#ifdef PLATZ_BINARY_DIR
        std::filesystem::path(PLATZ_BINARY_DIR) / "shaders";
#else
        platform::GetExecutableDir() / "shaders";
#endif
    std::filesystem::path srcShaderDir = platform::GetRepoRootDir() / "shaders";

    CompiledShaderStage pbrVert, pbrFrag, gridVert, gridFrag;
    if (!shaderCompiler_.LoadAndValidateSpvFile(binShaderDir / "pbr_surface.vert.spv", "vertex", "vsMain", &pbrVert, nullptr)) {
        if (!shaderCompiler_.CompileSlangFileToSpirv(srcShaderDir / "pbr_surface.slang", "vsMain", "vertex", &pbrVert, outError)) {
            return false;
        }
    }
    if (!shaderCompiler_.LoadAndValidateSpvFile(binShaderDir / "pbr_surface.frag.spv", "fragment", "fsMain", &pbrFrag, nullptr)) {
        if (!shaderCompiler_.CompileSlangFileToSpirv(srcShaderDir / "pbr_surface.slang", "fsMain", "fragment", &pbrFrag, outError)) {
            return false;
        }
    }
    if (!shaderCompiler_.LoadAndValidateSpvFile(binShaderDir / "grid_sky.vert.spv", "vertex", "vsMain", &gridVert, nullptr)) {
        if (!shaderCompiler_.CompileSlangFileToSpirv(srcShaderDir / "grid_sky.slang", "vsMain", "vertex", &gridVert, outError)) {
            return false;
        }
    }
    if (!shaderCompiler_.LoadAndValidateSpvFile(binShaderDir / "grid_sky.frag.spv", "fragment", "fsMain", &gridFrag, nullptr)) {
        if (!shaderCompiler_.CompileSlangFileToSpirv(srcShaderDir / "grid_sky.slang", "fsMain", "fragment", &gridFrag, outError)) {
            return false;
        }
    }

    shaderCompiler_.RegisterKnownGoodStages("pbr_surface", pbrVert, pbrFrag);
    shaderCompiler_.RegisterKnownGoodStages("grid_sky", gridVert, gridFrag);

    return BuildGraphicsPipelinesFromStages(pbrVert, pbrFrag, gridVert, gridFrag, outError);
}

bool VulkanRenderer::BuildGraphicsPipelinesFromStages(
    const CompiledShaderStage& pbrVert,
    const CompiledShaderStage& pbrFrag,
    const CompiledShaderStage& gridVert,
    const CompiledShaderStage& gridFrag,
    std::string* outError) {
    VkDevice dev = ctx_.GetDevice();

    auto createModule = [&](const CompiledShaderStage& st, VkShaderModule* outMod) -> bool {
        VkShaderModuleCreateInfo smInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smInfo.codeSize = st.spirvWords.size() * sizeof(uint32_t);
        smInfo.pCode = st.spirvWords.data();
        return vkCreateShaderModule(dev, &smInfo, nullptr, outMod) == VK_SUCCESS;
    };

    VkShaderModule pbrVertMod = VK_NULL_HANDLE, pbrFragMod = VK_NULL_HANDLE;
    VkShaderModule gridVertMod = VK_NULL_HANDLE, gridFragMod = VK_NULL_HANDLE;

    if (!createModule(pbrVert, &pbrVertMod) ||
        !createModule(pbrFrag, &pbrFragMod) ||
        !createModule(gridVert, &gridVertMod) ||
        !createModule(gridFrag, &gridFragMod)) {
        if (pbrVertMod) vkDestroyShaderModule(dev, pbrVertMod, nullptr);
        if (pbrFragMod) vkDestroyShaderModule(dev, pbrFragMod, nullptr);
        if (gridVertMod) vkDestroyShaderModule(dev, gridVertMod, nullptr);
        if (gridFragMod) vkDestroyShaderModule(dev, gridFragMod, nullptr);
        if (outError) *outError = "vkCreateShaderModule failed";
        return false;
    }

    const char* pbrVEntry = pbrVert.reflection.entryPointNames.empty() ? "main" : pbrVert.reflection.entryPointNames[0].c_str();
    const char* pbrFEntry = pbrFrag.reflection.entryPointNames.empty() ? "main" : pbrFrag.reflection.entryPointNames[0].c_str();
    const char* gridVEntry = gridVert.reflection.entryPointNames.empty() ? "main" : gridVert.reflection.entryPointNames[0].c_str();
    const char* gridFEntry = gridFrag.reflection.entryPointNames.empty() ? "main" : gridFrag.reflection.entryPointNames[0].c_str();

    VkVertexInputBindingDescription bindingDesc{};
    bindingDesc.binding = 0;
    bindingDesc.stride = sizeof(creative::Vertex3D);
    bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrDescs[4]{};
    attrDescs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(creative::Vertex3D, position)};
    attrDescs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(creative::Vertex3D, normal)};
    attrDescs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT,    offsetof(creative::Vertex3D, uv)};
    attrDescs[3] = {3, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(creative::Vertex3D, color)};

    VkPipelineVertexInputStateCreateInfo viInfo{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    viInfo.vertexBindingDescriptionCount = 1;
    viInfo.pVertexBindingDescriptions = &bindingDesc;
    viInfo.vertexAttributeDescriptionCount = 4;
    viInfo.pVertexAttributeDescriptions = attrDescs;

    VkPipelineInputAssemblyStateCreateInfo iaTriangles{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    iaTriangles.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineInputAssemblyStateCreateInfo iaLines{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    iaLines.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;

    VkPipelineViewportStateCreateInfo vpInfo{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vpInfo.viewportCount = 1;
    vpInfo.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rsInfo{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rsInfo.polygonMode = VK_POLYGON_MODE_FILL;
    rsInfo.cullMode = VK_CULL_MODE_NONE;
    rsInfo.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rsInfo.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo msInfo{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    msInfo.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo dsInfo{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    dsInfo.depthTestEnable = VK_TRUE;
    dsInfo.depthWriteEnable = VK_TRUE;
    dsInfo.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState blendAtts[2]{};
    blendAtts[0].colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAtts[0].blendEnable = VK_FALSE;

    blendAtts[1].colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAtts[1].blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo cbInfo{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbInfo.attachmentCount = 2;
    cbInfo.pAttachments = blendAtts;

    VkDynamicState dynStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynInfo{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynInfo.dynamicStateCount = 2;
    dynInfo.pDynamicStates = dynStates;

    VkFormat colorFormats[2] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R32_UINT};
    VkPipelineRenderingCreateInfo dynRenderInfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    dynRenderInfo.colorAttachmentCount = 2;
    dynRenderInfo.pColorAttachmentFormats = colorFormats;
    dynRenderInfo.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;

    VkPipelineShaderStageCreateInfo pbrStages[2]{};
    pbrStages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, pbrVertMod, pbrVEntry, nullptr};
    pbrStages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, pbrFragMod, pbrFEntry, nullptr};

    VkGraphicsPipelineCreateInfo pbrGpInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pbrGpInfo.pNext = &dynRenderInfo;
    pbrGpInfo.stageCount = 2;
    pbrGpInfo.pStages = pbrStages;
    pbrGpInfo.pVertexInputState = &viInfo;
    pbrGpInfo.pInputAssemblyState = &iaTriangles;
    pbrGpInfo.pViewportState = &vpInfo;
    pbrGpInfo.pRasterizationState = &rsInfo;
    pbrGpInfo.pMultisampleState = &msInfo;
    pbrGpInfo.pDepthStencilState = &dsInfo;
    pbrGpInfo.pColorBlendState = &cbInfo;
    pbrGpInfo.pDynamicState = &dynInfo;
    pbrGpInfo.layout = pbrPipelineLayout_;

    VkPipeline newPbrPipeline = VK_NULL_HANDLE;
    VkResult res1 = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pbrGpInfo, nullptr, &newPbrPipeline);

    VkPipelineShaderStageCreateInfo gridStages[2]{};
    gridStages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, gridVertMod, gridVEntry, nullptr};
    gridStages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, gridFragMod, gridFEntry, nullptr};

    VkGraphicsPipelineCreateInfo gridGpInfo = pbrGpInfo;
    gridGpInfo.pStages = gridStages;
    gridGpInfo.pInputAssemblyState = &iaLines;

    VkPipeline newGridPipeline = VK_NULL_HANDLE;
    VkResult res2 = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gridGpInfo, nullptr, &newGridPipeline);

    vkDestroyShaderModule(dev, pbrVertMod, nullptr);
    vkDestroyShaderModule(dev, pbrFragMod, nullptr);
    vkDestroyShaderModule(dev, gridVertMod, nullptr);
    vkDestroyShaderModule(dev, gridFragMod, nullptr);

    if (res1 != VK_SUCCESS || res2 != VK_SUCCESS) {
        if (newPbrPipeline) vkDestroyPipeline(dev, newPbrPipeline, nullptr);
        if (newGridPipeline) vkDestroyPipeline(dev, newGridPipeline, nullptr);
        if (outError) *outError = "vkCreateGraphicsPipelines failed";
        return false;
    }

    if (pbrPipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(dev, pbrPipeline_, nullptr);
    if (gridPipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(dev, gridPipeline_, nullptr);
    pbrPipeline_ = newPbrPipeline;
    gridPipeline_ = newGridPipeline;
    return true;
}

void VulkanRenderer::DestroyPipelines() {
    VkDevice dev = ctx_.GetDevice();
    if (dev == VK_NULL_HANDLE) return;
    if (pbrPipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, pbrPipeline_, nullptr);
        pbrPipeline_ = VK_NULL_HANDLE;
    }
    if (gridPipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, gridPipeline_, nullptr);
        gridPipeline_ = VK_NULL_HANDLE;
    }
    if (pbrPipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, pbrPipelineLayout_, nullptr);
        pbrPipelineLayout_ = VK_NULL_HANDLE;
    }
}

bool VulkanRenderer::HotReloadPbrShaderFromSlangSource(
    const std::string& slangSourceCode,
    std::string* outDiagnostic) {
    if (!shaderCompiler_.HotReloadSlangSource("pbr_surface", slangSourceCode, "vsMain", "fsMain", outDiagnostic)) {
        return false;
    }
    CompiledShaderStage pbrV, pbrF, gridV, gridF;
    if (!shaderCompiler_.GetActiveStages("pbr_surface", &pbrV, &pbrF) ||
        !shaderCompiler_.GetActiveStages("grid_sky", &gridV, &gridF)) {
        return false;
    }
    vkDeviceWaitIdle(ctx_.GetDevice());
    std::string err;
    if (!BuildGraphicsPipelinesFromStages(pbrV, pbrF, gridV, gridF, &err)) {
        if (outDiagnostic) *outDiagnostic = err;
        return false;
    }
    return true;
}

bool VulkanRenderer::UploadMeshToGpu(
    const creative::MeshData& mesh,
    GpuMeshResource* outResource,
    std::string* outError) {
    if (!outResource || mesh.vertices.empty() || mesh.indices.empty()) {
        if (outError) *outError = "Cannot upload empty MeshData to GPU";
        return false;
    }

    VkDeviceSize vBytes = mesh.vertices.size() * sizeof(creative::Vertex3D);
    VkDeviceSize iBytes = mesh.indices.size() * sizeof(uint32_t);

    VulkanBuffer stagingV, stagingI;
    if (!ctx_.CreateBuffer(
            vBytes,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
            "StagingVertex_" + mesh.name,
            &stagingV,
            outError)) {
        return false;
    }
    if (!ctx_.CreateBuffer(
            iBytes,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
            "StagingIndex_" + mesh.name,
            &stagingI,
            outError)) {
        ctx_.DestroyBufferImmediate(stagingV);
        return false;
    }

    std::memcpy(stagingV.allocInfo.pMappedData, mesh.vertices.data(), vBytes);
    std::memcpy(stagingI.allocInfo.pMappedData, mesh.indices.data(), iBytes);

    GpuMeshResource gpuMesh{};
    gpuMesh.vertexCount = static_cast<uint32_t>(mesh.vertices.size());
    gpuMesh.indexCount = static_cast<uint32_t>(mesh.indices.size());

    if (!ctx_.CreateBuffer(
            vBytes,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            0,
            "DeviceVertex_" + mesh.name,
            &gpuMesh.vertexBuffer,
            outError)) {
        ctx_.DestroyBufferImmediate(stagingV);
        ctx_.DestroyBufferImmediate(stagingI);
        return false;
    }

    if (!ctx_.CreateBuffer(
            iBytes,
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            0,
            "DeviceIndex_" + mesh.name,
            &gpuMesh.indexBuffer,
            outError)) {
        ctx_.DestroyBufferImmediate(stagingV);
        ctx_.DestroyBufferImmediate(stagingI);
        ctx_.DestroyBufferImmediate(gpuMesh.vertexBuffer);
        return false;
    }

    bool copyOk = ctx_.ExecuteImmediateCommands([&](VkCommandBuffer cmd) {
        VkBufferCopy copyV{0, 0, vBytes};
        vkCmdCopyBuffer(cmd, stagingV.buffer, gpuMesh.vertexBuffer.buffer, 1, &copyV);
        VkBufferCopy copyI{0, 0, iBytes};
        vkCmdCopyBuffer(cmd, stagingI.buffer, gpuMesh.indexBuffer.buffer, 1, &copyI);
    }, outError);

    ctx_.DestroyBufferImmediate(stagingV);
    ctx_.DestroyBufferImmediate(stagingI);

    if (!copyOk) {
        ctx_.DestroyBufferImmediate(gpuMesh.vertexBuffer);
        ctx_.DestroyBufferImmediate(gpuMesh.indexBuffer);
        return false;
    }

    if (outResource->vertexBuffer.buffer != VK_NULL_HANDLE) {
        ctx_.DeferDestroyBuffer(currentFrameSlot_, outResource->vertexBuffer);
    }
    if (outResource->indexBuffer.buffer != VK_NULL_HANDLE) {
        ctx_.DeferDestroyBuffer(currentFrameSlot_, outResource->indexBuffer);
    }
    *outResource = std::move(gpuMesh);
    return true;
}

bool VulkanRenderer::UploadTextureToGpu(
    const creative::ImageData2D& img,
    GpuTextureResource* outResource,
    std::string* outError) {
    if (!outResource || img.width == 0 || img.height == 0 || img.rgbaPixels.empty()) {
        if (outError) *outError = "Cannot upload empty ImageData2D to GPU";
        return false;
    }

    VkDeviceSize byteSize = img.rgbaPixels.size();
    VulkanBuffer staging;
    if (!ctx_.CreateBuffer(
            byteSize,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
            "StagingTexture",
            &staging,
            outError)) {
        return false;
    }
    std::memcpy(staging.allocInfo.pMappedData, img.rgbaPixels.data(), byteSize);

    GpuTextureResource gpuTex{};
    gpuTex.sampler = img.pixelExactNearest ? nearestSampler_ : linearSampler_;
    if (!ctx_.CreateImage2D(
            img.width,
            img.height,
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,
            "DeviceTexture2D",
            &gpuTex.image,
            outError)) {
        ctx_.DestroyBufferImmediate(staging);
        return false;
    }

    bool ok = ctx_.ExecuteImmediateCommands([&](VkCommandBuffer cmd) {
        TransitionImageLayout2(
            cmd,
            gpuTex.image.image,
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
            0,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT);

        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {img.width, img.height, 1};
        vkCmdCopyBufferToImage(cmd, staging.buffer, gpuTex.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        TransitionImageLayout2(
            cmd,
            gpuTex.image.image,
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }, outError);

    ctx_.DestroyBufferImmediate(staging);
    if (!ok) {
        ctx_.DestroyImageImmediate(gpuTex.image);
        return false;
    }

    if (outResource->image.image != VK_NULL_HANDLE) {
        ctx_.DeferDestroyImage(currentFrameSlot_, outResource->image);
    }
    *outResource = std::move(gpuTex);
    return true;
}

void VulkanRenderer::InvalidateDerivedGpuCaches() {
    if (ctx_.IsInitialized()) {
        vkDeviceWaitIdle(ctx_.GetDevice());
    }
    for (auto& [id, res] : customAssetMeshes_) {
        ctx_.DestroyBufferImmediate(res.vertexBuffer);
        ctx_.DestroyBufferImmediate(res.indexBuffer);
    }
    customAssetMeshes_.clear();

    for (auto& [id, tex] : customAssetTextures_) {
        ctx_.DestroyImageImmediate(tex.image);
        for (uint32_t i = 0; i < VulkanContext::kMaxFramesInFlight; ++i) {
            if (tex.sceneDescriptorSets[i] != VK_NULL_HANDLE && descriptorPool_ != VK_NULL_HANDLE) {
                vkFreeDescriptorSets(ctx_.GetDevice(), descriptorPool_, 1, &tex.sceneDescriptorSets[i]);
                tex.sceneDescriptorSets[i] = VK_NULL_HANDLE;
            }
        }
    }
    customAssetTextures_.clear();

    for (auto& [id, res] : modifiedObjectMeshes_) {
        ctx_.DestroyBufferImmediate(res.vertexBuffer);
        ctx_.DestroyBufferImmediate(res.indexBuffer);
    }
    modifiedObjectMeshes_.clear();

    lastSyncedProjectGeneration_ = 0;
    derivedCachesDirty_ = true;
}

bool VulkanRenderer::EnsureProjectGpuResourcesSynced(
    const core::ProjectState& project,
    const std::vector<core::SceneObject>& activeObjects,
    std::string* outError) {
    // O(1) steady-state fast path when no canonical project mutation or cache invalidation occurred
    if (!derivedCachesDirty_ && project.GetMutationGeneration() == lastSyncedProjectGeneration_) {
        ++perfMetrics_.gpuSyncSkipsCount;
        return true;
    }

    // 1. Sync content-addressed custom mesh assets
    for (const auto& ast : project.GetAssets()) {
        if (ast.assetType == "mesh") {
            auto it = customAssetMeshes_.find(ast.assetId);
            if (it != customAssetMeshes_.end() && it->second.assetGeneration == ast.generationVersion) {
                continue;
            }
            auto blobOpt = project.GetAssetStore().ReadBlobVerified(ast.contentSha256, outError);
            if (!blobOpt) {
                return false;
            }
            creative::MeshData mesh;
            if (!creative::MeshData::DeserializeFromBinaryBlob(*blobOpt, &mesh, outError)) {
                return false;
            }
            mesh.name = ast.logicalName;
            GpuMeshResource gpuRes{};
            if (!UploadMeshToGpu(mesh, &gpuRes, outError)) {
                return false;
            }
            gpuRes.assetGeneration = ast.generationVersion;
            if (it != customAssetMeshes_.end()) {
                ctx_.DeferDestroyBuffer(currentFrameSlot_, it->second.vertexBuffer);
                ctx_.DeferDestroyBuffer(currentFrameSlot_, it->second.indexBuffer);
            }
            customAssetMeshes_[ast.assetId] = std::move(gpuRes);
            ++perfMetrics_.rebuiltCacheCount;
        } else if (ast.assetType == "texture") {
            auto it = customAssetTextures_.find(ast.assetId);
            if (it != customAssetTextures_.end() && it->second.assetGeneration == ast.generationVersion) {
                continue;
            }
            auto blobOpt = project.GetAssetStore().ReadBlobVerified(ast.contentSha256, outError);
            if (!blobOpt) {
                return false;
            }
            creative::ImageData2D img;
            if (!creative::ImageData2D::DeserializeFromBinaryBlob(*blobOpt, &img, nullptr)) {
                // Graceful fallback for legacy raw RGBA square blobs
                size_t pixelCount = blobOpt->size() / 4;
                uint32_t side = static_cast<uint32_t>(std::round(std::sqrt(static_cast<double>(pixelCount))));
                if (side > 0 && static_cast<size_t>(side) * side * 4 == blobOpt->size()) {
                    img.width = side;
                    img.height = side;
                    img.pixelExactNearest = (side <= 64);
                    img.rgbaPixels = *blobOpt;
                } else {
                    continue;
                }
            }

            VkDescriptorSet reusedSets[VulkanContext::kMaxFramesInFlight]{VK_NULL_HANDLE, VK_NULL_HANDLE};
            if (it != customAssetTextures_.end()) {
                vkDeviceWaitIdle(ctx_.GetDevice());
                for (uint32_t i = 0; i < VulkanContext::kMaxFramesInFlight; ++i) {
                    reusedSets[i] = it->second.sceneDescriptorSets[i];
                }
                ctx_.DestroyImageImmediate(it->second.image);
            }

            GpuTextureResource gpuTex{};
            if (!UploadTextureToGpu(img, &gpuTex, outError)) {
                return false;
            }
            gpuTex.assetGeneration = ast.generationVersion;
            gpuTex.pixelExactNearest = img.pixelExactNearest;

            VkDevice dev = ctx_.GetDevice();
            if (reusedSets[0] == VK_NULL_HANDLE) {
                VkDescriptorSetLayout layouts[VulkanContext::kMaxFramesInFlight] = {
                    sceneDescriptorLayout_,
                    sceneDescriptorLayout_
                };
                VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                allocInfo.descriptorPool = descriptorPool_;
                allocInfo.descriptorSetCount = VulkanContext::kMaxFramesInFlight;
                allocInfo.pSetLayouts = layouts;
                if (vkAllocateDescriptorSets(dev, &allocInfo, reusedSets) != VK_SUCCESS) {
                    ctx_.DestroyImageImmediate(gpuTex.image);
                    if (outError) *outError = "vkAllocateDescriptorSets failed for custom texture asset";
                    return false;
                }
            }

            for (uint32_t i = 0; i < VulkanContext::kMaxFramesInFlight; ++i) {
                gpuTex.sceneDescriptorSets[i] = reusedSets[i];

                VkDescriptorBufferInfo bufInfo{};
                bufInfo.buffer = sceneUniformBuffers_[i].buffer;
                bufInfo.offset = 0;
                bufInfo.range = sizeof(GpuSceneUniforms);

                VkDescriptorImageInfo imgInfo{};
                imgInfo.sampler = img.pixelExactNearest ? nearestSampler_ : linearSampler_;
                imgInfo.imageView = gpuTex.image.view;
                imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                VkWriteDescriptorSet writes[2]{};
                writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[0].dstSet = gpuTex.sceneDescriptorSets[i];
                writes[0].dstBinding = 0;
                writes[0].descriptorCount = 1;
                writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                writes[0].pBufferInfo = &bufInfo;

                writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[1].dstSet = gpuTex.sceneDescriptorSets[i];
                writes[1].dstBinding = 1;
                writes[1].descriptorCount = 1;
                writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[1].pImageInfo = &imgInfo;

                vkUpdateDescriptorSets(dev, 2, writes, 0, nullptr);
            }

            customAssetTextures_[ast.assetId] = std::move(gpuTex);
            ++perfMetrics_.rebuiltCacheCount;
        }
    }

    // 2. Sync per-object CPU-evaluated non-destructive Modifier Stack meshes uploaded to Vulkan VMA device memory
    for (const auto& obj : activeObjects) {
        if (obj.mesh.modifierStack.empty()) {
            auto it = modifiedObjectMeshes_.find(obj.id);
            if (it != modifiedObjectMeshes_.end()) {
                ctx_.DeferDestroyBuffer(currentFrameSlot_, it->second.vertexBuffer);
                ctx_.DeferDestroyBuffer(currentFrameSlot_, it->second.indexBuffer);
                modifiedObjectMeshes_.erase(it);
            }
            continue;
        }

        std::string sig = ComputeModifierStackSignature(obj.mesh.primitiveType, obj.mesh.meshAssetId, obj.mesh.modifierStack);
        auto it = modifiedObjectMeshes_.find(obj.id);
        if (it != modifiedObjectMeshes_.end() && it->second.modifierSignature == sig) {
            continue;
        }

        creative::MeshData baseMesh;
        auto primIt = cpuPrimitiveBaseMeshes_.find(obj.mesh.primitiveType);
        if (primIt != cpuPrimitiveBaseMeshes_.end()) {
            baseMesh = primIt->second;
        } else {
            baseMesh = cpuPrimitiveBaseMeshes_["cube"];
        }

        creative::MeshData evaluated = creative::MeshGenerator::EvaluateModifierStack(baseMesh, obj.mesh.modifierStack);
        evaluated.name = obj.name + "_ModStack";
        GpuMeshResource gpuMod{};
        if (!UploadMeshToGpu(evaluated, &gpuMod, outError)) {
            return false;
        }
        gpuMod.modifierSignature = sig;
        if (it != modifiedObjectMeshes_.end()) {
            ctx_.DeferDestroyBuffer(currentFrameSlot_, it->second.vertexBuffer);
            ctx_.DeferDestroyBuffer(currentFrameSlot_, it->second.indexBuffer);
        }
        modifiedObjectMeshes_[obj.id] = std::move(gpuMod);
        ++perfMetrics_.rebuiltCacheCount;
    }

    lastSyncedProjectGeneration_ = project.GetMutationGeneration();
    derivedCachesDirty_ = false;
    return true;
}

void VulkanRenderer::RecordOffscreenScenePass(
    VkCommandBuffer cmd,
    uint32_t frameSlot,
    const std::vector<core::SceneObject>& activeObjects,
    const core::ProjectState& project,
    const scene::ViewportCamera& camera,
    core::ObjectId selectedObjectId) {
    float aspect = static_cast<float>(viewportWidth_) / static_cast<float>(std::max(1u, viewportHeight_));
    GpuSceneUniforms ubo{};
    ubo.viewProj = camera.GetViewProjectionMatrix(aspect);
    ubo.cameraPos = glm::vec4(camera.GetEyePosition(), camera.GetExposure());

    float azRad = glm::radians(sunAzimuthDeg_);
    float elRad = glm::radians(sunElevationDeg_);
    glm::vec3 defaultSunDir = glm::normalize(glm::vec3(
        std::cos(elRad) * std::sin(azRad),
        std::sin(elRad),
        std::cos(elRad) * std::cos(azRad)));
    ubo.sunDirection = glm::vec4(defaultSunDir, 2.9f);

    for (const auto& obj : activeObjects) {
        if (!obj.visible || !obj.light.enabled) continue;
        if (obj.light.lightType == "directional") {
            ubo.sunDirection = glm::vec4(defaultSunDir, obj.light.intensity);
            ubo.sunColor = glm::vec4(obj.light.color, 0.35f);
        } else if (obj.light.lightType == "point") {
            ubo.pointLightPos = glm::vec4(obj.transform.position, std::max(1.0f, obj.light.range));
            ubo.pointLightColor = glm::vec4(obj.light.color, obj.light.intensity);
        }
    }
    std::memcpy(sceneUniformBuffers_[frameSlot].allocInfo.pMappedData, &ubo, sizeof(GpuSceneUniforms));

    uint32_t queryBase = frameSlot * 2;
    if (timestampQueryPool_ != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(cmd, timestampQueryPool_, queryBase, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestampQueryPool_, queryBase);
    }

    TransitionImageLayout2(
        cmd,
        offscreenColor_.image,
        VK_IMAGE_ASPECT_COLOR_BIT,
        offscreenLayoutInitialized_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        0,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    TransitionImageLayout2(
        cmd,
        offscreenObjectId_.image,
        VK_IMAGE_ASPECT_COLOR_BIT,
        offscreenLayoutInitialized_ ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        0,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    TransitionImageLayout2(
        cmd,
        offscreenDepth_.image,
        VK_IMAGE_ASPECT_DEPTH_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        0,
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo colorAtts[2]{};
    colorAtts[0].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAtts[0].imageView = offscreenColor_.view;
    colorAtts[0].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAtts[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAtts[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAtts[0].clearValue.color = {{0.07f, 0.09f, 0.13f, 1.0f}};

    colorAtts[1].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAtts[1].imageView = offscreenObjectId_.view;
    colorAtts[1].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAtts[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAtts[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAtts[1].clearValue.color.uint32[0] = 0u;

    VkRenderingAttachmentInfo depthAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAtt.imageView = offscreenDepth_.view;
    depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAtt.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo renderInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
    renderInfo.renderArea = {{0, 0}, {viewportWidth_, viewportHeight_}};
    renderInfo.layerCount = 1;
    renderInfo.colorAttachmentCount = 2;
    renderInfo.pColorAttachments = colorAtts;
    renderInfo.pDepthAttachment = &depthAtt;

    vkCmdBeginRendering(cmd, &renderInfo);

    VkViewport vp{0.0f, 0.0f, static_cast<float>(viewportWidth_), static_cast<float>(viewportHeight_), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {viewportWidth_, viewportHeight_}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindDescriptorSets(
        cmd,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        pbrPipelineLayout_,
        0,
        1,
        &sceneDescriptorSets_[frameSlot],
        0,
        nullptr);

    uint32_t drawCalls = 0;
    uint32_t triangles = 0;

    if (gridPipeline_ != VK_NULL_HANDLE && worldGridMesh_.indexCount > 0) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gridPipeline_);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &worldGridMesh_.vertexBuffer.buffer, &offset);
        vkCmdBindIndexBuffer(cmd, worldGridMesh_.indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, worldGridMesh_.indexCount, 1, 0, 0, 0);
        ++drawCalls;
    }

    if (pbrPipeline_ != VK_NULL_HANDLE) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pbrPipeline_);
        VkDescriptorSet currentBoundSet = sceneDescriptorSets_[frameSlot];

        auto findById = [&](core::ObjectId id) -> const core::SceneObject* {
            for (const auto& o : activeObjects) {
                if (o.id == id) return &o;
            }
            return nullptr;
        };

        for (const auto& obj : activeObjects) {
            if (!obj.visible || !obj.mesh.visible) continue;

            const GpuMeshResource* gpuMesh = nullptr;
            auto modIt = modifiedObjectMeshes_.find(obj.id);
            if (modIt != modifiedObjectMeshes_.end() && modIt->second.indexCount > 0) {
                gpuMesh = &modIt->second;
            }
            if (!gpuMesh && obj.mesh.primitiveType == "custom_asset" && obj.mesh.meshAssetId != 0) {
                auto it = customAssetMeshes_.find(obj.mesh.meshAssetId);
                if (it != customAssetMeshes_.end()) {
                    gpuMesh = &it->second;
                }
            }
            if (!gpuMesh) {
                auto it = primitiveMeshes_.find(obj.mesh.primitiveType);
                if (it != primitiveMeshes_.end()) {
                    gpuMesh = &it->second;
                } else {
                    gpuMesh = &primitiveMeshes_["cube"];
                }
            }
            if (!gpuMesh || gpuMesh->indexCount == 0) continue;

            // Bind per-object custom GPU texture descriptor set (e.g. nearest-neighbor pixel art sprite) when assigned
            bool hasCustomTexture = false;
            VkDescriptorSet desiredSet = sceneDescriptorSets_[frameSlot];
            if (obj.material.albedoTextureAssetId != 0) {
                auto texIt = customAssetTextures_.find(obj.material.albedoTextureAssetId);
                if (texIt != customAssetTextures_.end() &&
                    texIt->second.sceneDescriptorSets[frameSlot] != VK_NULL_HANDLE) {
                    desiredSet = texIt->second.sceneDescriptorSets[frameSlot];
                    hasCustomTexture = true;
                }
            }
            if (desiredSet != currentBoundSet) {
                vkCmdBindDescriptorSets(
                    cmd,
                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                    pbrPipelineLayout_,
                    0,
                    1,
                    &desiredSet,
                    0,
                    nullptr);
                currentBoundSet = desiredSet;
            }

            glm::mat4 worldMat = obj.transform.ToLocalMatrix();
            core::ObjectId pid = obj.parentId;
            int guard = 0;
            while (pid != 0 && guard < 32) {
                const core::SceneObject* parent = findById(pid);
                if (!parent || parent->id == obj.id) break;
                worldMat = parent->transform.ToLocalMatrix() * worldMat;
                pid = parent->parentId;
                ++guard;
            }
            (void)project;

            GpuObjectPushConstants pc{};
            pc.modelMatrix = worldMat;
            pc.baseColor = obj.material.baseColor;
            pc.pbrParams = glm::vec4(
                obj.material.metallic,
                obj.material.roughness,
                obj.material.emissiveStrength,
                obj.material.normalScale);
            pc.emissiveColor = glm::vec4(
                obj.material.emissiveColor,
                (obj.id == selectedObjectId) ? 1.0f : 0.0f);
            pc.objectIdLow = static_cast<uint32_t>(obj.id & 0xFFFFFFFFu);
            pc.objectIdHigh = static_cast<uint32_t>((obj.id >> 32) & 0xFFFFFFFFu);
            pc.textureFlags = (obj.material.useProceduralChecker ? 1u : 0u) |
                              (hasCustomTexture ? 2u : 0u);

            vkCmdPushConstants(
                cmd,
                pbrPipelineLayout_,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0,
                sizeof(GpuObjectPushConstants),
                &pc);

            VkDeviceSize vOffset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &gpuMesh->vertexBuffer.buffer, &vOffset);
            vkCmdBindIndexBuffer(cmd, gpuMesh->indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, gpuMesh->indexCount, 1, 0, 0, 0);

            ++drawCalls;
            triangles += gpuMesh->indexCount / 3;
        }
    }

    vkCmdEndRendering(cmd);

    TransitionImageLayout2(
        cmd,
        offscreenColor_.image,
        VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);

    TransitionImageLayout2(
        cmd,
        offscreenObjectId_.image,
        VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_ACCESS_2_TRANSFER_READ_BIT);

    if (timestampQueryPool_ != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestampQueryPool_, queryBase + 1);
    }

    offscreenLayoutInitialized_ = true;
    perfMetrics_.drawCallCount = drawCalls;
    perfMetrics_.triangleCount = triangles;
}

bool VulkanRenderer::RenderSceneOffscreen(
    const core::ProjectState& project,
    const std::vector<core::SceneObject>& activeObjects,
    const scene::ViewportCamera& camera,
    core::ObjectId selectedObjectId,
    std::string* outError) {
    if (!initialized_ || ctx_.IsDeviceLost()) {
        if (outError) *outError = ctx_.IsDeviceLost() ? "VK_ERROR_DEVICE_LOST" : "Renderer not initialized";
        return false;
    }

    auto t0 = std::chrono::steady_clock::now();

    auto tSync0 = std::chrono::steady_clock::now();
    if (!EnsureProjectGpuResourcesSynced(project, activeObjects, outError)) {
        return false;
    }
    auto tSync1 = std::chrono::steady_clock::now();

    VkDevice dev = ctx_.GetDevice();
    uint32_t slot = currentFrameSlot_;
    vkWaitForFences(dev, 1, &frameFences_[slot], VK_TRUE, UINT64_MAX);
    vkResetFences(dev, 1, &frameFences_[slot]);

    ctx_.FlushDeferredDeletionsForFrame(slot);

    auto tRec0 = std::chrono::steady_clock::now();
    VkCommandBuffer cmd = frameCommandBuffers_[slot];
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    RecordOffscreenScenePass(cmd, slot, activeObjects, project, camera, selectedObjectId);

    vkEndCommandBuffer(cmd);
    auto tRec1 = std::chrono::steady_clock::now();

    auto tSub0 = std::chrono::steady_clock::now();
    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    if (vkQueueSubmit(ctx_.GetGraphicsQueue(), 1, &submitInfo, frameFences_[slot]) != VK_SUCCESS) {
        if (outError) *outError = "vkQueueSubmit failed in RenderSceneOffscreen";
        return false;
    }
    auto tSub1 = std::chrono::steady_clock::now();

    auto tWait0 = std::chrono::steady_clock::now();
    vkWaitForFences(dev, 1, &frameFences_[slot], VK_TRUE, UINT64_MAX);
    auto tWait1 = std::chrono::steady_clock::now();

    if (timestampQueryPool_ != VK_NULL_HANDLE) {
        uint64_t timestamps[2]{0, 0};
        if (vkGetQueryPoolResults(
                dev,
                timestampQueryPool_,
                slot * 2,
                2,
                sizeof(timestamps),
                timestamps,
                sizeof(uint64_t),
                VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
            timestamps[1] >= timestamps[0]) {
            double deltaNs = static_cast<double>(timestamps[1] - timestamps[0]) *
                             static_cast<double>(ctx_.GetCapabilities().timestampPeriodNs);
            perfMetrics_.gpuScenePassTimeMs = deltaNs / 1e6;
        }
    }

    auto t1 = std::chrono::steady_clock::now();
    perfMetrics_.cpuSceneSyncMs = std::chrono::duration<double, std::milli>(tSync1 - tSync0).count();
    perfMetrics_.cpuUiBuildMs = 0.0;
    perfMetrics_.cpuCommandRecordMs = std::chrono::duration<double, std::milli>(tRec1 - tRec0).count();
    perfMetrics_.cpuQueueSubmitMs = std::chrono::duration<double, std::milli>(tSub1 - tSub0).count();
    perfMetrics_.cpuPresentWaitMs = std::chrono::duration<double, std::milli>(tWait1 - tWait0).count();
    perfMetrics_.cpuActiveWorkMs =
        perfMetrics_.cpuSceneSyncMs + perfMetrics_.cpuCommandRecordMs + perfMetrics_.cpuQueueSubmitMs;
    perfMetrics_.cpuFrameTimeMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    perfMetrics_.presentModeName = "OFFSCREEN";
    ++perfMetrics_.renderedFrameCount;
    currentFrameSlot_ = (currentFrameSlot_ + 1) % VulkanContext::kMaxFramesInFlight;
    return true;
}

bool VulkanRenderer::RenderInteractiveFrame(
    const core::ProjectState& project,
    const std::vector<core::SceneObject>& activeObjects,
    const scene::ViewportCamera& camera,
    core::ObjectId selectedObjectId,
    const std::function<void()>& buildImGuiCallback,
    std::string* outError) {
    if (!initialized_ || ctx_.IsDeviceLost()) {
        if (outError) *outError = "Renderer not ready";
        return false;
    }
    if (!window_ || window_->IsHeadless()) {
        return RenderSceneOffscreen(project, activeObjects, camera, selectedObjectId, outError);
    }
    if (window_->IsMinimized()) {
        return true;
    }

    auto t0 = std::chrono::steady_clock::now();

    auto tSync0 = std::chrono::steady_clock::now();
    if (!EnsureProjectGpuResourcesSynced(project, activeObjects, outError)) {
        return false;
    }
    auto tSync1 = std::chrono::steady_clock::now();

    if (swapchain_ == VK_NULL_HANDLE) {
        if (!CreateSwapchain(outError) || swapchain_ == VK_NULL_HANDLE) {
            return true;
        }
    }

    auto tWait0 = std::chrono::steady_clock::now();
    VkDevice dev = ctx_.GetDevice();
    uint32_t slot = currentFrameSlot_;
    vkWaitForFences(dev, 1, &frameFences_[slot], VK_TRUE, UINT64_MAX);

    if (timestampQueryPool_ != VK_NULL_HANDLE && perfMetrics_.renderedFrameCount >= VulkanContext::kMaxFramesInFlight) {
        uint64_t ts[2]{0, 0};
        if (vkGetQueryPoolResults(
                dev,
                timestampQueryPool_,
                slot * 2,
                2,
                sizeof(ts),
                ts,
                sizeof(uint64_t),
                VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
            ts[1] >= ts[0]) {
            double deltaNs = static_cast<double>(ts[1] - ts[0]) *
                             static_cast<double>(ctx_.GetCapabilities().timestampPeriodNs);
            perfMetrics_.gpuScenePassTimeMs = deltaNs / 1e6;
        }
    }

    ctx_.FlushDeferredDeletionsForFrame(slot);

    uint32_t swapchainImageIndex = 0;
    VkResult acqRes = vkAcquireNextImageKHR(
        dev,
        swapchain_,
        UINT64_MAX,
        imageAvailableSemaphores_[slot],
        VK_NULL_HANDLE,
        &swapchainImageIndex);
    auto tWait1 = std::chrono::steady_clock::now();

    if (acqRes == VK_ERROR_OUT_OF_DATE_KHR) {
        vkDeviceWaitIdle(dev);
        CreateSwapchain(outError);
        return true;
    }
    if (acqRes != VK_SUCCESS && acqRes != VK_SUBOPTIMAL_KHR) {
        if (outError) *outError = "vkAcquireNextImageKHR failed";
        return false;
    }

    vkResetFences(dev, 1, &frameFences_[slot]);

    auto tUi0 = std::chrono::steady_clock::now();
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    if (buildImGuiCallback) {
        buildImGuiCallback();
    }
    ImGui::Render();
    ImDrawData* drawData = ImGui::GetDrawData();
    auto tUi1 = std::chrono::steady_clock::now();

    auto tRec0 = std::chrono::steady_clock::now();
    VkCommandBuffer cmd = frameCommandBuffers_[slot];
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    RecordOffscreenScenePass(cmd, slot, activeObjects, project, camera, selectedObjectId);

    TransitionImageLayout2(
        cmd,
        swapchainImages_[swapchainImageIndex],
        VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        0,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo scAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    scAtt.imageView = swapchainImageViews_[swapchainImageIndex];
    scAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    scAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    scAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    scAtt.clearValue.color = {{0.055f, 0.065f, 0.085f, 1.0f}};

    VkRenderingInfo scRenderInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
    scRenderInfo.renderArea = {{0, 0}, swapchainExtent_};
    scRenderInfo.layerCount = 1;
    scRenderInfo.colorAttachmentCount = 1;
    scRenderInfo.pColorAttachments = &scAtt;

    vkCmdBeginRendering(cmd, &scRenderInfo);
    if (drawData) {
        ImGui_ImplVulkan_RenderDrawData(drawData, cmd);
    }
    vkCmdEndRendering(cmd);

    // Only pay the ~8 MB swapchain GPU->Host copy cost when a screenshot capture was explicitly requested!
    if (captureNextSwapchainFrame_ && swapchainSupportsTransferSrc_ &&
        swapchainExtent_.width > 0 && swapchainExtent_.height > 0) {
        captureNextSwapchainFrame_ = false;
        VkDeviceSize neededBytes = static_cast<VkDeviceSize>(swapchainExtent_.width) * swapchainExtent_.height * 4ULL;
        if (lastSwapchainCaptureBuffer_.buffer == VK_NULL_HANDLE || lastSwapchainCaptureBuffer_.size != neededBytes) {
            ctx_.DestroyBufferImmediate(lastSwapchainCaptureBuffer_);
            ctx_.CreateBuffer(
                neededBytes,
                VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VMA_MEMORY_USAGE_AUTO,
                VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                "SwapchainFrameCaptureBuffer",
                &lastSwapchainCaptureBuffer_,
                nullptr);
        }
        if (lastSwapchainCaptureBuffer_.buffer != VK_NULL_HANDLE) {
            TransitionImageLayout2(
                cmd,
                swapchainImages_[swapchainImageIndex],
                VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);

            VkBufferImageCopy region{};
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = {swapchainExtent_.width, swapchainExtent_.height, 1};
            vkCmdCopyImageToBuffer(
                cmd,
                swapchainImages_[swapchainImageIndex],
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                lastSwapchainCaptureBuffer_.buffer,
                1,
                &region);

            TransitionImageLayout2(
                cmd,
                swapchainImages_[swapchainImageIndex],
                VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                0);
            lastSwapchainCaptureWidth_ = swapchainExtent_.width;
            lastSwapchainCaptureHeight_ = swapchainExtent_.height;
        } else {
            TransitionImageLayout2(
                cmd,
                swapchainImages_[swapchainImageIndex],
                VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                0);
        }
    } else {
        TransitionImageLayout2(
            cmd,
            swapchainImages_[swapchainImageIndex],
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
            0);
    }

    vkEndCommandBuffer(cmd);
    auto tRec1 = std::chrono::steady_clock::now();

    auto tSub0 = std::chrono::steady_clock::now();
    VkSemaphore renderDoneSem = swapchainRenderFinishedSemaphores_[swapchainImageIndex];
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &imageAvailableSemaphores_[slot];
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &renderDoneSem;

    if (vkQueueSubmit(ctx_.GetGraphicsQueue(), 1, &submitInfo, frameFences_[slot]) != VK_SUCCESS) {
        if (outError) *outError = "vkQueueSubmit failed in RenderInteractiveFrame";
        return false;
    }
    auto tSub1 = std::chrono::steady_clock::now();

    auto tPres0 = std::chrono::steady_clock::now();
    VkPresentInfoKHR presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &renderDoneSem;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain_;
    presentInfo.pImageIndices = &swapchainImageIndex;

    VkResult presRes = vkQueuePresentKHR(ctx_.GetGraphicsQueue(), &presentInfo);
    auto tPres1 = std::chrono::steady_clock::now();

    if (presRes == VK_ERROR_OUT_OF_DATE_KHR || presRes == VK_SUBOPTIMAL_KHR) {
        vkDeviceWaitIdle(dev);
        CreateSwapchain(outError);
    } else {
        lastPresentedSwapchainImageIndex_ = swapchainImageIndex;
        hasPresentedSwapchainFrame_ = true;
    }

    auto t1 = std::chrono::steady_clock::now();
    perfMetrics_.cpuSceneSyncMs = std::chrono::duration<double, std::milli>(tSync1 - tSync0).count();
    perfMetrics_.cpuUiBuildMs = std::chrono::duration<double, std::milli>(tUi1 - tUi0).count();
    perfMetrics_.cpuCommandRecordMs = std::chrono::duration<double, std::milli>(tRec1 - tRec0).count();
    perfMetrics_.cpuQueueSubmitMs = std::chrono::duration<double, std::milli>(tSub1 - tSub0).count();
    perfMetrics_.cpuPresentWaitMs =
        std::chrono::duration<double, std::milli>(tWait1 - tWait0).count() +
        std::chrono::duration<double, std::milli>(tPres1 - tPres0).count();
    perfMetrics_.cpuActiveWorkMs =
        perfMetrics_.cpuSceneSyncMs +
        perfMetrics_.cpuUiBuildMs +
        perfMetrics_.cpuCommandRecordMs +
        perfMetrics_.cpuQueueSubmitMs;
    perfMetrics_.cpuFrameTimeMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    ++perfMetrics_.renderedFrameCount;
    currentFrameSlot_ = (currentFrameSlot_ + 1) % VulkanContext::kMaxFramesInFlight;
    return true;
}

bool VulkanRenderer::ReadbackViewportColorRgba(creative::ImageData2D* outImage, std::string* outError) {
    if (!initialized_ || !offscreenLayoutInitialized_ || !outImage) {
        if (outError) *outError = "Offscreen viewport has not been rendered yet";
        return false;
    }
    vkDeviceWaitIdle(ctx_.GetDevice());

    VkDeviceSize byteSize = static_cast<VkDeviceSize>(viewportWidth_) * viewportHeight_ * 4ULL;
    VulkanBuffer readbackBuf;
    if (!ctx_.CreateBuffer(
            byteSize,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
            "ReadbackColorBuffer",
            &readbackBuf,
            outError)) {
        return false;
    }

    bool ok = ctx_.ExecuteImmediateCommands([&](VkCommandBuffer cmd) {
        TransitionImageLayout2(
            cmd,
            offscreenColor_.image,
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT);

        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {viewportWidth_, viewportHeight_, 1};
        vkCmdCopyImageToBuffer(cmd, offscreenColor_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readbackBuf.buffer, 1, &region);

        TransitionImageLayout2(
            cmd,
            offscreenColor_.image,
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }, outError);

    if (ok) {
        outImage->width = viewportWidth_;
        outImage->height = viewportHeight_;
        outImage->rgbaPixels.resize(static_cast<size_t>(byteSize));
        std::memcpy(outImage->rgbaPixels.data(), readbackBuf.allocInfo.pMappedData, static_cast<size_t>(byteSize));
    }

    ctx_.DestroyBufferImmediate(readbackBuf);
    return ok;
}

bool VulkanRenderer::ReadbackSwapchainWindowRgba(creative::ImageData2D* outImage, std::string* outError) {
    if (!initialized_ || !hasPresentedSwapchainFrame_ ||
        lastSwapchainCaptureBuffer_.buffer == VK_NULL_HANDLE ||
        lastSwapchainCaptureWidth_ == 0 || lastSwapchainCaptureHeight_ == 0 || !outImage) {
        return ReadbackViewportColorRgba(outImage, outError);
    }
    vkDeviceWaitIdle(ctx_.GetDevice());

    uint32_t w = lastSwapchainCaptureWidth_;
    uint32_t h = lastSwapchainCaptureHeight_;
    size_t byteSize = static_cast<size_t>(w) * h * 4ULL;

    outImage->width = w;
    outImage->height = h;
    outImage->rgbaPixels.resize(byteSize);
    std::memcpy(outImage->rgbaPixels.data(), lastSwapchainCaptureBuffer_.allocInfo.pMappedData, byteSize);

    if (swapchainFormat_ == VK_FORMAT_B8G8R8A8_UNORM || swapchainFormat_ == VK_FORMAT_B8G8R8A8_SRGB) {
        for (size_t i = 0; i + 3 < outImage->rgbaPixels.size(); i += 4) {
            std::swap(outImage->rgbaPixels[i], outImage->rgbaPixels[i + 2]);
        }
    }
    return true;
}

bool VulkanRenderer::ReadbackPickedObjectIdAtPixel(
    uint32_t pixelX,
    uint32_t pixelY,
    core::ObjectId* outPickedId,
    std::string* outError) {
    if (!initialized_ || !offscreenLayoutInitialized_) {
        if (outError) *outError = "Offscreen viewport has not been rendered yet";
        return false;
    }
    vkDeviceWaitIdle(ctx_.GetDevice());
    if (pixelX >= viewportWidth_ || pixelY >= viewportHeight_) {
        if (outPickedId) *outPickedId = 0;
        return true;
    }

    VulkanBuffer readbackBuf;
    if (!ctx_.CreateBuffer(
            sizeof(uint32_t),
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
            "ReadbackObjectIdPixel",
            &readbackBuf,
            outError)) {
        return false;
    }

    bool ok = ctx_.ExecuteImmediateCommands([&](VkCommandBuffer cmd) {
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {static_cast<int32_t>(pixelX), static_cast<int32_t>(pixelY), 0};
        region.imageExtent = {1, 1, 1};
        vkCmdCopyImageToBuffer(
            cmd,
            offscreenObjectId_.image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            readbackBuf.buffer,
            1,
            &region);
    }, outError);

    if (ok && outPickedId) {
        uint32_t idLow = 0;
        std::memcpy(&idLow, readbackBuf.allocInfo.pMappedData, sizeof(uint32_t));
        *outPickedId = static_cast<core::ObjectId>(idLow);
    }

    ctx_.DestroyBufferImmediate(readbackBuf);
    return ok;
}

} // namespace platz::engine::vulkan
