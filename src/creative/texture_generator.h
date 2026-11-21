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

    // Binary serialization for content-addressed texture blobs
    std::vector<uint8_t> SerializeToBinaryBlob() const;
    static bool DeserializeFromBinaryBlob(
        std::span<const uint8_t> blob,
        ImageData2D* outImg,
        std::string* outError = nullptr);
};

// Editable Layer for Multi-Layer Non-Flattened 2D Pixel-Art Canvas (PLZ-ART-001, PLZ-ART-007, PLZ-CORE-001)
constexpr uint8_t kTransparentPixelIndex = 255u;

struct PixelArtLayer {
    std::string layerName{"BaseLayer"};
    bool visible{true};
    float opacity{1.0f};
    std::vector<uint8_t> pixelIndices; // width * height (255 = transparent)
};

struct PixelArtCanvas {
    std::string spriteName{"PZ_Hero_Crystal_Sprite"};
    uint32_t width{16};
    uint32_t height{16};
    std::vector<RgbaColor8> palette;
    std::vector<PixelArtLayer> layers;
    uint32_t activeLayerIndex{0};

    PixelArtCanvas(uint32_t w = 16, uint32_t h = 16);

    void InitializeDefaultPaletteAndEmblem();
    uint32_t AddLayer(const std::string& name);
    bool RemoveLayer(uint32_t layerIdx);
    PixelArtLayer& GetActiveLayer();
    const PixelArtLayer& GetActiveLayer() const;

    // Reads active layer pixel index (or 255 if transparent)
    uint8_t GetIndex(uint32_t x, uint32_t y) const;
    uint8_t GetLayerIndex(uint32_t layerIdx, uint32_t x, uint32_t y) const;
    void SetIndex(uint32_t x, uint32_t y, uint8_t paletteIdx, bool mirrorX = false);
    void ErasePixel(uint32_t x, uint32_t y, bool mirrorX = false);
    void FloodFill(uint32_t startX, uint32_t startY, uint8_t newPaletteIdx);
    void StampTilePattern(uint8_t primaryIdx, uint8_t secondaryIdx);

    // Composites all visible editable layers into a pixel-exact derived RGBA8 texture without mutating layers
    ImageData2D BakeToImageData2D() const;

    nlohmann::json ToJson() const;
    std::string ToJsonString() const;
    static PixelArtCanvas FromJson(const nlohmann::json& j);
    static PixelArtCanvas FromJsonString(const std::string& jsonStr);
};

// ============================================================================
// Expandable Validated DAG Visual Material Node Graph -> Slang 2026.22.1 Compiler
// (PLZ-MAT-001, PLZ-MAT-003, PLZ-MAT-006, PLZ-RENDERX-002, PLZ-CREATEX-004)
// ============================================================================

struct MaterialGraphNode {
    std::string nodeId{"node_1"};
    // Supported node types:
    //  "AlbedoColor"    : Constant color (colorParam)
    //  "CheckerPattern" : Procedural UV checker mixing inputNodeA with colorParam (scalarA = freq, scalarB = mixWeight)
    //  "WaveBands"      : Procedural sine-wave bands mixing inputNodeA with colorParam (scalarA = freq, scalarB = contrast)
    //  "HeightGradient" : World-Y height gradient mixing inputNodeA with colorParam (scalarA = minY, scalarB = maxY)
    //  "NoisePattern"   : Procedural tri-harmonic noise modulating inputNodeA with colorParam (scalarA = scale, scalarB = strength)
    //  "BlendMix"       : Linearly blends inputNodeA and inputNodeB (scalarA = factor [0..1])
    //  "FresnelRim"     : View-dependent grazing rim highlight over inputNodeA (colorParam = tint, scalarA = power, scalarB = intensity)
    //  "PbrOutput"      : Terminal surface node (inputNodeA = albedo/color, inputNodeB = optional emissive/rim, scalarA = metallic, scalarB = roughness)
    std::string nodeType{"PbrOutput"};
    glm::vec3 colorParam{0.15f, 0.82f, 0.96f};
    float scalarA{8.0f};
    float scalarB{0.35f};
    std::string inputNodeA;
    std::string inputNodeB;
};

struct MaterialGraphValidationResult {
    bool valid{false};
    std::string failingNodeId;
    std::string diagnosticMessage;
    std::vector<std::string> topologicalOrder; // Evaluation order ending at PbrOutput
};

class MaterialGraphDocument {
public:
    MaterialGraphDocument();

    void BuildDefaultProceduralGraph();
    const std::vector<MaterialGraphNode>& GetNodes() const { return nodes_; }
    std::vector<MaterialGraphNode>& GetNodesMutable() { return nodes_; }
    MaterialGraphNode* FindNode(const std::string& nodeId);
    const MaterialGraphNode* FindNode(const std::string& nodeId) const;

    // Graph editing API (supports arbitrary node insertion, removal, and validated rewiring)
    std::string AddNode(
        const std::string& nodeType,
        const glm::vec3& colorParam = {0.2f, 0.85f, 0.95f},
        float scalarA = 6.0f,
        float scalarB = 0.35f,
        const std::string& inputNodeA = {},
        const std::string& inputNodeB = {});
    bool RemoveNode(const std::string& nodeId, std::string* outError = nullptr);
    bool ConnectNodes(
        const std::string& sourceNodeId,
        const std::string& targetNodeId,
        int targetPortIndex, // 0 = inputNodeA, 1 = inputNodeB
        std::string* outError = nullptr);

    // Full DAG validation: missing PbrOutput, missing links, self-links, unknown types, and DFS cycle detection (PLZ-RENDERX-002)
    MaterialGraphValidationResult ValidateGraph() const;

    // Compiles the topological DAG connections directly into Slang 2026.22.1 shader source code
    bool CompileToSlangShaderSource(
        std::string* outSlangSource,
        std::string* outFailingNodeId = nullptr,
        std::string* outError = nullptr) const;

    // Evaluates the actual connected DAG on CPU to bake a live 2D preview/albedo texture
    ImageData2D BakeGraphAlbedoTexture(uint32_t width = 128, uint32_t height = 128) const;

    std::string ToJsonString() const;
    static MaterialGraphDocument FromJsonString(const std::string& jsonStr);

private:
    static bool IsSupportedNodeType(const std::string& nodeType);
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
