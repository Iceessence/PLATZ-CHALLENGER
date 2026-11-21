#include "platform/platform_paths.h"

#include <array>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace platz::platform {

namespace {

constexpr std::array<uint32_t, 64> kSha256K = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

constexpr uint32_t RotR(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32u - n));
}

struct Sha256State {
    uint32_t h[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    uint8_t block[64]{};
    size_t blockLen{0};
    uint64_t totalBits{0};

    void Transform(const uint8_t data[64]) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(data[i * 4]) << 24) |
                   (static_cast<uint32_t>(data[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(data[i * 4 + 2]) << 8) |
                   (static_cast<uint32_t>(data[i * 4 + 3]));
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = RotR(w[i - 15], 7) ^ RotR(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = RotR(w[i - 2], 17) ^ RotR(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];

        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = RotR(e, 6) ^ RotR(e, 11) ^ RotR(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t temp1 = hh + S1 + ch + kSha256K[i] + w[i];
            uint32_t S0 = RotR(a, 2) ^ RotR(a, 13) ^ RotR(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temp2 = S0 + maj;

            hh = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    void Update(const uint8_t* bytes, size_t len) {
        for (size_t i = 0; i < len; ++i) {
            block[blockLen++] = bytes[i];
            if (blockLen == 64) {
                Transform(block);
                totalBits += 512;
                blockLen = 0;
            }
        }
    }

    std::string FinalHex() {
        totalBits += static_cast<uint64_t>(blockLen) * 8ULL;
        block[blockLen++] = 0x80u;
        if (blockLen > 56) {
            while (blockLen < 64) block[blockLen++] = 0;
            Transform(block);
            blockLen = 0;
        }
        while (blockLen < 56) block[blockLen++] = 0;
        for (int i = 7; i >= 0; --i) {
            block[blockLen++] = static_cast<uint8_t>((totalBits >> (i * 8)) & 0xFFu);
        }
        Transform(block);

        std::ostringstream oss;
        oss << std::hex << std::setfill('0');
        for (int i = 0; i < 8; ++i) {
            oss << std::setw(8) << h[i];
        }
        return oss.str();
    }
};

std::string NormalizeLowercasePath(const std::filesystem::path& p) {
    std::string s = p.lexically_normal().generic_string();
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    while (s.size() > 1 && s.back() == '/') {
        s.pop_back();
    }
    return s;
}

} // namespace

std::string ComputeSha256Hex(std::span<const uint8_t> data) {
    Sha256State st;
    st.Update(data.data(), data.size());
    return st.FinalHex();
}

std::string ComputeSha256Hex(std::string_view text) {
    Sha256State st;
    st.Update(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    return st.FinalHex();
}

std::string ComputeFileSha256Hex(const std::filesystem::path& filePath, bool* outOk) {
    std::ifstream ifs(filePath, std::ios::binary);
    if (!ifs) {
        if (outOk) *outOk = false;
        return {};
    }
    Sha256State st;
    std::array<uint8_t, 32768> buf{};
    while (ifs) {
        ifs.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        std::streamsize got = ifs.gcount();
        if (got > 0) {
            st.Update(buf.data(), static_cast<size_t>(got));
        }
    }
    if (outOk) *outOk = true;
    return st.FinalHex();
}

bool IsReparsePointOrSymlink(const std::filesystem::path& p) {
    std::error_code ec;
    auto st = std::filesystem::symlink_status(p, ec);
    if (!ec && std::filesystem::is_symlink(st)) {
        return true;
    }
#ifdef _WIN32
    DWORD attrs = GetFileAttributesW(p.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return true;
    }
#endif
    return false;
}

PathSecurityResult ResolveConfinedPath(
    const std::filesystem::path& rootDir,
    const std::filesystem::path& candidatePath) {
    PathSecurityResult res{};
    std::string rawCandidate = candidatePath.string();
    if (rawCandidate.find('\0') != std::string::npos) {
        res.violationReason = "Path contains embedded NUL character";
        return res;
    }
    // Reject UNC or device namespace paths
    if (rawCandidate.starts_with("\\\\") || rawCandidate.starts_with("//") ||
        rawCandidate.find("::") != std::string::npos) {
        res.violationReason = "UNC or device namespace paths are forbidden";
        return res;
    }

    std::error_code ec;
    std::filesystem::path canonicalRoot = std::filesystem::weakly_canonical(rootDir, ec);
    if (ec || canonicalRoot.empty()) {
        res.violationReason = "Failed to canonicalize root directory";
        return res;
    }

    std::filesystem::path combined = candidatePath.is_absolute()
        ? candidatePath
        : (canonicalRoot / candidatePath);

    // Walk every existing prefix component from root downward to check for symlinks/junctions
    std::filesystem::path accum;
    for (const auto& part : combined) {
        accum /= part;
        if (std::filesystem::exists(accum, ec)) {
            if (IsReparsePointOrSymlink(accum)) {
                // Only allow if the reparse point is an ancestor above rootDir itself
                std::string normAccum = NormalizeLowercasePath(accum);
                std::string normRoot = NormalizeLowercasePath(canonicalRoot);
                if (normAccum.size() > normRoot.size() && normAccum.starts_with(normRoot)) {
                    res.violationReason = "Reparse point, junction, or symbolic link detected inside confined workspace: " + accum.string();
                    return res;
                }
            }
        }
    }

    std::filesystem::path resolved = std::filesystem::weakly_canonical(combined, ec);
    if (ec || resolved.empty()) {
        res.violationReason = "Failed to resolve target path canonically";
        return res;
    }

#ifdef _WIN32
    // If target or its parent exists on Windows, resolve short 8.3 aliases via GetFinalPathNameByHandleW
    std::filesystem::path probe = resolved;
    std::vector<std::filesystem::path> trailing;
    while (!probe.empty() && !std::filesystem::exists(probe, ec)) {
        trailing.push_back(probe.filename());
        probe = probe.parent_path();
    }
    if (!probe.empty() && std::filesystem::exists(probe, ec)) {
        HANDLE h = CreateFileW(
            probe.c_str(),
            0,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS,
            nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            wchar_t finalBuf[MAX_PATH * 4]{};
            DWORD len = GetFinalPathNameByHandleW(h, finalBuf, static_cast<DWORD>(std::size(finalBuf)), FILE_NAME_NORMALIZED);
            CloseHandle(h);
            if (len > 0 && len < std::size(finalBuf)) {
                std::wstring ws(finalBuf, len);
                if (ws.starts_with(L"\\\\?\\")) {
                    ws = ws.substr(4);
                }
                std::filesystem::path winResolved(ws);
                for (auto it = trailing.rbegin(); it != trailing.rend(); ++it) {
                    winResolved /= *it;
                }
                resolved = winResolved.lexically_normal();
            }
        }
    }
#endif

    std::string normRoot = NormalizeLowercasePath(canonicalRoot);
    std::string normResolved = NormalizeLowercasePath(resolved);

    bool inside = (normResolved == normRoot) ||
                  (normResolved.size() > normRoot.size() &&
                   normResolved.starts_with(normRoot) &&
                   normResolved[normRoot.size()] == '/');
    if (!inside) {
        res.violationReason = "Path escapes confined project directory (" + normResolved + " is outside " + normRoot + ")";
        return res;
    }

    // Also forbid access to protected C:\PLATZ repository as an extra hard guard
    if (normResolved == "c:/platz" || normResolved.starts_with("c:/platz/")) {
        res.violationReason = "Strict isolation guard: C:/PLATZ is permanently off-limits";
        return res;
    }

    res.allowed = true;
    res.resolvedPath = resolved;
    return res;
}

std::filesystem::path GetExecutableDir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 2]{};
    DWORD len = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (len > 0) {
        return std::filesystem::path(std::wstring(buf, len)).parent_path();
    }
#endif
    return std::filesystem::current_path();
}

std::filesystem::path GetRepoRootDir() {
#ifdef PLATZ_SOURCE_DIR
    return std::filesystem::path(PLATZ_SOURCE_DIR);
#else
    return std::filesystem::current_path();
#endif
}

std::string UtcIso8601Now() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tmUtc{};
#ifdef _WIN32
    gmtime_s(&tmUtc, &t);
#else
    gmtime_r(&t, &tmUtc);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tmUtc, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

} // namespace platz::platform
