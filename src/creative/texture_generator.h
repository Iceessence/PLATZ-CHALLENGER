#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <json.hpp>

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

// Interactive 2D Pixel-Art & Sprite Layer Canvas (PLZ-ART-001, PLZ-ART-007..009, PLZ-ARCH-005)
struct PixelArtCanvas {
    std::string spriteName{"PZ_Hero_Crystal_Sprite"};
    uint32_t width{16};
    uint32_t height{16};
    std::vector<RgbaColor8> palette;
    std::vector<uint8_t> pixelIndices; // width * height

    PixelArtCanvas(uint32_t w = 16, uint32_t h = 16);

    void InitializeDefaultPaletteAndEmblem();
    uint8_t GetIndex(uint32_t x, uint32_t y) const;
    void SetIndex(uint32_t x, uint32_t y, uint8_t paletteIdx, bool mirrorX = false);
    void FloodFill(uint32_t startX, uint32_t startY, uint8_t newPaletteIdx);
    void StampTilePattern(uint8_t primaryIdx, uint8_t secondaryIdx);
    ImageData2D BakeToImageData2D() const;

    nlohmann::json ToJson() const;
    static PixelArtCanvas FromJson(const nlohmann::json& j);
};

// Visual Material Node Graph -> Slang Shader Code Generator & Preview Baker (PLZ-MAT-001, PLZ-MAT-003, PLZ-RENDERX-002)
struct MaterialGraphNode {
    std::string nodeId{"node_1"};
    std::string nodeType{"PbrOutput"}; // "AlbedoColor", "CheckerPattern", "FresnelRim", "WaveBands", "PbrOutput"
    glm::vec3 colorParam{0.15f, 0.82f, 0.96f};
    float scalarA{8.0f}; // e.g., UV scale or Fresnel power or Metallic
    float scalarB{0.35f}; // e.g., pattern contrast or Roughness
    std::string inputNodeA;
    std::string inputNodeB;
};

struct MaterialGraphValidationResult {
    bool valid{false};
    std::string failingNodeId;
    std::string diagnosticMessage;
};

class MaterialGraphDocument {
public:
    MaterialGraphDocument();

    void BuildDefaultProceduralGraph();
    const std::vector<MaterialGraphNode>& GetNodes() const { return nodes_; }
    std::vector<MaterialGraphNode>& GetNodesMutable() { return nodes_; }
    MaterialGraphNode* FindNode(const std::string& nodeId);
    const MaterialGraphNode* FindNode(const std::string& nodeId) const;

    // Checks for missing output node, broken links, or graph cycles (PLZ-RENDERX-002)
    MaterialGraphValidationResult ValidateGraph() const;

    // Compiles the visual material node graph into a full Slang 2026.22.1 shader module (PLZ-MAT-001, PLZ-MAT-006)
    bool CompileToSlangShaderSource(
        std::string* outSlangSource,
        std::string* outFailingNodeId = nullptr,
        std::string* outError = nullptr) const;

    // Evaluates the material graph on CPU to produce a 2D preview/albedo texture
    ImageData2D BakeGraphAlbedoTexture(uint32_t width = 128, uint32_t height = 128) const;

    std::string ToJsonString() const;
    static MaterialGraphDocument FromJsonString(const std::string& jsonStr);

private:
    std::vector<MaterialGraphNode> nodes_;
};

class TextureGenerator {
public:
    static ImageData2D GeneratePbrCalibrationTexture(uint32_t width = 256, uint32_t height = 256);

    static ImageData2D GenerateIndexedPixelArtSprite(
        uint32_t width,
        uint32_t height,
        std::span<const RgbaColor8> palette,
        std::span<const uint8_t> paletteIndices);

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
