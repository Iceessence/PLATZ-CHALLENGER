#include "engine/render/vulkan/vulkan_context.h"
#include "platform/sdl_window.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace platz::engine::vulkan {

VulkanContext::~VulkanContext() {
    Shutdown();
}

VKAPI_ATTR VkBool32 VKAPI_CALL VulkanContext::DebugUtilsCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
    void* userData) {
    auto* self = static_cast<VulkanContext*>(userData);
    if (!self || !callbackData || !callbackData->pMessage) {
        return VK_FALSE;
    }
    std::lock_guard<std::mutex> lock(self->validationMutex_);
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        ++self->validationErrors_;
        if (self->validationMessages_.size() < 128) {
            self->validationMessages_.push_back(std::string("[VK_ERROR] ") + callbackData->pMessage);
        }
    } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0) {
        ++self->validationWarnings_;
        if (self->validationMessages_.size() < 128) {
            self->validationMessages_.push_back(std::string("[VK_WARN] ") + callbackData->pMessage);
        }
    }
    return VK_FALSE;
}

bool VulkanContext::Initialize(
    const platform::SdlWindow* window,
    bool enableValidationLayers,
    std::string* outError) {
    Shutdown();

    if (volkInitialize() != VK_SUCCESS) {
        if (outError) *outError = "volkInitialize() failed: system vulkan-1.dll not found or failed to load";
        return false;
    }

#ifdef PLATZ_VALIDATION_LAYER_PATH
    if (enableValidationLayers) {
        std::filesystem::path layerDir(PLATZ_VALIDATION_LAYER_PATH);
        if (std::filesystem::exists(layerDir / "VkLayer_khronos_validation.json")) {
            _putenv_s("VK_ADD_LAYER_PATH", layerDir.string().c_str());
        }
    }
#endif

    uint32_t supportedInstanceVersion = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) {
        vkEnumerateInstanceVersion(&supportedInstanceVersion);
    }

    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "PLATZ Studio";
    appInfo.applicationVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
    appInfo.pEngineName = "PLATZ Engine";
    appInfo.engineVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
    appInfo.apiVersion = (supportedInstanceVersion >= VK_API_VERSION_1_4)
                             ? VK_API_VERSION_1_4
                             : VK_API_VERSION_1_3;

    // Enumerate instance extensions
    uint32_t extCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> availExts(extCount);
    if (extCount > 0) {
        vkEnumerateInstanceExtensionProperties(nullptr, &extCount, availExts.data());
    }
    auto hasInstanceExt = [&](const char* name) {
        for (const auto& e : availExts) {
            if (std::strcmp(e.extensionName, name) == 0) return true;
        }
        return false;
    };

    std::vector<const char*> reqInstanceExts;
    if (window && !window->IsHeadless()) {
        reqInstanceExts = window->GetRequiredVulkanInstanceExtensions();
    }
    bool hasDebugUtils = hasInstanceExt(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (hasDebugUtils) {
        reqInstanceExts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    // Check if VK_LAYER_KHRONOS_validation is available
    bool khronosLayerFound = false;
    if (enableValidationLayers) {
        uint32_t layerCount = 0;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        std::vector<VkLayerProperties> availLayers(layerCount);
        if (layerCount > 0) {
            vkEnumerateInstanceLayerProperties(&layerCount, availLayers.data());
        }
        for (const auto& l : availLayers) {
            if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
                khronosLayerFound = true;
                break;
            }
        }
    }

    VkDebugUtilsMessengerCreateInfoEXT dbgCreateInfo{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    dbgCreateInfo.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    dbgCreateInfo.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    dbgCreateInfo.pfnUserCallback = &VulkanContext::DebugUtilsCallback;
    dbgCreateInfo.pUserData = this;

    std::vector<const char*> activeLayers;
    if (enableValidationLayers && khronosLayerFound) {
        activeLayers.push_back("VK_LAYER_KHRONOS_validation");
    }

    VkInstanceCreateInfo instInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instInfo.pApplicationInfo = &appInfo;
    instInfo.enabledExtensionCount = static_cast<uint32_t>(reqInstanceExts.size());
    instInfo.ppEnabledExtensionNames = reqInstanceExts.data();
    instInfo.enabledLayerCount = static_cast<uint32_t>(activeLayers.size());
    instInfo.ppEnabledLayerNames = activeLayers.data();
    if (hasDebugUtils) {
        instInfo.pNext = &dbgCreateInfo;
    }

    VkResult res = vkCreateInstance(&instInfo, nullptr, &instance_);
    if (res != VK_SUCCESS && !activeLayers.empty()) {
        // Fallback if a third-party implicit overlay layer on the host conflicts with explicit layer loading
        activeLayers.clear();
        instInfo.enabledLayerCount = 0;
        instInfo.ppEnabledLayerNames = nullptr;
        res = vkCreateInstance(&instInfo, nullptr, &instance_);
    }
    if (res != VK_SUCCESS || instance_ == VK_NULL_HANDLE) {
        if (outError) *outError = "vkCreateInstance failed with VkResult=" + std::to_string(static_cast<int>(res));
        return false;
    }

    volkLoadInstance(instance_);
    validationLayerActive_ = !activeLayers.empty();

    if (hasDebugUtils && vkCreateDebugUtilsMessengerEXT) {
        vkCreateDebugUtilsMessengerEXT(instance_, &dbgCreateInfo, nullptr, &debugMessenger_);
    }

    if (window && !window->IsHeadless()) {
        if (!window->CreateVulkanSurface(instance_, &surface_, outError)) {
            Shutdown();
            return false;
        }
    }

    // Enumerate physical devices and pick the highest-scoring Vulkan 1.4 / 1.3 capable GPU (Discrete RTX 5080 preferred)
    uint32_t gpuCount = 0;
    vkEnumeratePhysicalDevices(instance_, &gpuCount, nullptr);
    if (gpuCount == 0) {
        if (outError) *outError = "No Vulkan-capable physical devices found";
        Shutdown();
        return false;
    }
    std::vector<VkPhysicalDevice> gpus(gpuCount);
    vkEnumeratePhysicalDevices(instance_, &gpuCount, gpus.data());

    int bestScore = -1;
    uint32_t bestQueueFamily = 0;

    for (VkPhysicalDevice candidate : gpus) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(candidate, &props);

        uint32_t qCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &qCount, nullptr);
        std::vector<VkQueueFamilyProperties> qProps(qCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &qCount, qProps.data());

        int foundGraphicsQueue = -1;
        for (uint32_t q = 0; q < qCount; ++q) {
            bool hasGraphicsAndCompute =
                ((qProps[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) &&
                ((qProps[q].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0);
            if (!hasGraphicsAndCompute) continue;

            if (surface_ != VK_NULL_HANDLE) {
                VkBool32 presentSupport = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(candidate, q, surface_, &presentSupport);
                if (!presentSupport) continue;
            }
            foundGraphicsQueue = static_cast<int>(q);
            break;
        }
        if (foundGraphicsQueue < 0) continue;

        int score = 100;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score += 10000;
        else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score += 1000;
        if (props.apiVersion >= VK_API_VERSION_1_4) score += 500;

        if (score > bestScore) {
            bestScore = score;
            physicalDevice_ = candidate;
            bestQueueFamily = static_cast<uint32_t>(foundGraphicsQueue);
        }
    }

    if (physicalDevice_ == VK_NULL_HANDLE) {
        if (outError) *outError = "No suitable Vulkan physical device with graphics+compute queue found";
        Shutdown();
        return false;
    }
    graphicsQueueFamily_ = bestQueueFamily;

    // Query detailed physical device features & properties
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physicalDevice_, &props);

    VkPhysicalDeviceVulkan12Features feat12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features feat13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    feat13.pNext = &feat12;
    VkPhysicalDeviceFeatures2 feat2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    feat2.pNext = &feat13;
    vkGetPhysicalDeviceFeatures2(physicalDevice_, &feat2);

    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memProps);
    uint64_t totalDeviceLocal = 0;
    for (uint32_t i = 0; i < memProps.memoryHeapCount; ++i) {
        if ((memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
            totalDeviceLocal += memProps.memoryHeaps[i].size;
        }
    }

    caps_.deviceName = props.deviceName;
    caps_.vendorId = props.vendorID;
    caps_.deviceId = props.deviceID;
    caps_.apiVersion = props.apiVersion;
    caps_.driverVersion = props.driverVersion;
    caps_.isDiscreteGpu = (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);
    caps_.supportsVulkan14 = (props.apiVersion >= VK_API_VERSION_1_4);
    caps_.supportsDynamicRendering = (feat13.dynamicRendering == VK_TRUE);
    caps_.supportsSynchronization2 = (feat13.synchronization2 == VK_TRUE);
    caps_.supportsTimelineSemaphore = (feat12.timelineSemaphore == VK_TRUE);
    caps_.supportsBufferDeviceAddress = (feat12.bufferDeviceAddress == VK_TRUE);
    caps_.supportsSamplerAnisotropy = (feat2.features.samplerAnisotropy == VK_TRUE);
    caps_.supportsTimestampQueries = (props.limits.timestampComputeAndGraphics == VK_TRUE);
    caps_.timestampPeriodNs = props.limits.timestampPeriod;
    caps_.deviceLocalVramBytes = totalDeviceLocal;

    if (!caps_.supportsDynamicRendering || !caps_.supportsSynchronization2) {
        if (outError) *outError = "Selected GPU does not support required Vulkan 1.3/1.4 dynamicRendering & synchronization2";
        Shutdown();
        return false;
    }

    // Create logical device
    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo qCreate{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qCreate.queueFamilyIndex = graphicsQueueFamily_;
    qCreate.queueCount = 1;
    qCreate.pQueuePriorities = &queuePriority;

    std::vector<const char*> deviceExts;
    if (surface_ != VK_NULL_HANDLE) {
        deviceExts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }

    VkPhysicalDeviceVulkan12Features enable12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    enable12.timelineSemaphore = caps_.supportsTimelineSemaphore ? VK_TRUE : VK_FALSE;
    enable12.bufferDeviceAddress = caps_.supportsBufferDeviceAddress ? VK_TRUE : VK_FALSE;

    VkPhysicalDeviceVulkan13Features enable13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    enable13.pNext = &enable12;
    enable13.dynamicRendering = VK_TRUE;
    enable13.synchronization2 = VK_TRUE;

    VkPhysicalDeviceFeatures2 enableFeat2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    enableFeat2.pNext = &enable13;
    enableFeat2.features.samplerAnisotropy = caps_.supportsSamplerAnisotropy ? VK_TRUE : VK_FALSE;
    enableFeat2.features.fillModeNonSolid = feat2.features.fillModeNonSolid;

    VkDeviceCreateInfo devCreate{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    devCreate.pNext = &enableFeat2;
    devCreate.queueCreateInfoCount = 1;
    devCreate.pQueueCreateInfos = &qCreate;
    devCreate.enabledExtensionCount = static_cast<uint32_t>(deviceExts.size());
    devCreate.ppEnabledExtensionNames = deviceExts.data();

    res = vkCreateDevice(physicalDevice_, &devCreate, nullptr, &device_);
    if (res != VK_SUCCESS || device_ == VK_NULL_HANDLE) {
        if (outError) *outError = "vkCreateDevice failed with VkResult=" + std::to_string(static_cast<int>(res));
        Shutdown();
        return false;
    }

    volkLoadDevice(device_);
    vkGetDeviceQueue(device_, graphicsQueueFamily_, 0, &graphicsQueue_);

    // Initialize VMA 3.5.0 with Volk dynamic function pointers
    VmaVulkanFunctions vmaFuncs{};
    vmaFuncs.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vmaFuncs.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo allocInfo{};
    allocInfo.vulkanApiVersion = (props.apiVersion >= VK_API_VERSION_1_4) ? VK_API_VERSION_1_4 : VK_API_VERSION_1_3;
    allocInfo.physicalDevice = physicalDevice_;
    allocInfo.device = device_;
    allocInfo.instance = instance_;
    allocInfo.pVulkanFunctions = &vmaFuncs;
    if (caps_.supportsBufferDeviceAddress) {
        allocInfo.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    }

    res = vmaCreateAllocator(&allocInfo, &allocator_);
    if (res != VK_SUCCESS || allocator_ == VK_NULL_HANDLE) {
        if (outError) *outError = "vmaCreateAllocator failed";
        Shutdown();
        return false;
    }

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = graphicsQueueFamily_;
    if (vkCreateCommandPool(device_, &poolInfo, nullptr, &immediateCommandPool_) != VK_SUCCESS) {
        if (outError) *outError = "vkCreateCommandPool failed";
        Shutdown();
        return false;
    }

    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(device_, &fenceInfo, nullptr, &immediateFence_) != VK_SUCCESS) {
        if (outError) *outError = "vkCreateFence failed";
        Shutdown();
        return false;
    }

    initialized_ = true;
    return true;
}

void VulkanContext::Shutdown() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }

    FlushAllDeferredDeletions();

    if (immediateFence_ != VK_NULL_HANDLE) {
        vkDestroyFence(device_, immediateFence_, nullptr);
        immediateFence_ = VK_NULL_HANDLE;
    }
    if (immediateCommandPool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, immediateCommandPool_, nullptr);
        immediateCommandPool_ = VK_NULL_HANDLE;
    }
    if (allocator_ != VK_NULL_HANDLE) {
        vmaDestroyAllocator(allocator_);
        allocator_ = VK_NULL_HANDLE;
    }
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    if (debugMessenger_ != VK_NULL_HANDLE && vkDestroyDebugUtilsMessengerEXT) {
        vkDestroyDebugUtilsMessengerEXT(instance_, debugMessenger_, nullptr);
        debugMessenger_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }

    physicalDevice_ = VK_NULL_HANDLE;
    graphicsQueue_ = VK_NULL_HANDLE;
    initialized_ = false;
}

std::string VulkanContext::GetFormattedApiVersion() const {
    uint32_t v = caps_.apiVersion;
    return std::to_string(VK_API_VERSION_MAJOR(v)) + "." +
           std::to_string(VK_API_VERSION_MINOR(v)) + "." +
           std::to_string(VK_API_VERSION_PATCH(v));
}

uint32_t VulkanContext::GetValidationErrorCount() const {
    std::lock_guard<std::mutex> lock(validationMutex_);
    return validationErrors_;
}

uint32_t VulkanContext::GetValidationWarningCount() const {
    std::lock_guard<std::mutex> lock(validationMutex_);
    return validationWarnings_;
}

std::vector<std::string> VulkanContext::GetValidationMessages() const {
    std::lock_guard<std::mutex> lock(validationMutex_);
    return validationMessages_;
}

void VulkanContext::ClearValidationMessages() {
    std::lock_guard<std::mutex> lock(validationMutex_);
    validationErrors_ = 0;
    validationWarnings_ = 0;
    validationMessages_.clear();
}

bool VulkanContext::CreateBuffer(
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VmaMemoryUsage memoryUsage,
    VmaAllocationCreateFlags vmaFlags,
    const std::string& debugLabel,
    VulkanBuffer* outBuffer,
    std::string* outError) {
    if (!initialized_ || simulatedDeviceLoss_) {
        if (outError) *outError = simulatedDeviceLoss_ ? "VK_ERROR_DEVICE_LOST (simulated N-11)" : "VulkanContext not initialized";
        return false;
    }
    if (size == 0) {
        if (outError) *outError = "Cannot allocate 0-byte VkBuffer";
        return false;
    }

    VkBufferCreateInfo bufInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufInfo.size = size;
    bufInfo.usage = usage;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocCreate{};
    allocCreate.usage = memoryUsage;
    allocCreate.flags = vmaFlags;

    VulkanBuffer res{};
    res.size = size;
    res.debugLabel = debugLabel;

    VkResult vkRes = vmaCreateBuffer(allocator_, &bufInfo, &allocCreate, &res.buffer, &res.allocation, &res.allocInfo);
    if (vkRes != VK_SUCCESS) {
        if (outError) *outError = "vmaCreateBuffer failed with VkResult=" + std::to_string(static_cast<int>(vkRes));
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(budgetMutex_);
        res.generationHandle = nextGenerationHandle_++;
        ++activeAllocationCount_;
        activeAllocatedBytes_ += size;
        peakAllocatedBytes_ = std::max(peakAllocatedBytes_, activeAllocatedBytes_);
    }

    if (outBuffer) *outBuffer = std::move(res);
    return true;
}

void VulkanContext::DestroyBufferImmediate(VulkanBuffer& buf) {
    if (buf.buffer != VK_NULL_HANDLE && allocator_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator_, buf.buffer, buf.allocation);
        std::lock_guard<std::mutex> lock(budgetMutex_);
        if (activeAllocationCount_ > 0) --activeAllocationCount_;
        if (activeAllocatedBytes_ >= buf.size) activeAllocatedBytes_ -= buf.size;
        else activeAllocatedBytes_ = 0;
    }
    buf.buffer = VK_NULL_HANDLE;
    buf.allocation = VK_NULL_HANDLE;
    buf.size = 0;
}

void VulkanContext::DeferDestroyBuffer(uint32_t frameSlot, VulkanBuffer& buf) {
    if (buf.buffer == VK_NULL_HANDLE) return;
    uint32_t slot = frameSlot % kMaxFramesInFlight;
    deferredBuffers_[slot].push_back(buf);
    buf.buffer = VK_NULL_HANDLE;
    buf.allocation = VK_NULL_HANDLE;
    buf.size = 0;
}

bool VulkanContext::CreateImage2D(
    uint32_t width,
    uint32_t height,
    VkFormat format,
    VkImageUsageFlags usage,
    VkImageAspectFlags viewAspectMask,
    const std::string& debugLabel,
    VulkanImage* outImage,
    std::string* outError) {
    if (!initialized_ || simulatedDeviceLoss_) {
        if (outError) *outError = simulatedDeviceLoss_ ? "VK_ERROR_DEVICE_LOST (simulated N-11)" : "VulkanContext not initialized";
        return false;
    }

    VkImageCreateInfo imgInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.format = format;
    imgInfo.extent = {width, height, 1};
    imgInfo.mipLevels = 1;
    imgInfo.arrayLayers = 1;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage = usage;
    imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocCreate{};
    allocCreate.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    VulkanImage res{};
    res.format = format;
    res.width = width;
    res.height = height;
    res.debugLabel = debugLabel;

    VmaAllocationInfo vmaInfo{};
    VkResult vkRes = vmaCreateImage(allocator_, &imgInfo, &allocCreate, &res.image, &res.allocation, &vmaInfo);
    if (vkRes != VK_SUCCESS) {
        if (outError) *outError = "vmaCreateImage failed with VkResult=" + std::to_string(static_cast<int>(vkRes));
        return false;
    }

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = res.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = viewAspectMask;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device_, &viewInfo, nullptr, &res.view) != VK_SUCCESS) {
        vmaDestroyImage(allocator_, res.image, res.allocation);
        if (outError) *outError = "vkCreateImageView failed";
        return false;
    }

    uint64_t approxBytes = static_cast<uint64_t>(width) * height * 4ULL;
    {
        std::lock_guard<std::mutex> lock(budgetMutex_);
        res.generationHandle = nextGenerationHandle_++;
        ++activeAllocationCount_;
        activeAllocatedBytes_ += approxBytes;
        peakAllocatedBytes_ = std::max(peakAllocatedBytes_, activeAllocatedBytes_);
    }

    if (outImage) *outImage = std::move(res);
    return true;
}

void VulkanContext::DestroyImageImmediate(VulkanImage& img) {
    if (img.view != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, img.view, nullptr);
    }
    if (img.image != VK_NULL_HANDLE && allocator_ != VK_NULL_HANDLE) {
        vmaDestroyImage(allocator_, img.image, img.allocation);
        uint64_t approxBytes = static_cast<uint64_t>(img.width) * img.height * 4ULL;
        std::lock_guard<std::mutex> lock(budgetMutex_);
        if (activeAllocationCount_ > 0) --activeAllocationCount_;
        if (activeAllocatedBytes_ >= approxBytes) activeAllocatedBytes_ -= approxBytes;
        else activeAllocatedBytes_ = 0;
    }
    img.view = VK_NULL_HANDLE;
    img.image = VK_NULL_HANDLE;
    img.allocation = VK_NULL_HANDLE;
}

void VulkanContext::DeferDestroyImage(uint32_t frameSlot, VulkanImage& img) {
    if (img.image == VK_NULL_HANDLE && img.view == VK_NULL_HANDLE) return;
    uint32_t slot = frameSlot % kMaxFramesInFlight;
    deferredImages_[slot].push_back(img);
    img.view = VK_NULL_HANDLE;
    img.image = VK_NULL_HANDLE;
    img.allocation = VK_NULL_HANDLE;
}

void VulkanContext::FlushDeferredDeletionsForFrame(uint32_t frameSlot) {
    uint32_t slot = frameSlot % kMaxFramesInFlight;
    for (auto& buf : deferredBuffers_[slot]) {
        DestroyBufferImmediate(buf);
    }
    deferredBuffers_[slot].clear();

    for (auto& img : deferredImages_[slot]) {
        DestroyImageImmediate(img);
    }
    deferredImages_[slot].clear();
}

void VulkanContext::FlushAllDeferredDeletions() {
    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        FlushDeferredDeletionsForFrame(i);
    }
}

bool VulkanContext::ExecuteImmediateCommands(
    const std::function<void(VkCommandBuffer cmd)>& recordFn,
    std::string* outError) {
    if (!initialized_ || simulatedDeviceLoss_) {
        if (outError) *outError = simulatedDeviceLoss_ ? "Device lost" : "Not initialized";
        return false;
    }

    VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool = immediateCommandPool_;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device_, &allocInfo, &cmd) != VK_SUCCESS) {
        if (outError) *outError = "vkAllocateCommandBuffers failed";
        return false;
    }

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    recordFn(cmd);

    vkEndCommandBuffer(cmd);

    vkResetFences(device_, 1, &immediateFence_);
    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    if (vkQueueSubmit(graphicsQueue_, 1, &submitInfo, immediateFence_) != VK_SUCCESS) {
        vkFreeCommandBuffers(device_, immediateCommandPool_, 1, &cmd);
        if (outError) *outError = "vkQueueSubmit failed";
        return false;
    }
    vkWaitForFences(device_, 1, &immediateFence_, VK_TRUE, UINT64_MAX);
    vkFreeCommandBuffers(device_, immediateCommandPool_, 1, &cmd);
    return true;
}

GpuMemoryBudgetSnapshot VulkanContext::GetMemoryBudgetSnapshot() const {
    GpuMemoryBudgetSnapshot snap{};
    {
        std::lock_guard<std::mutex> lock(budgetMutex_);
        snap.activeAllocationCount = activeAllocationCount_;
        snap.activeAllocatedBytes = activeAllocatedBytes_;
        snap.peakAllocatedBytes = peakAllocatedBytes_;
        snap.softRenderBudgetLimitBytes = softRenderBudgetLimitBytes_;
        snap.softAiWorkloadLimitBytes = softAiWorkloadLimitBytes_;
    }

    if (allocator_ != VK_NULL_HANDLE && physicalDevice_ != VK_NULL_HANDLE) {
        VkPhysicalDeviceMemoryProperties memProps{};
        vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memProps);
        VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
        vmaGetHeapBudgets(allocator_, budgets);
        for (uint32_t i = 0; i < memProps.memoryHeapCount; ++i) {
            if ((memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
                snap.totalDeviceHeapBudgetBytes += budgets[i].budget;
                snap.totalDeviceHeapUsageBytes += budgets[i].usage;
            }
        }
    }

    snap.underContentionPressure = (snap.activeAllocatedBytes > snap.softRenderBudgetLimitBytes);
    return snap;
}

void VulkanContext::SetSoftBudgetLimits(uint64_t renderBudgetBytes, uint64_t aiWorkloadBudgetBytes) {
    std::lock_guard<std::mutex> lock(budgetMutex_);
    softRenderBudgetLimitBytes_ = renderBudgetBytes;
    softAiWorkloadLimitBytes_ = aiWorkloadBudgetBytes;
}

bool VulkanContext::AttemptRecoverFromSimulatedDeviceLoss() {
    if (!initialized_) return false;
    vkDeviceWaitIdle(device_);
    simulatedDeviceLoss_ = false;
    return true;
}

} // namespace platz::engine::vulkan
