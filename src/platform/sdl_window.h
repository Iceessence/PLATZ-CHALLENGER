#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <volk.h>

struct SDL_Window;

namespace platz::platform {

struct WindowConfig {
    std::string title{"PLATZ Studio — One Place. Zero Limits."};
    int width{1600};
    int height{920};
    bool resizable{true};
    bool highDpi{true};
    bool headless{false};
};

class SdlWindow {
public:
    SdlWindow() = default;
    ~SdlWindow();

    SdlWindow(const SdlWindow&) = delete;
    SdlWindow& operator=(const SdlWindow&) = delete;

    bool Initialize(const WindowConfig& config, std::string* outError = nullptr);
    void Shutdown();

    bool IsInitialized() const { return initialized_; }
    bool IsHeadless() const { return headless_; }
    SDL_Window* GetNativeHandle() const { return window_; }

    void GetPixelSize(int* outWidth, int* outHeight) const;
    float GetDisplayScale() const;
    bool IsMinimized() const;

    std::vector<const char*> GetRequiredVulkanInstanceExtensions() const;
    bool CreateVulkanSurface(VkInstance instance, VkSurfaceKHR* outSurface, std::string* outError = nullptr) const;

    static std::string GetSdlVersionString();

private:
    SDL_Window* window_{nullptr};
    int width_{1600};
    int height_{920};
    bool initialized_{false};
    bool headless_{false};
};

} // namespace platz::platform
