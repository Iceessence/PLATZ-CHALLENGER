#include "platform/sdl_window.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

namespace platz::platform {

SdlWindow::~SdlWindow() {
    Shutdown();
}

bool SdlWindow::Initialize(const WindowConfig& config, std::string* outError) {
    Shutdown();
    width_ = config.width;
    height_ = config.height;
    headless_ = config.headless;

    if (headless_) {
        initialized_ = true;
        return true;
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        if (outError) *outError = std::string("SDL_Init failed: ") + SDL_GetError();
        return false;
    }

    SDL_WindowFlags flags = SDL_WINDOW_VULKAN;
    if (config.resizable) flags |= SDL_WINDOW_RESIZABLE;
    if (config.highDpi) flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;

    window_ = SDL_CreateWindow(config.title.c_str(), width_, height_, flags);
    if (!window_) {
        if (outError) *outError = std::string("SDL_CreateWindow failed: ") + SDL_GetError();
        SDL_Quit();
        return false;
    }

    initialized_ = true;
    return true;
}

void SdlWindow::Shutdown() {
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    if (initialized_ && !headless_) {
        SDL_Quit();
    }
    initialized_ = false;
}

void SdlWindow::GetPixelSize(int* outWidth, int* outHeight) const {
    if (window_) {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window_, &w, &h);
        if (outWidth) *outWidth = w;
        if (outHeight) *outHeight = h;
    } else {
        if (outWidth) *outWidth = width_;
        if (outHeight) *outHeight = height_;
    }
}

float SdlWindow::GetDisplayScale() const {
    if (window_) {
        float s = SDL_GetWindowDisplayScale(window_);
        return s > 0.1f ? s : 1.0f;
    }
    return 1.0f;
}

bool SdlWindow::IsMinimized() const {
    if (!window_) return false;
    SDL_WindowFlags flags = SDL_GetWindowFlags(window_);
    int w = 0, h = 0;
    GetPixelSize(&w, &h);
    return ((flags & SDL_WINDOW_MINIMIZED) != 0) || (w <= 0) || (h <= 0);
}

std::vector<const char*> SdlWindow::GetRequiredVulkanInstanceExtensions() const {
    std::vector<const char*> exts;
    if (!window_) return exts;
    Uint32 count = 0;
    const char* const* sdlExts = SDL_Vulkan_GetInstanceExtensions(&count);
    if (sdlExts) {
        for (Uint32 i = 0; i < count; ++i) {
            exts.push_back(sdlExts[i]);
        }
    }
    return exts;
}

bool SdlWindow::CreateVulkanSurface(VkInstance instance, VkSurfaceKHR* outSurface, std::string* outError) const {
    if (!window_) {
        if (outError) *outError = "Cannot create Vulkan surface in headless mode";
        return false;
    }
    if (!SDL_Vulkan_CreateSurface(window_, instance, nullptr, outSurface)) {
        if (outError) *outError = std::string("SDL_Vulkan_CreateSurface failed: ") + SDL_GetError();
        return false;
    }
    return true;
}

std::string SdlWindow::GetSdlVersionString() {
    int ver = SDL_GetVersion();
    int major = SDL_VERSIONNUM_MAJOR(ver);
    int minor = SDL_VERSIONNUM_MINOR(ver);
    int micro = SDL_VERSIONNUM_MICRO(ver);
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(micro);
}

} // namespace platz::platform
