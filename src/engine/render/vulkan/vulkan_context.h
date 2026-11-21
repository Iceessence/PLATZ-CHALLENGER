#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <volk.h>
#include <vk_mem_alloc.h>

namespace platz::platform {
class SdlWindow;
}

namespace platz::engine::vulkan {

struct GpuDeviceCapabilities {
    std::string deviceName;
    uint32_t vendorId{0};
    uint32_t deviceId{0};
    uint32_t apiVersion{0};
    uint32_t driverVersion{0};
    bool isDiscreteGpu{false};
    bool supportsVulkan14{false};
    bool supportsDynamicRendering{false};
    bool supportsSynchronization2{false};
    bool supportsTimelineSemaphore{false};
    bool supportsBufferDeviceAddress{false};
    bool supportsSamplerAnisotropy{false};
    bool supportsTimestampQueries{false};
    float timestampPeriodNs{1.0f};
    uint64_t deviceLocalVramBytes{0};
};

struct GpuMemoryBudgetSnapshot {
    uint64_t activeAllocationCount{0};
    uint64_t activeAllocatedBytes{0};
    uint64_t peakAllocatedBytes{0};
    uint64_t totalDeviceHeapBudgetBytes{0};
    uint64_t totalDeviceHeapUsageBytes{0};
    uint64_t softRenderBudgetLimitBytes{0};
    uint64_t softAiWorkloadLimitBytes{0};
    bool underContentionPressure{false};
};

// Move-only RAII wrapper for VMA-allocated VkBuffer
struct VulkanBuffer {
    VkBuffer buffer{VK_NULL_HANDLE};
    VmaAllocation allocation{VK_NULL_HANDLE};
    VmaAllocationInfo allocInfo{};
    VkDeviceSize size{0};
    uint64_t generationHandle{0};
    std::string debugLabel;
};

// Move-only RAII wrapper for VMA-allocated VkImage + VkImageView
struct VulkanImage {
    VkImage image{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};
    VmaAllocation allocation{VK_NULL_HANDLE};
    VkFormat format{VK_FORMAT_UNDEFINED};
    uint32_t width{0};
    uint32_t height{0};
    uint64_t generationHandle{0};
    std::string debugLabel;
};

class VulkanContext {
public:
    static constexpr uint32_t kMaxFramesInFlight = 2;

    VulkanContext() = default;
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    bool Initialize(
        const platform::SdlWindow* window,
        bool enableValidationLayers,
        std::string* outError = nullptr);

    void Shutdown();

    bool IsInitialized() const { return initialized_; }
    VkInstance GetInstance() const { return instance_; }
    VkPhysicalDevice GetPhysicalDevice() const { return physicalDevice_; }
    VkDevice GetDevice() const { return device_; }
    VkQueue GetGraphicsQueue() const { return graphicsQueue_; }
    uint32_t GetGraphicsQueueFamily() const { return graphicsQueueFamily_; }
    VkSurfaceKHR GetSurface() const { return surface_; }
    VmaAllocator GetVmaAllocator() const { return allocator_; }
    VkCommandPool GetImmediateCommandPool() const { return immediateCommandPool_; }

    const GpuDeviceCapabilities& GetCapabilities() const { return caps_; }
    std::string GetFormattedApiVersion() const;

    // Validation & Debug Messenger Inspection
    bool IsValidationActive() const { return validationLayerActive_; }
    uint32_t GetValidationErrorCount() const;
    uint32_t GetValidationWarningCount() const;
    std::vector<std::string> GetValidationMessages() const;
    void ClearValidationMessages();

    // VMA Resource Allocation & Budget Tracking (PLZ-RENDERX-006, N-11, N-12)
    bool CreateBuffer(
        VkDeviceSize size,
        VkBufferUsageFlags usage,
        VmaMemoryUsage memoryUsage,
        VmaAllocationCreateFlags vmaFlags,
        const std::string& debugLabel,
        VulkanBuffer* outBuffer,
        std::string* outError = nullptr);

    void DestroyBufferImmediate(VulkanBuffer& buf);
    void DeferDestroyBuffer(uint32_t frameSlot, VulkanBuffer& buf);

    bool CreateImage2D(
        uint32_t width,
        uint32_t height,
        VkFormat format,
        VkImageUsageFlags usage,
        VkImageAspectFlags viewAspectMask,
        const std::string& debugLabel,
        VulkanImage* outImage,
        std::string* outError = nullptr);

    void DestroyImageImmediate(VulkanImage& img);
    void DeferDestroyImage(uint32_t frameSlot, VulkanImage& img);

    // Flushes deferred destruction queue for completed frameSlot (called after frame fence signals)
    void FlushDeferredDeletionsForFrame(uint32_t frameSlot);
    void FlushAllDeferredDeletions();

    // Immediate single-time command buffer submission for staging uploads / readbacks
    bool ExecuteImmediateCommands(
        const std::function<void(VkCommandBuffer cmd)>& recordFn,
        std::string* outError = nullptr);

    GpuMemoryBudgetSnapshot GetMemoryBudgetSnapshot() const;
    void SetSoftBudgetLimits(uint64_t renderBudgetBytes, uint64_t aiWorkloadBudgetBytes);

    // Device-loss & OOM contention simulation hooks for negative tests N-11 & N-12
    void SimulateDeviceLossForTesting(bool simulatedLoss) { simulatedDeviceLoss_ = simulatedLoss; }
    bool IsDeviceLost() const { return simulatedDeviceLoss_; }
    bool AttemptRecoverFromSimulatedDeviceLoss();

private:
    static VKAPI_ATTR VkBool32 VKAPI_CALL DebugUtilsCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT types,
        const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
        void* userData);

    bool initialized_{false};
    bool validationLayerActive_{false};
    bool simulatedDeviceLoss_{false};

    VkInstance instance_{VK_NULL_HANDLE};
    VkDebugUtilsMessengerEXT debugMessenger_{VK_NULL_HANDLE};
    VkSurfaceKHR surface_{VK_NULL_HANDLE};
    VkPhysicalDevice physicalDevice_{VK_NULL_HANDLE};
    VkDevice device_{VK_NULL_HANDLE};
    VkQueue graphicsQueue_{VK_NULL_HANDLE};
    uint32_t graphicsQueueFamily_{0};
    VmaAllocator allocator_{VK_NULL_HANDLE};
    VkCommandPool immediateCommandPool_{VK_NULL_HANDLE};
    VkFence immediateFence_{VK_NULL_HANDLE};

    GpuDeviceCapabilities caps_{};

    mutable std::mutex budgetMutex_;
    uint64_t activeAllocationCount_{0};
    uint64_t activeAllocatedBytes_{0};
    uint64_t peakAllocatedBytes_{0};
    uint64_t nextGenerationHandle_{1};
    uint64_t softRenderBudgetLimitBytes_{512ULL * 1024ULL * 1024ULL};
    uint64_t softAiWorkloadLimitBytes_{256ULL * 1024ULL * 1024ULL};

    std::vector<VulkanBuffer> deferredBuffers_[kMaxFramesInFlight];
    std::vector<VulkanImage> deferredImages_[kMaxFramesInFlight];

    mutable std::mutex validationMutex_;
    uint32_t validationErrors_{0};
    uint32_t validationWarnings_{0};
    std::vector<std::string> validationMessages_;
};

} // namespace platz::engine::vulkan
