#include "utils.h"
#include "protocol.h"

#include <fstream>
#include <sys/stat.h>
#include <chrono>
#include <thread>
#include <filesystem>
#include <system_error>

std::vector<std::vector<uint8_t>> readFileChunks(const std::string &path, size_t chunk_size) {
    std::vector<std::vector<uint8_t>> chunks;
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return chunks;
    while (true) {
        std::vector<uint8_t> buf;
        buf.resize(chunk_size);
        ifs.read(reinterpret_cast<char *>(buf.data()), static_cast<std::streamsize>(chunk_size));
        std::streamsize r = ifs.gcount();
        if (r <= 0) break;
        buf.resize(static_cast<size_t>(r));
        chunks.push_back(std::move(buf));
    }
    return chunks;
}

bool writeChunksToFile(const std::string &outpath, const std::vector<std::vector<uint8_t>> &chunks) {
    std::filesystem::path p(outpath);
    if (p.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
    }
    std::ofstream ofs(outpath, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    for (const auto &c : chunks) {
        ofs.write(reinterpret_cast<const char *>(c.data()), static_cast<std::streamsize>(c.size()));
        if (!ofs) return false;
    }
    return true;
}

bool writeChunkToTemp(const std::string &outdir, uint32_t seq, const std::vector<uint8_t> &chunk) {
    std::error_code ec;
    std::filesystem::create_directories(outdir, ec);
    std::string file = outdir + "/" + std::to_string(seq) + ".part";
    std::ofstream ofs(file, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    if (!chunk.empty()) {
        ofs.write(reinterpret_cast<const char *>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
    }
    return static_cast<bool>(ofs);
}

bool assembleChunksFromDir(const std::string &outdir, const std::string &final_path, uint32_t total_chunks) {
    std::filesystem::path p(final_path);
    if (p.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
    }
    std::ofstream ofs(final_path, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    for (uint32_t i = 0; i < total_chunks; ++i) {
        std::string part = outdir + "/" + std::to_string(i) + ".part";
        std::ifstream ifs(part, std::ios::binary);
        if (!ifs) return false;
        std::vector<uint8_t> buf((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        if (!buf.empty()) {
            ofs.write(reinterpret_cast<const char *>(buf.data()), static_cast<std::streamsize>(buf.size()));
            if (!ofs) return false;
        }
    }
    return true;
}

uint64_t currentTimeMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

void sleepMs(uint64_t ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

bool ensureDir(const std::string &path) {
    if (path.empty()) return true;
    std::error_code ec;
    return std::filesystem::create_directories(path, ec) || std::filesystem::exists(path, ec);
}

std::string sanitizeFilename(const std::string &input) {
    if (input.empty())
        return {};

    std::filesystem::path p(input);
    std::string name = p.filename().string();
    if (name.empty() || name == "." || name == "..")
        return {};
    if (input != name)
        return {};
    if (input[0] == '/' || input[0] == '\\')
        return {};
    if (input.find("/") != std::string::npos || input.find("\\") != std::string::npos)
        return {};
    return name;
}
