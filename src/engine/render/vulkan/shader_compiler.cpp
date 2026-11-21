#include "engine/render/vulkan/shader_compiler.h"
#include "platform/platform_paths.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace platz::engine::vulkan {

namespace {
std::atomic<uint64_t> gShaderCompileSeq{1};
} // namespace

ShaderCompiler::ShaderCompiler() {
#ifdef PLATZ_SLANGC_PATH
    slangcPath_ = std::filesystem::path(PLATZ_SLANGC_PATH);
#else
    slangcPath_ = platform::GetRepoRootDir() / "out" / "tools" / "slang" / "bin" / "slangc.exe";
#endif
}

bool ShaderCompiler::LoadAndValidateSpvFile(
    const std::filesystem::path& spvFilePath,
    const std::string& stageName,
    const std::string& entryPoint,
    CompiledShaderStage* outStage,
    std::string* outError) {
    std::ifstream ifs(spvFilePath, std::ios::binary | std::ios::ate);
    if (!ifs) {
        if (outError) *outError = "Cannot open SPIR-V file: " + spvFilePath.string();
        return false;
    }
    auto sz = ifs.tellg();
    if (sz <= 0 || (sz % 4) != 0) {
        if (outError) *outError = "Invalid SPIR-V file byte size: " + spvFilePath.string();
        return false;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(sz));
    ifs.seekg(0, std::ios::beg);
    ifs.read(reinterpret_cast<char*>(bytes.data()), sz);

    SpirvReflectionSummary refl = SpirvValidator::ValidateAndReflect(bytes);
    if (!refl.valid) {
        if (outError) *outError = "SPIR-V validation failed for " + spvFilePath.string() + ": " + refl.diagnostic;
        return false;
    }

    if (outStage) {
        outStage->valid = true;
        outStage->stageName = stageName;
        outStage->entryPoint = entryPoint;
        outStage->spirvWords.resize(bytes.size() / sizeof(uint32_t));
        std::memcpy(outStage->spirvWords.data(), bytes.data(), bytes.size());
        outStage->reflection = refl;
        outStage->compilerDiagnostic = refl.diagnostic;
        outStage->compileGeneration = gShaderCompileSeq.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

bool ShaderCompiler::CompileSlangFileToSpirv(
    const std::filesystem::path& slangFilePath,
    const std::string& entryPoint,
    const std::string& stageName,
    CompiledShaderStage* outStage,
    std::string* outError) {
    if (!std::filesystem::exists(slangcPath_)) {
        if (outError) *outError = "Slang compiler not found at: " + slangcPath_.string();
        return false;
    }
    if (!std::filesystem::exists(slangFilePath)) {
        if (outError) *outError = "Slang source file does not exist: " + slangFilePath.string();
        return false;
    }

    uint64_t seq = gShaderCompileSeq.fetch_add(1, std::memory_order_relaxed);
    std::filesystem::path tempDir = platform::GetRepoRootDir() / "out" / "shader_cache";
    std::error_code ec;
    std::filesystem::create_directories(tempDir, ec);

    std::filesystem::path outSpv = tempDir / ("shader_" + std::to_string(seq) + ".spv");
    std::filesystem::path outLog = tempDir / ("shader_" + std::to_string(seq) + ".log");

    // Quote command properly for Windows _wsystem / cmd.exe
    std::ostringstream cmd;
    cmd << "\"\"" << slangcPath_.string() << "\" \"" << slangFilePath.string()
        << "\" -target spirv -profile spirv_1_5 -entry " << entryPoint
        << " -stage " << stageName
        << " -O2 -o \"" << outSpv.string() << "\" > \"" << outLog.string() << "\" 2>&1\"";

    int rc = std::system(cmd.str().c_str());

    std::string logText;
    {
        std::ifstream logIfs(outLog);
        if (logIfs) {
            std::ostringstream logOss;
            logOss << logIfs.rdbuf();
            logText = logOss.str();
        }
    }
    std::filesystem::remove(outLog, ec);

    if (rc != 0 || !std::filesystem::exists(outSpv, ec)) {
        std::filesystem::remove(outSpv, ec);
        lastDiagnostic_ = "Slang compile error (" + slangFilePath.filename().string() + "): " +
                          (logText.empty() ? ("exit code " + std::to_string(rc)) : logText);
        if (outError) *outError = lastDiagnostic_;
        return false;
    }

    bool ok = LoadAndValidateSpvFile(outSpv, stageName, entryPoint, outStage, outError);
    std::filesystem::remove(outSpv, ec);
    if (ok && outStage && !logText.empty()) {
        outStage->compilerDiagnostic = logText;
    }
    return ok;
}

void ShaderCompiler::RegisterKnownGoodStages(
    const std::string& pipelineKey,
    const CompiledShaderStage& vertexStage,
    const CompiledShaderStage& fragmentStage) {
    activePipelines_[pipelineKey] = PipelineStagePair{vertexStage, fragmentStage};
}

bool ShaderCompiler::GetActiveStages(
    const std::string& pipelineKey,
    CompiledShaderStage* outVertexStage,
    CompiledShaderStage* outFragmentStage) const {
    auto it = activePipelines_.find(pipelineKey);
    if (it == activePipelines_.end()) return false;
    if (outVertexStage) *outVertexStage = it->second.vertex;
    if (outFragmentStage) *outFragmentStage = it->second.fragment;
    return it->second.vertex.valid && it->second.fragment.valid;
}

bool ShaderCompiler::HotReloadSlangSource(
    const std::string& pipelineKey,
    const std::string& slangSourceCode,
    const std::string& vertexEntry,
    const std::string& fragmentEntry,
    std::string* outDiagnostic) {
    uint64_t seq = gShaderCompileSeq.fetch_add(1, std::memory_order_relaxed);
    std::filesystem::path tempDir = platform::GetRepoRootDir() / "out" / "shader_cache";
    std::error_code ec;
    std::filesystem::create_directories(tempDir, ec);

    std::filesystem::path tempSlang = tempDir / ("hotreload_" + pipelineKey + "_" + std::to_string(seq) + ".slang");
    {
        std::ofstream ofs(tempSlang, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            lastDiagnostic_ = "Failed writing temporary Slang file for hot-reload";
            if (outDiagnostic) *outDiagnostic = lastDiagnostic_;
            ++hotReloadFallbackCount_;
            return false;
        }
        ofs << slangSourceCode;
    }

    CompiledShaderStage newVert, newFrag;
    std::string errV, errF;
    bool vOk = CompileSlangFileToSpirv(tempSlang, vertexEntry, "vertex", &newVert, &errV);
    bool fOk = vOk && CompileSlangFileToSpirv(tempSlang, fragmentEntry, "fragment", &newFrag, &errF);
    std::filesystem::remove(tempSlang, ec);

    if (!vOk || !fOk) {
        // Retain last-known-good stages! (PLZ-MAT-006, PLZ-RENDERX-002, N-13)
        lastDiagnostic_ = !vOk ? errV : errF;
        if (outDiagnostic) *outDiagnostic = lastDiagnostic_;
        ++hotReloadFallbackCount_;
        return false;
    }

    activePipelines_[pipelineKey] = PipelineStagePair{std::move(newVert), std::move(newFrag)};
    lastDiagnostic_ = "Hot-reload succeeded for pipeline '" + pipelineKey + "'";
    if (outDiagnostic) *outDiagnostic = lastDiagnostic_;
    ++hotReloadSuccessCount_;
    return true;
}

} // namespace platz::engine::vulkan
