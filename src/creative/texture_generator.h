#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace platz::creative {

struct RgbaColor8 {
    uint8_t r{0};
    uint8_t g{0};
    uint8_t b{0};
    uint8_t a{255};

    bool operator==(const RgbaColor8& other) const = default;
};

struct ImageData2D {
    uint32_t width{0};
    uint32_t height{0};
    bool pixelExactNearest{false}; // True for pixel-art sprites (PLZ-ART-007, PLZ-ARCH-005, N-14)
    std::vector<uint8_t> rgbaPixels; // width * height * 4

    RgbaColor8 GetPixel(uint32_t x, uint32_t y) const;
    void SetPixel(uint32_t x, uint32_t y, RgbaColor8 c);
};

class TextureGenerator {
public:
    // Generates a crisp procedural PBR studio checker/calibration texture
    static ImageData2D GeneratePbrCalibrationTexture(uint32_t width = 256, uint32_t height = 256);

    // Generates an indexed-palette pixel-art sprite (PLZ-ART-007, N-14)
    static ImageData2D GenerateIndexedPixelArtSprite(
        uint32_t width,
        uint32_t height,
        std::span<const RgbaColor8> palette,
        std::span<const uint8_t> paletteIndices);

    // Nearest-neighbor pixel-exact magnification without bilinear blur or HDR tonemap alteration (N-14)
    static ImageData2D SampleNearestNeighborExact(
        const ImageData2D& sourceSprite,
        uint32_t zoomFactor);

    static bool LoadImageFromFile(
        const std::filesystem::path& filePath,
        ImageData2D* outImage,
        std::string* outError = nullptr);

    static bool WriteImageToPng(
        const ImageData2D& image,
        const std::filesystem::path& outputPath,
        std::string* outError = nullptr);
};

} // namespace platz::creative
