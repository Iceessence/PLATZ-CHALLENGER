#include "creative/texture_generator.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace platz::creative {

RgbaColor8 ImageData2D::GetPixel(uint32_t x, uint32_t y) const {
    if (x >= width || y >= height || rgbaPixels.size() < static_cast<size_t>(width) * height * 4) {
        return {};
    }
    size_t idx = (static_cast<size_t>(y) * width + x) * 4;
    return {rgbaPixels[idx], rgbaPixels[idx + 1], rgbaPixels[idx + 2], rgbaPixels[idx + 3]};
}

void ImageData2D::SetPixel(uint32_t x, uint32_t y, RgbaColor8 c) {
    if (x >= width || y >= height || rgbaPixels.size() < static_cast<size_t>(width) * height * 4) {
        return;
    }
    size_t idx = (static_cast<size_t>(y) * width + x) * 4;
    rgbaPixels[idx] = c.r;
    rgbaPixels[idx + 1] = c.g;
    rgbaPixels[idx + 2] = c.b;
    rgbaPixels[idx + 3] = c.a;
}

ImageData2D TextureGenerator::GeneratePbrCalibrationTexture(uint32_t width, uint32_t height) {
    ImageData2D img;
    img.width = width;
    img.height = height;
    img.pixelExactNearest = false;
    img.rgbaPixels.resize(static_cast<size_t>(width) * height * 4);

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t cellX = (x * 8) / width;
            uint32_t cellY = (y * 8) / height;
            bool even = ((cellX + cellY) % 2) == 0;
            bool border = (x % (width / 8) == 0) || (y % (height / 8) == 0);

            RgbaColor8 col;
            if (border) {
                col = {40, 210, 245, 255}; // PZ Cyan accent lines
            } else if (even) {
                col = {225, 230, 238, 255};
            } else {
                col = {148, 156, 168, 255};
            }
            img.SetPixel(x, y, col);
        }
    }
    return img;
}

ImageData2D TextureGenerator::GenerateIndexedPixelArtSprite(
    uint32_t width,
    uint32_t height,
    std::span<const RgbaColor8> palette,
    std::span<const uint8_t> paletteIndices) {
    ImageData2D img;
    img.width = width;
    img.height = height;
    img.pixelExactNearest = true;
    img.rgbaPixels.resize(static_cast<size_t>(width) * height * 4, 0);

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            size_t i = static_cast<size_t>(y) * width + x;
            uint8_t palIdx = (i < paletteIndices.size()) ? paletteIndices[i] : 0;
            RgbaColor8 c = (palIdx < palette.size()) ? palette[palIdx] : RgbaColor8{0, 0, 0, 255};
            img.SetPixel(x, y, c);
        }
    }
    return img;
}

ImageData2D TextureGenerator::SampleNearestNeighborExact(
    const ImageData2D& sourceSprite,
    uint32_t zoomFactor) {
    if (zoomFactor <= 1) return sourceSprite;
    ImageData2D out;
    out.width = sourceSprite.width * zoomFactor;
    out.height = sourceSprite.height * zoomFactor;
    out.pixelExactNearest = true;
    out.rgbaPixels.resize(static_cast<size_t>(out.width) * out.height * 4);

    for (uint32_t y = 0; y < out.height; ++y) {
        for (uint32_t x = 0; x < out.width; ++x) {
            out.SetPixel(x, y, sourceSprite.GetPixel(x / zoomFactor, y / zoomFactor));
        }
    }
    return out;
}

bool TextureGenerator::LoadImageFromFile(
    const std::filesystem::path& filePath,
    ImageData2D* outImage,
    std::string* outError) {
    int w = 0, h = 0, ch = 0;
    unsigned char* pixels = stbi_load(filePath.string().c_str(), &w, &h, &ch, 4);
    if (!pixels || w <= 0 || h <= 0) {
        if (outError) *outError = "stbi_load failed for " + filePath.string();
        if (pixels) stbi_image_free(pixels);
        return false;
    }
    if (outImage) {
        outImage->width = static_cast<uint32_t>(w);
        outImage->height = static_cast<uint32_t>(h);
        outImage->rgbaPixels.assign(pixels, pixels + static_cast<size_t>(w) * h * 4);
    }
    stbi_image_free(pixels);
    return true;
}

bool TextureGenerator::WriteImageToPng(
    const ImageData2D& image,
    const std::filesystem::path& outputPath,
    std::string* outError) {
    if (image.width == 0 || image.height == 0 ||
        image.rgbaPixels.size() < static_cast<size_t>(image.width) * image.height * 4) {
        if (outError) *outError = "Invalid image dimensions for PNG write";
        return false;
    }
    std::error_code ec;
    if (!outputPath.parent_path().empty()) {
        std::filesystem::create_directories(outputPath.parent_path(), ec);
    }
    int ok = stbi_write_png(
        outputPath.string().c_str(),
        static_cast<int>(image.width),
        static_cast<int>(image.height),
        4,
        image.rgbaPixels.data(),
        static_cast<int>(image.width * 4));
    if (!ok) {
        if (outError) *outError = "stbi_write_png failed for " + outputPath.string();
        return false;
    }
    return true;
}

} // namespace platz::creative
