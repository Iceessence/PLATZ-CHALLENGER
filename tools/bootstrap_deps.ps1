param(
    [string]$RepoRoot = (Resolve-Path "$PSScriptRoot\..").Path
)
$ErrorActionPreference = "Stop"

$lockFile = Join-Path $RepoRoot "dependency-lock.json"
if (-not (Test-Path $lockFile)) {
    throw "Missing dependency-lock.json at $lockFile"
}
$lock = Get-Content $lockFile -Raw | ConvertFrom-Json

$cache = Join-Path $RepoRoot "out\deps_cache"
$ext = Join-Path $RepoRoot "out\deps_extracted"
$tools = Join-Path $RepoRoot "out\tools"
$singleHeaders = Join-Path $ext "single_headers"
New-Item -ItemType Directory -Force -Path $cache, $ext, $tools, $singleHeaders | Out-Null

foreach ($dep in $lock.dependencies) {
    $archivePath = Join-Path $cache $dep.archive_name
    if (-not (Test-Path $archivePath)) {
        Write-Host "[PLATZ Bootstrap] Downloading $($dep.name) ($($dep.version))..."
        Invoke-WebRequest -Uri $dep.url -OutFile $archivePath -UseBasicParsing
    }
    $actualHash = (Get-FileHash -Path $archivePath -Algorithm SHA256).Hash.ToLower()
    if ($actualHash -ne $dep.sha256.ToLower()) {
        throw "[PLATZ Bootstrap] SHA-256 mismatch for $($dep.name): expected $($dep.sha256), got $actualHash"
    }
}

function Ensure-Extracted($ZipName, $DestSubdir, $MarkerRel) {
    $dest = Join-Path $ext $DestSubdir
    $marker = Join-Path $dest $MarkerRel
    if (-not (Test-Path $marker)) {
        Expand-Archive -Path (Join-Path $cache $ZipName) -DestinationPath $dest -Force
    }
}

Ensure-Extracted "SDL3-devel-3.4.20-VC.zip" "sdl3" "SDL3-3.4.20\include\SDL3\SDL.h"
Ensure-Extracted "sqlite-amalgamation-3540100.zip" "sqlite" "sqlite-amalgamation-3540100\sqlite3.h"
Ensure-Extracted "Vulkan-Headers-1.4.363.0.zip" "vulkan_headers" "Vulkan-Headers-vulkan-sdk-1.4.363.0\include\vulkan\vulkan.h"
Ensure-Extracted "volk-1.4.363.0.zip" "volk" "volk-vulkan-sdk-1.4.363.0\volk.h"
Ensure-Extracted "VMA-3.5.0.zip" "vma" "VulkanMemoryAllocator-3.5.0\include\vk_mem_alloc.h"
Ensure-Extracted "imgui-1.93.0-docking.zip" "imgui" "imgui-1.93.0-docking\imgui.h"
Ensure-Extracted "glm-1.0.3.zip" "glm" "glm-1.0.3\glm\glm.hpp"

if (-not (Test-Path (Join-Path $tools "slang\bin\slangc.exe"))) {
    Expand-Archive -Path (Join-Path $cache "slang-2026.22.1-windows-x86_64.zip") -DestinationPath (Join-Path $tools "slang") -Force
}
if (-not (Test-Path (Join-Path $tools "validation_layers\VkLayer_khronos_validation.dll"))) {
    Expand-Archive -Path (Join-Path $cache "Validation_Layers_Windows_64.zip") -DestinationPath (Join-Path $tools "validation_layers") -Force
}

foreach ($hdr in @("json.hpp", "doctest.h", "cgltf.h", "tiny_obj_loader.h", "stb_image.h", "stb_image_write.h")) {
    $dst = Join-Path $singleHeaders $hdr
    if (-not (Test-Path $dst)) {
        Copy-Item (Join-Path $cache $hdr) $dst -Force
    }
}

Write-Host "[PLATZ Bootstrap] All 15 dependencies verified against dependency-lock.json and ready."
