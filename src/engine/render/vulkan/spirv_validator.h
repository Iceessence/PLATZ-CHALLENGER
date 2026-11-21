#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace platz::engine::vulkan {

struct SpirvReflectionSummary {
    bool valid{false};
    uint32_t versionMajor{0};
    uint32_t versionMinor{0};
    uint32_t generatorId{0};
    uint32_t idBound{0};
    uint32_t instructionCount{0};
    bool hasMemoryModel{false};
    bool hasEntryPoint{false};
    std::vector<std::string> entryPointNames;
    std::string diagnostic;
};

class SpirvValidator {
public:
    static SpirvReflectionSummary ValidateAndReflect(std::span<const uint8_t> spirvBytes);
    static SpirvReflectionSummary ValidateWords(std::span<const uint32_t> words);
};

} // namespace platz::engine::vulkan
