#include "engine/render/vulkan/spirv_validator.h"

#include <cstring>

namespace platz::engine::vulkan {

namespace {
constexpr uint32_t kSpirvMagicNumber = 0x07230203u;
constexpr uint16_t kOpMemoryModel = 14u;
constexpr uint16_t kOpEntryPoint = 15u;
} // namespace

SpirvReflectionSummary SpirvValidator::ValidateAndReflect(std::span<const uint8_t> spirvBytes) {
    SpirvReflectionSummary res{};
    if (spirvBytes.empty()) {
        res.diagnostic = "SPIR-V binary is empty";
        return res;
    }
    if (spirvBytes.size() % sizeof(uint32_t) != 0) {
        res.diagnostic = "SPIR-V binary byte size (" + std::to_string(spirvBytes.size()) + ") is not a multiple of 4";
        return res;
    }
    std::span<const uint32_t> words(
        reinterpret_cast<const uint32_t*>(spirvBytes.data()),
        spirvBytes.size() / sizeof(uint32_t));
    return ValidateWords(words);
}

SpirvReflectionSummary SpirvValidator::ValidateWords(std::span<const uint32_t> words) {
    SpirvReflectionSummary res{};
    if (words.size() < 5) {
        res.diagnostic = "SPIR-V module has fewer than 5 header words";
        return res;
    }
    if (words[0] != kSpirvMagicNumber) {
        res.diagnostic = "Invalid SPIR-V magic number (expected 0x07230203)";
        return res;
    }

    uint32_t versionWord = words[1];
    res.versionMajor = (versionWord >> 16) & 0xFFu;
    res.versionMinor = (versionWord >> 8) & 0xFFu;
    if (res.versionMajor != 1 || res.versionMinor > 6) {
        res.diagnostic = "Unsupported SPIR-V version " +
                         std::to_string(res.versionMajor) + "." + std::to_string(res.versionMinor);
        return res;
    }

    res.generatorId = words[2];
    res.idBound = words[3];
    if (res.idBound == 0 || res.idBound > 4194304u) {
        res.diagnostic = "Invalid SPIR-V ID bound in header";
        return res;
    }
    if (words[4] != 0) {
        res.diagnostic = "Reserved SPIR-V header word[4] must be 0";
        return res;
    }

    size_t offset = 5;
    while (offset < words.size()) {
        uint32_t firstWord = words[offset];
        uint16_t wordCount = static_cast<uint16_t>((firstWord >> 16) & 0xFFFFu);
        uint16_t opcode = static_cast<uint16_t>(firstWord & 0xFFFFu);

        if (wordCount == 0) {
            res.diagnostic = "Malformed SPIR-V instruction with wordCount=0 at word offset " + std::to_string(offset);
            return res;
        }
        if (offset + wordCount > words.size()) {
            res.diagnostic = "Truncated SPIR-V instruction at word offset " + std::to_string(offset);
            return res;
        }

        if (opcode == kOpMemoryModel) {
            res.hasMemoryModel = true;
        } else if (opcode == kOpEntryPoint && wordCount >= 4) {
            res.hasEntryPoint = true;
            // Word[offset+1] = ExecutionModel, Word[offset+2] = EntryPoint ID, Word[offset+3..] = null-terminated UTF-8 name
            const char* namePtr = reinterpret_cast<const char*>(&words[offset + 3]);
            size_t maxBytes = (wordCount - 3) * sizeof(uint32_t);
            size_t len = strnlen(namePtr, maxBytes);
            if (len < maxBytes) {
                res.entryPointNames.emplace_back(namePtr, len);
            }
        }

        offset += wordCount;
        ++res.instructionCount;
    }

    if (!res.hasMemoryModel) {
        res.diagnostic = "SPIR-V module is missing required OpMemoryModel instruction";
        return res;
    }
    if (!res.hasEntryPoint) {
        res.diagnostic = "SPIR-V module is missing required OpEntryPoint instruction";
        return res;
    }

    res.valid = true;
    res.diagnostic = "Valid SPIR-V " + std::to_string(res.versionMajor) + "." +
                     std::to_string(res.versionMinor) + " module (" +
                     std::to_string(res.instructionCount) + " instructions)";
    return res;
}

} // namespace platz::engine::vulkan
