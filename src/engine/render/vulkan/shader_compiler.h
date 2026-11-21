#pragma once

#include "engine/render/vulkan/spirv_validator.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace platz::engine::vulkan {

struct CompiledShaderStage {
    bool valid{false};
    std::string stageName; // "vertex" or "fragment"
    std::string entryPoint;
    std::vector<uint32_t> spirvWords;
    SpirvReflectionSummary reflection;
    std::string compilerDiagnostic;
    uint64_t compileGeneration{0};
};

class ShaderCompiler {
public:
    ShaderCompiler();

    // Load a precompiled .spv file and validate it
    bool LoadAndValidateSpvFile(
        const std::filesystem::path& spvFilePath,
        const std::string& stageName,
        const std::string& entryPoint,
        CompiledShaderStage* outStage,
        std::string* outError = nullptr);

    // Compile Slang source file via slangc.exe (v2026.22.1) -> SPIR-V and validate
    bool CompileSlangFileToSpirv(
        const std::filesystem::path& slangFilePath,
        const std::string& entryPoint,
        const std::string& stageName,
        CompiledShaderStage* outStage,
        std::string* outError = nullptr);

    // Compile in-memory Slang source code with last-known-good fallback (PLZ-MAT-006, PLZ-RENDERX-002, N-13)
    bool HotReloadSlangSource(
        const std::string& pipelineKey,
        const std::string& slangSourceCode,
        const std::string& vertexEntry,
        const std::string& fragmentEntry,
        std::string* outDiagnostic = nullptr);

    void RegisterKnownGoodStages(
        const std::string& pipelineKey,
        const CompiledShaderStage& vertexStage,
        const CompiledShaderStage& fragmentStage);

    bool GetActiveStages(
        const std::string& pipelineKey,
        CompiledShaderStage* outVertexStage,
        CompiledShaderStage* outFragmentStage) const;

    const std::string& GetLastDiagnostic() const { return lastDiagnostic_; }
    uint64_t GetHotReloadSuccessCount() const { return hotReloadSuccessCount_; }
    uint64_t GetHotReloadFallbackCount() const { return hotReloadFallbackCount_; }

private:
    struct PipelineStagePair {
        CompiledShaderStage vertex;
        CompiledShaderStage fragment;
    };

    std::filesystem::path slangcPath_;
    std::map<std::string, PipelineStagePair> activePipelines_;
    std::string lastDiagnostic_;
    uint64_t hotReloadSuccessCount_{0};
    uint64_t hotReloadFallbackCount_{0};
};

} // namespace platz::engine::vulkan
