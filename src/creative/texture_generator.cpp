#include "creative/texture_generator.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <queue>
#include <set>
#include <sstream>

namespace platz::creative {

namespace {
constexpr uint32_t kPlatzTextureMagic = 0x545A4C50u; // 'PLZT'
constexpr uint32_t kPlatzTextureVersion = 1u;

std::string SanitizeSlangIdent(const std::string& raw) {
    std::string out = "n_";
    for (char c : raw) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') {
            out.push_back(c);
        } else {
            out.push_back('_');
        }
    }
    return out;
}

} // namespace

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

std::vector<uint8_t> ImageData2D::SerializeToBinaryBlob() const {
    uint32_t header[5] = {
        kPlatzTextureMagic,
        kPlatzTextureVersion,
        width,
        height,
        pixelExactNearest ? 1u : 0u
    };
    std::vector<uint8_t> out(sizeof(header) + rgbaPixels.size());
    std::memcpy(out.data(), header, sizeof(header));
    if (!rgbaPixels.empty()) {
        std::memcpy(out.data() + sizeof(header), rgbaPixels.data(), rgbaPixels.size());
    }
    return out;
}

bool ImageData2D::DeserializeFromBinaryBlob(
    std::span<const uint8_t> blob,
    ImageData2D* outImg,
    std::string* outError) {
    if (blob.size() < sizeof(uint32_t) * 5) {
        if (outError) *outError = "Texture blob too small for header";
        return false;
    }
    uint32_t header[5]{};
    std::memcpy(header, blob.data(), sizeof(header));
    if (header[0] != kPlatzTextureMagic || header[1] != kPlatzTextureVersion) {
        if (outError) *outError = "Invalid PLATZ texture binary magic or version";
        return false;
    }
    uint32_t w = header[2];
    uint32_t h = header[3];
    size_t expectedPixels = static_cast<size_t>(w) * h * 4ULL;
    if (blob.size() != sizeof(header) + expectedPixels) {
        if (outError) *outError = "Texture blob byte size mismatch";
        return false;
    }
    if (outImg) {
        outImg->width = w;
        outImg->height = h;
        outImg->pixelExactNearest = (header[4] != 0u);
        outImg->rgbaPixels.assign(blob.data() + sizeof(header), blob.data() + blob.size());
    }
    return true;
}

// ============================================================================
// Multi-Layer Editable PixelArtCanvas Implementation (PLZ-ART-001, PLZ-ART-007)
// ============================================================================

PixelArtCanvas::PixelArtCanvas(uint32_t w, uint32_t h)
    : width(std::max(4u, w)), height(std::max(4u, h)) {
    InitializeDefaultPaletteAndEmblem();
}

void PixelArtCanvas::InitializeDefaultPaletteAndEmblem() {
    palette = {
        {14,  18,  28, 255}, // 0: Obsidian Navy Background
        {0,  212, 245, 255}, // 1: PZ Electric Cyan
        {248, 198,  48, 255}, // 2: Solar Gold
        {235, 242, 250, 255}, // 3: Crisp White
        {42,  218, 132, 255}, // 4: Emerald Pulse
        {238,  68,  84, 255}, // 5: Crimson Alert
        {138,  92, 246, 255}, // 6: Amethyst Violet
        {72,   84, 106, 255}  // 7: Steel Slate
    };

    layers.clear();
    PixelArtLayer bgLayer;
    bgLayer.layerName = "BackgroundFrame";
    bgLayer.visible = true;
    bgLayer.opacity = 1.0f;
    bgLayer.pixelIndices.assign(static_cast<size_t>(width) * height, 0);

    PixelArtLayer emblemLayer;
    emblemLayer.layerName = "CrystalEmblem";
    emblemLayer.visible = true;
    emblemLayer.opacity = 1.0f;
    emblemLayer.pixelIndices.assign(static_cast<size_t>(width) * height, kTransparentPixelIndex);

    float cx = (static_cast<float>(width) - 1.0f) * 0.5f;
    float cy = (static_cast<float>(height) - 1.0f) * 0.5f;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            size_t idx = static_cast<size_t>(y) * width + x;
            if (x == 0 || y == 0 || x == width - 1 || y == height - 1) {
                bgLayer.pixelIndices[idx] = 7;
            }
            float dx = std::abs(static_cast<float>(x) - cx);
            float dy = std::abs(static_cast<float>(y) - cy);
            float manhattan = dx + dy;
            if (manhattan < 2.2f) {
                emblemLayer.pixelIndices[idx] = 3;
            } else if (manhattan < 4.5f) {
                emblemLayer.pixelIndices[idx] = 2;
            } else if (manhattan < 6.8f) {
                emblemLayer.pixelIndices[idx] = 1;
            }
        }
    }

    layers.push_back(std::move(bgLayer));
    layers.push_back(std::move(emblemLayer));
    activeLayerIndex = 1;
}

uint32_t PixelArtCanvas::AddLayer(const std::string& name) {
    PixelArtLayer layer;
    layer.layerName = name.empty() ? ("Layer_" + std::to_string(layers.size() + 1)) : name;
    layer.visible = true;
    layer.opacity = 1.0f;
    layer.pixelIndices.assign(static_cast<size_t>(width) * height, kTransparentPixelIndex);
    layers.push_back(std::move(layer));
    activeLayerIndex = static_cast<uint32_t>(layers.size() - 1);
    return activeLayerIndex;
}

bool PixelArtCanvas::RemoveLayer(uint32_t layerIdx) {
    if (layers.size() <= 1 || layerIdx >= layers.size()) return false;
    layers.erase(layers.begin() + layerIdx);
    if (activeLayerIndex >= layers.size()) {
        activeLayerIndex = static_cast<uint32_t>(layers.size() - 1);
    }
    return true;
}

PixelArtLayer& PixelArtCanvas::GetActiveLayer() {
    if (layers.empty()) InitializeDefaultPaletteAndEmblem();
    if (activeLayerIndex >= layers.size()) activeLayerIndex = static_cast<uint32_t>(layers.size() - 1);
    return layers[activeLayerIndex];
}

const PixelArtLayer& PixelArtCanvas::GetActiveLayer() const {
    uint32_t idx = std::min(activeLayerIndex, static_cast<uint32_t>(layers.size() - 1));
    return layers[idx];
}

uint8_t PixelArtCanvas::GetIndex(uint32_t x, uint32_t y) const {
    if (layers.empty() || x >= width || y >= height) return 0;
    // Composite visible layers top-down for effective visible palette index
    for (int l = static_cast<int>(layers.size()) - 1; l >= 0; --l) {
        if (!layers[l].visible) continue;
        uint8_t v = layers[l].pixelIndices[static_cast<size_t>(y) * width + x];
        if (v != kTransparentPixelIndex) return v;
    }
    return 0;
}

uint8_t PixelArtCanvas::GetLayerIndex(uint32_t layerIdx, uint32_t x, uint32_t y) const {
    if (layerIdx >= layers.size() || x >= width || y >= height) return kTransparentPixelIndex;
    return layers[layerIdx].pixelIndices[static_cast<size_t>(y) * width + x];
}

void PixelArtCanvas::SetIndex(uint32_t x, uint32_t y, uint8_t paletteIdx, bool mirrorX) {
    if (x >= width || y >= height || palette.empty()) return;
    auto& layer = GetActiveLayer();
    uint8_t clamped = (paletteIdx == kTransparentPixelIndex)
                          ? kTransparentPixelIndex
                          : static_cast<uint8_t>(paletteIdx % palette.size());
    layer.pixelIndices[static_cast<size_t>(y) * width + x] = clamped;
    if (mirrorX) {
        uint32_t mx = (width - 1) - x;
        layer.pixelIndices[static_cast<size_t>(y) * width + mx] = clamped;
    }
}

void PixelArtCanvas::ErasePixel(uint32_t x, uint32_t y, bool mirrorX) {
    SetIndex(x, y, kTransparentPixelIndex, mirrorX);
}

void PixelArtCanvas::FloodFill(uint32_t startX, uint32_t startY, uint8_t newPaletteIdx) {
    if (startX >= width || startY >= height || palette.empty()) return;
    auto& layer = GetActiveLayer();
    uint8_t targetIdx = layer.pixelIndices[static_cast<size_t>(startY) * width + startX];
    uint8_t replacement = static_cast<uint8_t>(newPaletteIdx % palette.size());
    if (targetIdx == replacement) return;

    std::queue<std::pair<uint32_t, uint32_t>> q;
    q.push({startX, startY});
    while (!q.empty()) {
        auto [x, y] = q.front();
        q.pop();
        if (x >= width || y >= height) continue;
        size_t pos = static_cast<size_t>(y) * width + x;
        if (layer.pixelIndices[pos] != targetIdx) continue;
        layer.pixelIndices[pos] = replacement;
        if (x > 0) q.push({x - 1, y});
        if (x + 1 < width) q.push({x + 1, y});
        if (y > 0) q.push({x, y - 1});
        if (y + 1 < height) q.push({x, y + 1});
    }
}

void PixelArtCanvas::StampTilePattern(uint8_t primaryIdx, uint8_t secondaryIdx) {
    auto& layer = GetActiveLayer();
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            bool edge = (x % 4 == 0) || (y % 4 == 0);
            layer.pixelIndices[static_cast<size_t>(y) * width + x] =
                static_cast<uint8_t>((edge ? primaryIdx : secondaryIdx) % std::max<size_t>(1, palette.size()));
        }
    }
}

ImageData2D PixelArtCanvas::BakeToImageData2D() const {
    ImageData2D img;
    img.width = width;
    img.height = height;
    img.pixelExactNearest = true;
    img.rgbaPixels.resize(static_cast<size_t>(width) * height * 4, 0);

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            RgbaColor8 outColor = palette.empty() ? RgbaColor8{0, 0, 0, 255} : palette[0];
            size_t idx = static_cast<size_t>(y) * width + x;
            for (const auto& layer : layers) {
                if (!layer.visible || idx >= layer.pixelIndices.size()) continue;
                uint8_t palIdx = layer.pixelIndices[idx];
                if (palIdx == kTransparentPixelIndex || palIdx >= palette.size()) continue;
                const auto& src = palette[palIdx];
                float a = std::clamp(layer.opacity, 0.0f, 1.0f);
                if (a >= 0.99f) {
                    outColor = src;
                } else {
                    outColor.r = static_cast<uint8_t>(outColor.r * (1.0f - a) + src.r * a);
                    outColor.g = static_cast<uint8_t>(outColor.g * (1.0f - a) + src.g * a);
                    outColor.b = static_cast<uint8_t>(outColor.b * (1.0f - a) + src.b * a);
                    outColor.a = 255;
                }
            }
            img.SetPixel(x, y, outColor);
        }
    }
    return img;
}

nlohmann::json PixelArtCanvas::ToJson() const {
    nlohmann::json palJson = nlohmann::json::array();
    for (const auto& c : palette) {
        palJson.push_back({c.r, c.g, c.b, c.a});
    }
    nlohmann::json layersJson = nlohmann::json::array();
    for (const auto& l : layers) {
        layersJson.push_back({
            {"layerName", l.layerName},
            {"visible", l.visible},
            {"opacity", l.opacity},
            {"pixelIndices", l.pixelIndices}
        });
    }
    return {
        {"spriteName", spriteName},
        {"width", width},
        {"height", height},
        {"activeLayerIndex", activeLayerIndex},
        {"palette", std::move(palJson)},
        {"layers", std::move(layersJson)}
    };
}

std::string PixelArtCanvas::ToJsonString() const {
    return ToJson().dump();
}

PixelArtCanvas PixelArtCanvas::FromJson(const nlohmann::json& j) {
    uint32_t w = j.value("width", 16u);
    uint32_t h = j.value("height", 16u);
    PixelArtCanvas c(w, h);
    c.spriteName = j.value("spriteName", std::string("PZ_Hero_Crystal_Sprite"));
    if (j.contains("palette") && j["palette"].is_array() && !j["palette"].empty()) {
        c.palette.clear();
        for (const auto& cj : j["palette"]) {
            if (cj.is_array() && cj.size() >= 4) {
                c.palette.push_back({
                    cj[0].get<uint8_t>(),
                    cj[1].get<uint8_t>(),
                    cj[2].get<uint8_t>(),
                    cj[3].get<uint8_t>()
                });
            }
        }
    }
    if (j.contains("layers") && j["layers"].is_array() && !j["layers"].empty()) {
        c.layers.clear();
        for (const auto& lj : j["layers"]) {
            PixelArtLayer layer;
            layer.layerName = lj.value("layerName", std::string("Layer"));
            layer.visible = lj.value("visible", true);
            layer.opacity = lj.value("opacity", 1.0f);
            if (lj.contains("pixelIndices") && lj["pixelIndices"].is_array()) {
                auto vec = lj["pixelIndices"].get<std::vector<uint8_t>>();
                if (vec.size() == static_cast<size_t>(w) * h) {
                    layer.pixelIndices = std::move(vec);
                } else {
                    layer.pixelIndices.assign(static_cast<size_t>(w) * h, kTransparentPixelIndex);
                }
            }
            c.layers.push_back(std::move(layer));
        }
        c.activeLayerIndex = std::min(j.value("activeLayerIndex", 0u), static_cast<uint32_t>(c.layers.size() - 1));
    } else if (j.contains("pixelIndices") && j["pixelIndices"].is_array()) {
        // Backwards-compatible migration from single-layer format
        auto vec = j["pixelIndices"].get<std::vector<uint8_t>>();
        if (vec.size() == static_cast<size_t>(w) * h) {
            c.layers.clear();
            c.layers.push_back({"MigratedLayer", true, 1.0f, std::move(vec)});
            c.activeLayerIndex = 0;
        }
    }
    return c;
}

PixelArtCanvas PixelArtCanvas::FromJsonString(const std::string& jsonStr) {
    if (jsonStr.empty()) return PixelArtCanvas(16, 16);
    auto j = nlohmann::json::parse(jsonStr, nullptr, false);
    if (j.is_discarded()) return PixelArtCanvas(16, 16);
    return FromJson(j);
}

// ============================================================================
// Expandable DAG MaterialGraphDocument Implementation
// ============================================================================

MaterialGraphDocument::MaterialGraphDocument() {
    BuildDefaultProceduralGraph();
}

bool MaterialGraphDocument::IsSupportedNodeType(const std::string& nodeType) {
    return nodeType == "AlbedoColor" ||
           nodeType == "CheckerPattern" ||
           nodeType == "WaveBands" ||
           nodeType == "HeightGradient" ||
           nodeType == "NoisePattern" ||
           nodeType == "BlendMix" ||
           nodeType == "FresnelRim" ||
           nodeType == "PbrOutput";
}

void MaterialGraphDocument::BuildDefaultProceduralGraph() {
    nodes_.clear();

    MaterialGraphNode baseNode;
    baseNode.nodeId = "node_albedo";
    baseNode.nodeType = "AlbedoColor";
    baseNode.colorParam = {0.14f, 0.78f, 0.94f};
    baseNode.scalarA = 1.0f;
    baseNode.scalarB = 0.0f;
    nodes_.push_back(baseNode);

    MaterialGraphNode patternNode;
    patternNode.nodeId = "node_checker";
    patternNode.nodeType = "CheckerPattern";
    patternNode.colorParam = {0.95f, 0.76f, 0.25f};
    patternNode.scalarA = 8.0f;  // UV frequency
    patternNode.scalarB = 0.28f; // Pattern mix weight
    patternNode.inputNodeA = "node_albedo";
    nodes_.push_back(patternNode);

    MaterialGraphNode rimNode;
    rimNode.nodeId = "node_fresnel";
    rimNode.nodeType = "FresnelRim";
    rimNode.colorParam = {0.00f, 0.92f, 1.00f};
    rimNode.scalarA = 2.4f;  // Fresnel exponent
    rimNode.scalarB = 0.35f; // Rim intensity
    rimNode.inputNodeA = "node_checker";
    nodes_.push_back(rimNode);

    MaterialGraphNode outNode;
    outNode.nodeId = "node_output";
    outNode.nodeType = "PbrOutput";
    outNode.scalarA = 0.35f; // Metallic
    outNode.scalarB = 0.24f; // Roughness
    outNode.inputNodeA = "node_fresnel";
    nodes_.push_back(outNode);
}

MaterialGraphNode* MaterialGraphDocument::FindNode(const std::string& nodeId) {
    for (auto& n : nodes_) {
        if (n.nodeId == nodeId) return &n;
    }
    return nullptr;
}

const MaterialGraphNode* MaterialGraphDocument::FindNode(const std::string& nodeId) const {
    for (const auto& n : nodes_) {
        if (n.nodeId == nodeId) return &n;
    }
    return nullptr;
}

std::string MaterialGraphDocument::AddNode(
    const std::string& nodeType,
    const glm::vec3& colorParam,
    float scalarA,
    float scalarB,
    const std::string& inputNodeA,
    const std::string& inputNodeB) {
    if (!IsSupportedNodeType(nodeType)) return {};
    uint32_t seq = static_cast<uint32_t>(nodes_.size()) + 1;
    std::string newId;
    do {
        newId = "node_" + std::to_string(seq++);
    } while (FindNode(newId) != nullptr);

    MaterialGraphNode n;
    n.nodeId = newId;
    n.nodeType = nodeType;
    n.colorParam = colorParam;
    n.scalarA = scalarA;
    n.scalarB = scalarB;
    n.inputNodeA = inputNodeA;
    n.inputNodeB = inputNodeB;

    // Insert before PbrOutput if present so visual flow stays readable
    auto outIt = std::find_if(nodes_.begin(), nodes_.end(), [](const MaterialGraphNode& x) {
        return x.nodeType == "PbrOutput";
    });
    if (outIt != nodes_.end() && nodeType != "PbrOutput") {
        nodes_.insert(outIt, std::move(n));
    } else {
        nodes_.push_back(std::move(n));
    }
    return newId;
}

bool MaterialGraphDocument::RemoveNode(const std::string& nodeId, std::string* outError) {
    auto it = std::find_if(nodes_.begin(), nodes_.end(), [&](const MaterialGraphNode& n) {
        return n.nodeId == nodeId;
    });
    if (it == nodes_.end()) {
        if (outError) *outError = "Node not found: " + nodeId;
        return false;
    }
    if (it->nodeType == "PbrOutput") {
        if (outError) *outError = "Cannot delete terminal PbrOutput node";
        return false;
    }
    std::string fallbackInput = it->inputNodeA;
    nodes_.erase(it);
    // Cleanly rewire downstream nodes that pointed to deleted node
    for (auto& n : nodes_) {
        if (n.inputNodeA == nodeId) n.inputNodeA = fallbackInput;
        if (n.inputNodeB == nodeId) n.inputNodeB = "";
    }
    return true;
}

bool MaterialGraphDocument::ConnectNodes(
    const std::string& sourceNodeId,
    const std::string& targetNodeId,
    int targetPortIndex,
    std::string* outError) {
    if (sourceNodeId == targetNodeId) {
        if (outError) *outError = "Self-connection cycle forbidden on node '" + sourceNodeId + "'";
        return false;
    }
    const MaterialGraphNode* src = FindNode(sourceNodeId);
    MaterialGraphNode* dst = FindNode(targetNodeId);
    if (!src || !dst) {
        if (outError) *outError = "Source or target node does not exist";
        return false;
    }
    if (src->nodeType == "PbrOutput") {
        if (outError) *outError = "Terminal PbrOutput cannot feed another node";
        return false;
    }

    std::string prevA = dst->inputNodeA;
    std::string prevB = dst->inputNodeB;
    if (targetPortIndex == 0) dst->inputNodeA = sourceNodeId;
    else dst->inputNodeB = sourceNodeId;

    auto val = ValidateGraph();
    if (!val.valid) {
        // Roll back invalid cyclic connection cleanly
        dst->inputNodeA = prevA;
        dst->inputNodeB = prevB;
        if (outError) *outError = val.diagnosticMessage;
        return false;
    }
    return true;
}

MaterialGraphValidationResult MaterialGraphDocument::ValidateGraph() const {
    MaterialGraphValidationResult res{};
    if (nodes_.empty()) {
        res.diagnosticMessage = "Material graph contains zero nodes";
        return res;
    }

    const MaterialGraphNode* outNode = nullptr;
    for (const auto& n : nodes_) {
        if (!IsSupportedNodeType(n.nodeType)) {
            res.failingNodeId = n.nodeId;
            res.diagnosticMessage = "Unsupported nodeType '" + n.nodeType + "' on node '" + n.nodeId + "' (PLZ-RENDERX-002)";
            return res;
        }
        if (n.nodeType == "PbrOutput") {
            outNode = &n;
        }
        if (!n.inputNodeA.empty() && !FindNode(n.inputNodeA)) {
            res.failingNodeId = n.nodeId;
            res.diagnosticMessage = "Node '" + n.nodeId + "' references missing inputNodeA '" + n.inputNodeA + "' (PLZ-RENDERX-002)";
            return res;
        }
        if (!n.inputNodeB.empty() && !FindNode(n.inputNodeB)) {
            res.failingNodeId = n.nodeId;
            res.diagnosticMessage = "Node '" + n.nodeId + "' references missing inputNodeB '" + n.inputNodeB + "' (PLZ-RENDERX-002)";
            return res;
        }
        if (!std::isfinite(n.scalarA) || !std::isfinite(n.scalarB)) {
            res.failingNodeId = n.nodeId;
            res.diagnosticMessage = "Node '" + n.nodeId + "' contains non-finite scalar parameters";
            return res;
        }
    }

    if (!outNode) {
        res.failingNodeId = nodes_.back().nodeId;
        res.diagnosticMessage = "Material graph is missing required 'PbrOutput' terminal node (PLZ-RENDERX-002)";
        return res;
    }

    // Full DFS cycle detection & topological ordering over BOTH inputNodeA and inputNodeB across all nodes reachable from PbrOutput
    std::map<std::string, int> visitState; // 0 = unvisited, 1 = visiting (on stack), 2 = done
    std::vector<std::string> topoOrder;
    std::string cycleNode;

    std::function<bool(const std::string&)> dfs = [&](const std::string& id) -> bool {
        visitState[id] = 1;
        const MaterialGraphNode* cur = FindNode(id);
        if (!cur) return true;

        const std::string inputs[2] = {cur->inputNodeA, cur->inputNodeB};
        for (const auto& inId : inputs) {
            if (inId.empty()) continue;
            int st = visitState[inId];
            if (st == 1) {
                cycleNode = inId;
                return false;
            }
            if (st == 0) {
                if (!dfs(inId)) return false;
            }
        }
        visitState[id] = 2;
        topoOrder.push_back(id);
        return true;
    };

    if (!dfs(outNode->nodeId)) {
        res.failingNodeId = cycleNode;
        res.diagnosticMessage = "Cycle detected in material node DAG at node '" + cycleNode + "' (PLZ-RENDERX-002)";
        return res;
    }

    // Also detect cycles in any disconnected subgraphs
    for (const auto& n : nodes_) {
        if (visitState[n.nodeId] == 0) {
            if (!dfs(n.nodeId)) {
                res.failingNodeId = cycleNode;
                res.diagnosticMessage = "Cycle detected in material node DAG at node '" + cycleNode + "' (PLZ-RENDERX-002)";
                return res;
            }
        }
    }

    res.valid = true;
    res.topologicalOrder = std::move(topoOrder);
    res.diagnosticMessage = "Valid DAG (" + std::to_string(nodes_.size()) + " nodes, " +
                            std::to_string(res.topologicalOrder.size()) + " evaluated into PbrOutput)";
    return res;
}

bool MaterialGraphDocument::CompileToSlangShaderSource(
    std::string* outSlangSource,
    std::string* outFailingNodeId,
    std::string* outError) const {
    auto val = ValidateGraph();
    if (!val.valid) {
        if (outFailingNodeId) *outFailingNodeId = val.failingNodeId;
        if (outError) *outError = val.diagnosticMessage;
        return false;
    }

    // Emit topological DAG evaluation statements directly inside fsMain
    std::ostringstream dagCode;
    std::string finalColorVar = "gObject.baseColor.rgb";
    std::string finalEmissiveBoostVar = "float3(0.0, 0.0, 0.0)";

    for (const auto& nid : val.topologicalOrder) {
        const MaterialGraphNode* n = FindNode(nid);
        if (!n) continue;
        std::string varName = SanitizeSlangIdent(n->nodeId);
        std::string inAVar = n->inputNodeA.empty() ? "gObject.baseColor.rgb" : SanitizeSlangIdent(n->inputNodeA);
        std::string inBVar = n->inputNodeB.empty() ? "float3(0.0, 0.0, 0.0)" : SanitizeSlangIdent(n->inputNodeB);

        glm::vec3 col = glm::clamp(n->colorParam, glm::vec3(0.0f), glm::vec3(1.0f));

        if (n->nodeType == "AlbedoColor") {
            dagCode << "    float3 " << varName << " = lerp(gObject.baseColor.rgb, float3("
                    << col.r << ", " << col.g << ", " << col.b << "), 0.35);\n";
        } else if (n->nodeType == "CheckerPattern") {
            float freq = std::clamp(n->scalarA, 1.0f, 64.0f);
            float mixW = std::clamp(n->scalarB, 0.0f, 1.0f);
            dagCode << "    float2 chkG_" << varName << " = floor(input.uv * " << freq << ");\n"
                    << "    float chkM_" << varName << " = abs(fmod(chkG_" << varName << ".x + chkG_" << varName << ".y, 2.0));\n"
                    << "    float3 " << varName << " = ((gObject.textureFlags & 1u) != 0u) ? lerp("
                    << inAVar << ", float3(" << col.r << ", " << col.g << ", " << col.b << "), chkM_" << varName << " * " << mixW
                    << ") : " << inAVar << ";\n";
        } else if (n->nodeType == "WaveBands") {
            float freq = std::clamp(n->scalarA, 1.0f, 48.0f);
            float mixW = std::clamp(n->scalarB, 0.0f, 1.0f);
            dagCode << "    float wav_" << varName << " = 0.5 + 0.5 * sin((input.uv.x + input.uv.y) * " << (freq * 3.14159f) << ");\n"
                    << "    float3 " << varName << " = lerp(" << inAVar << ", float3("
                    << col.r << ", " << col.g << ", " << col.b << "), wav_" << varName << " * " << mixW << ");\n";
        } else if (n->nodeType == "HeightGradient") {
            float minY = n->scalarA;
            float maxY = std::max(n->scalarB, minY + 0.05f);
            dagCode << "    float hg_" << varName << " = clamp((input.worldPos.y - " << minY << ") / " << (maxY - minY) << ", 0.0, 1.0);\n"
                    << "    float3 " << varName << " = lerp(" << inAVar << ", float3("
                    << col.r << ", " << col.g << ", " << col.b << "), hg_" << varName << " * 0.65);\n";
        } else if (n->nodeType == "NoisePattern") {
            float sc = std::clamp(n->scalarA, 0.5f, 32.0f);
            float str = std::clamp(n->scalarB, 0.0f, 1.0f);
            dagCode << "    float nz_" << varName << " = 0.5 + 0.5 * sin(input.worldPos.x * " << sc
                    << ") * cos(input.worldPos.y * " << (sc * 1.17f) << ") * sin(input.worldPos.z * " << (sc * 0.83f) << ");\n"
                    << "    float3 " << varName << " = lerp(" << inAVar << ", float3("
                    << col.r << ", " << col.g << ", " << col.b << "), nz_" << varName << " * " << str << ");\n";
        } else if (n->nodeType == "BlendMix") {
            float fac = std::clamp(n->scalarA, 0.0f, 1.0f);
            dagCode << "    float3 " << varName << " = lerp(" << inAVar << ", " << inBVar << ", " << fac << ");\n";
        } else if (n->nodeType == "FresnelRim") {
            float pwr = std::clamp(n->scalarA, 0.5f, 8.0f);
            float str = std::clamp(n->scalarB, 0.0f, 1.0f);
            dagCode << "    float3 graphFresnel_" << varName << " = float3(" << col.r << ", " << col.g << ", " << col.b
                    << ") * pow(clamp(1.0 - max(dot(N, V), 0.0), 0.0, 1.0), " << pwr << ") * " << str << ";\n"
                    << "    float3 " << varName << " = " << inAVar << ";\n";
            finalEmissiveBoostVar = "graphFresnel_" + varName;
        } else if (n->nodeType == "PbrOutput") {
            finalColorVar = inAVar;
        }
    }

    std::ostringstream slang;
    slang << "// Auto-generated by PLATZ DAG Material Node Graph -> Slang 2026.22.1 Compiler\n"
          << "struct SceneUniforms {\n"
          << "    float4x4 viewProj;\n"
          << "    float4   cameraPos;\n"
          << "    float4   sunDirection;\n"
          << "    float4   sunColor;\n"
          << "    float4   pointLightPos;\n"
          << "    float4   pointLightColor;\n"
          << "    float4   skyTopColor;\n"
          << "    float4   skyHorizonColor;\n"
          << "};\n"
          << "struct ObjectPushConstants {\n"
          << "    float4x4 modelMatrix;\n"
          << "    float4   baseColor;\n"
          << "    float4   pbrParams;\n"
          << "    float4   emissiveColor;\n"
          << "    uint     objectIdLow;\n"
          << "    uint     objectIdHigh;\n"
          << "    uint     textureFlags;\n"
          << "    uint     pad0;\n"
          << "};\n"
          << "[[vk::binding(0, 0)]] ConstantBuffer<SceneUniforms> gScene;\n"
          << "[[vk::binding(1, 0)]] Sampler2D gAlbedoTexture;\n"
          << "[[vk::push_constant]] ConstantBuffer<ObjectPushConstants> gObject;\n"
          << "struct VSInput {\n"
          << "    [[vk::location(0)]] float3 position : POSITION;\n"
          << "    [[vk::location(1)]] float3 normal   : NORMAL;\n"
          << "    [[vk::location(2)]] float2 uv       : TEXCOORD0;\n"
          << "    [[vk::location(3)]] float3 color    : COLOR0;\n"
          << "};\n"
          << "struct VSOutput {\n"
          << "    float4 svPosition : SV_Position;\n"
          << "    [[vk::location(0)]] float3 worldPos    : TEXCOORD0;\n"
          << "    [[vk::location(1)]] float3 worldNormal : TEXCOORD1;\n"
          << "    [[vk::location(2)]] float2 uv          : TEXCOORD2;\n"
          << "    [[vk::location(3)]] float3 vertexColor : COLOR0;\n"
          << "};\n"
          << "struct FSOutput {\n"
          << "    [[vk::location(0)]] float4 outColor    : SV_Target0;\n"
          << "    [[vk::location(1)]] uint   outObjectId : SV_Target1;\n"
          << "};\n"
          << "static const float PI = 3.14159265359;\n"
          << "float DistributionGGX(float3 N, float3 H, float roughness) {\n"
          << "    float a = roughness * roughness;\n"
          << "    float a2 = a * a;\n"
          << "    float NdotH = max(dot(N, H), 0.0);\n"
          << "    float denom = (NdotH * NdotH * (a2 - 1.0) + 1.0);\n"
          << "    return a2 / max(PI * denom * denom, 0.00001);\n"
          << "}\n"
          << "float GeometrySchlickGGX(float NdotV, float roughness) {\n"
          << "    float r = (roughness + 1.0);\n"
          << "    float k = (r * r) / 8.0;\n"
          << "    return NdotV / max(NdotV * (1.0 - k) + k, 0.00001);\n"
          << "}\n"
          << "float GeometrySmith(float3 N, float3 V, float3 L, float roughness) {\n"
          << "    return GeometrySchlickGGX(max(dot(N, V), 0.0), roughness) * GeometrySchlickGGX(max(dot(N, L), 0.0), roughness);\n"
          << "}\n"
          << "float3 FresnelSchlick(float cosTheta, float3 F0) {\n"
          << "    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);\n"
          << "}\n"
          << "[shader(\"vertex\")]\n"
          << "VSOutput vsMain(VSInput input) {\n"
          << "    VSOutput output;\n"
          << "    float4 worldPos4 = mul(gObject.modelMatrix, float4(input.position, 1.0));\n"
          << "    output.worldPos = worldPos4.xyz;\n"
          << "    float3x3 normalMat = (float3x3)gObject.modelMatrix;\n"
          << "    output.worldNormal = normalize(mul(normalMat, input.normal));\n"
          << "    output.uv = input.uv;\n"
          << "    output.vertexColor = input.color;\n"
          << "    output.svPosition = mul(gScene.viewProj, worldPos4);\n"
          << "    return output;\n"
          << "}\n"
          << "[shader(\"fragment\")]\n"
          << "FSOutput fsMain(VSOutput input) {\n"
          << "    float3 N = normalize(input.worldNormal);\n"
          << "    float3 V = normalize(gScene.cameraPos.xyz - input.worldPos);\n"
          << dagCode.str()
          << "    float3 albedo = " << finalColorVar << " * input.vertexColor;\n"
          << "    if ((gObject.textureFlags & 2u) != 0u) {\n"
          << "        albedo = gAlbedoTexture.Sample(input.uv).rgb;\n"
          << "    }\n"
          << "    float metallic = clamp(gObject.pbrParams.x, 0.0, 1.0);\n"
          << "    float roughness = clamp(gObject.pbrParams.y, 0.04, 1.0);\n"
          << "    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);\n"
          << "    float3 L_sun = normalize(gScene.sunDirection.xyz);\n"
          << "    float3 H_sun = normalize(V + L_sun);\n"
          << "    float3 radiance_sun = gScene.sunColor.rgb * max(gScene.sunDirection.w, 0.0);\n"
          << "    float3 F_sun = FresnelSchlick(max(dot(H_sun, V), 0.0), F0);\n"
          << "    float3 spec_sun = (DistributionGGX(N, H_sun, roughness) * GeometrySmith(N, V, L_sun, roughness) * F_sun) / (4.0 * max(dot(N, V), 0.0) * max(dot(N, L_sun), 0.0) + 0.0001);\n"
          << "    float3 kD_sun = (float3(1.0, 1.0, 1.0) - F_sun) * (1.0 - metallic);\n"
          << "    float3 Lo = (kD_sun * albedo / PI + spec_sun) * radiance_sun * max(dot(N, L_sun), 0.0);\n"
          << "    float3 toPoint = gScene.pointLightPos.xyz - input.worldPos;\n"
          << "    float dist = length(toPoint);\n"
          << "    float radius = max(gScene.pointLightPos.w, 0.001);\n"
          << "    if (dist < radius && gScene.pointLightColor.a > 0.0) {\n"
          << "        float3 L_pt = toPoint / max(dist, 0.0001);\n"
          << "        float3 H_pt = normalize(V + L_pt);\n"
          << "        float atten = clamp(1.0 - (dist / radius), 0.0, 1.0);\n"
          << "        atten *= atten;\n"
          << "        float3 radiance_pt = gScene.pointLightColor.rgb * gScene.pointLightColor.a * atten;\n"
          << "        float3 F_pt = FresnelSchlick(max(dot(H_pt, V), 0.0), F0);\n"
          << "        float3 spec_pt = (DistributionGGX(N, H_pt, roughness) * GeometrySmith(N, V, L_pt, roughness) * F_pt) / (4.0 * max(dot(N, V), 0.0) * max(dot(N, L_pt), 0.0) + 0.0001);\n"
          << "        float3 kD_pt = (float3(1.0, 1.0, 1.0) - F_pt) * (1.0 - metallic);\n"
          << "        Lo += (kD_pt * albedo / PI + spec_pt) * radiance_pt * max(dot(N, L_pt), 0.0);\n"
          << "    }\n"
          << "    float hemi = clamp(N.y * 0.5 + 0.5, 0.0, 1.0);\n"
          << "    float3 skyAmb = lerp(gScene.skyHorizonColor.rgb * 0.65, gScene.skyTopColor.rgb * 1.35, hemi);\n"
          << "    float3 kS_amb = FresnelSchlick(max(dot(N, V), 0.0), F0);\n"
          << "    float3 kD_amb = (1.0 - kS_amb) * (1.0 - metallic * 0.65);\n"
          << "    float3 R = reflect(-V, N);\n"
          << "    float3 envSky = lerp(float3(0.22, 0.26, 0.34), float3(0.68, 0.78, 0.95), clamp(R.y * 0.5 + 0.5, 0.0, 1.0));\n"
          << "    float3 envSpec = (envSky + gScene.sunColor.rgb * pow(max(dot(R, L_sun), 0.0), lerp(32.0, 3.0, roughness)) * 0.85) * kS_amb * (1.0 - roughness * 0.55) * lerp(0.35, 1.15, metallic);\n"
          << "    float3 ambient = skyAmb * albedo * kD_amb * max(gScene.sunColor.a, 0.25) + envSpec;\n"
          << "    float3 emissive = gObject.emissiveColor.rgb * max(gObject.pbrParams.z, 0.0) + (" << finalEmissiveBoostVar << ") * max(gObject.pbrParams.z, 0.15);\n"
          << "    float3 color = (ambient + Lo + emissive) * max(gScene.cameraPos.w, 0.1);\n"
          << "    color = clamp((color * (2.51 * color + 0.03)) / (color * (2.43 * color + 0.59) + 0.14), 0.0, 1.0);\n"
          << "    if (gObject.emissiveColor.w > 0.5) {\n"
          << "        float rim = pow(clamp(1.0 - max(dot(N, V), 0.0), 0.0, 1.0), 2.2);\n"
          << "        color = lerp(color, float3(0.0, 0.85, 1.0), clamp(rim * 0.75 + 0.15, 0.0, 0.85));\n"
          << "    }\n"
          << "    FSOutput output;\n"
          << "    output.outColor = float4(color, gObject.baseColor.a);\n"
          << "    output.outObjectId = gObject.objectIdLow;\n"
          << "    return output;\n"
          << "}\n";

    if (outSlangSource) *outSlangSource = slang.str();
    return true;
}

ImageData2D MaterialGraphDocument::BakeGraphAlbedoTexture(uint32_t width, uint32_t height) const {
    ImageData2D img;
    img.width = std::max(8u, width);
    img.height = std::max(8u, height);
    img.rgbaPixels.resize(static_cast<size_t>(img.width) * img.height * 4);

    auto val = ValidateGraph();
    for (uint32_t y = 0; y < img.height; ++y) {
        float v = static_cast<float>(y) / static_cast<float>(img.height);
        for (uint32_t x = 0; x < img.width; ++x) {
            float u = static_cast<float>(x) / static_cast<float>(img.width);
            std::map<std::string, glm::vec3> evalValues;
            glm::vec3 outCol(0.5f);

            if (val.valid) {
                for (const auto& nid : val.topologicalOrder) {
                    const MaterialGraphNode* n = FindNode(nid);
                    if (!n) continue;
                    glm::vec3 inA = (!n->inputNodeA.empty() && evalValues.count(n->inputNodeA))
                                        ? evalValues[n->inputNodeA]
                                        : glm::vec3(0.14f, 0.78f, 0.94f);
                    glm::vec3 inB = (!n->inputNodeB.empty() && evalValues.count(n->inputNodeB))
                                        ? evalValues[n->inputNodeB]
                                        : n->colorParam;
                    glm::vec3 res = inA;
                    if (n->nodeType == "AlbedoColor") {
                        res = n->colorParam;
                    } else if (n->nodeType == "CheckerPattern") {
                        float freq = std::clamp(n->scalarA, 1.0f, 32.0f);
                        bool odd = ((static_cast<int>(std::floor(u * freq)) + static_cast<int>(std::floor(v * freq))) & 1) != 0;
                        res = odd ? glm::mix(inA, n->colorParam, std::clamp(n->scalarB, 0.0f, 1.0f)) : inA;
                    } else if (n->nodeType == "WaveBands") {
                        float w = 0.5f + 0.5f * std::sin((u + v) * std::clamp(n->scalarA, 1.0f, 32.0f) * 3.14159f);
                        res = glm::mix(inA, n->colorParam, w * std::clamp(n->scalarB, 0.0f, 1.0f));
                    } else if (n->nodeType == "HeightGradient") {
                        res = glm::mix(inA, n->colorParam, (1.0f - v) * 0.65f);
                    } else if (n->nodeType == "NoisePattern") {
                        float nz = 0.5f + 0.5f * std::sin(u * n->scalarA * 3.1f) * std::cos(v * n->scalarA * 2.7f);
                        res = glm::mix(inA, n->colorParam, nz * std::clamp(n->scalarB, 0.0f, 1.0f));
                    } else if (n->nodeType == "BlendMix") {
                        res = glm::mix(inA, inB, std::clamp(n->scalarA, 0.0f, 1.0f));
                    } else if (n->nodeType == "FresnelRim") {
                        float edge = std::clamp(std::hypot(u - 0.5f, v - 0.5f) * 2.0f, 0.0f, 1.0f);
                        res = inA + n->colorParam * std::pow(edge, std::clamp(n->scalarA, 0.5f, 6.0f)) * std::clamp(n->scalarB, 0.0f, 1.0f);
                    } else if (n->nodeType == "PbrOutput") {
                        outCol = inA;
                    }
                    evalValues[nid] = res;
                }
            }

            outCol = glm::clamp(outCol, glm::vec3(0.0f), glm::vec3(1.0f));
            img.SetPixel(x, y, {
                static_cast<uint8_t>(outCol.r * 255.0f),
                static_cast<uint8_t>(outCol.g * 255.0f),
                static_cast<uint8_t>(outCol.b * 255.0f),
                255
            });
        }
    }
    return img;
}

std::string MaterialGraphDocument::ToJsonString() const {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& n : nodes_) {
        arr.push_back({
            {"nodeId", n.nodeId},
            {"nodeType", n.nodeType},
            {"colorParam", {n.colorParam.r, n.colorParam.g, n.colorParam.b}},
            {"scalarA", n.scalarA},
            {"scalarB", n.scalarB},
            {"inputNodeA", n.inputNodeA},
            {"inputNodeB", n.inputNodeB}
        });
    }
    return nlohmann::json({{"nodes", arr}}).dump();
}

MaterialGraphDocument MaterialGraphDocument::FromJsonString(const std::string& jsonStr) {
    MaterialGraphDocument doc;
    if (jsonStr.empty()) return doc;
    auto j = nlohmann::json::parse(jsonStr, nullptr, false);
    if (j.is_discarded() || !j.contains("nodes") || !j["nodes"].is_array() || j["nodes"].empty()) {
        return doc;
    }
    doc.nodes_.clear();
    for (const auto& nj : j["nodes"]) {
        MaterialGraphNode n;
        n.nodeId = nj.value("nodeId", std::string("node_1"));
        n.nodeType = nj.value("nodeType", std::string("PbrOutput"));
        if (nj.contains("colorParam") && nj["colorParam"].is_array() && nj["colorParam"].size() >= 3) {
            n.colorParam = {
                nj["colorParam"][0].get<float>(),
                nj["colorParam"][1].get<float>(),
                nj["colorParam"][2].get<float>()
            };
        }
        n.scalarA = nj.value("scalarA", 1.0f);
        n.scalarB = nj.value("scalarB", 0.5f);
        n.inputNodeA = nj.value("inputNodeA", std::string(""));
        n.inputNodeB = nj.value("inputNodeB", std::string(""));
        doc.nodes_.push_back(std::move(n));
    }
    return doc;
}

// ============================================================================
// TextureGenerator Implementation
// ============================================================================

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
                col = {40, 210, 245, 255};
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
