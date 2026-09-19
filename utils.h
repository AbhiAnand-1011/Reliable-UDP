#ifndef SIUU_RUDP_UTILS_H
#define SIUU_RUDP_UTILS_H

#include <string>
#include <vector>
#include <cstdint>

std::vector<std::vector<uint8_t>> readFileChunks(const std::string &path, size_t chunk_size);
bool writeChunksToFile(const std::string &outpath, const std::vector<std::vector<uint8_t>> &chunks);
bool writeChunkToTemp(const std::string &outdir, uint32_t seq, const std::vector<uint8_t> &chunk);
bool assembleChunksFromDir(const std::string &outdir, const std::string &final_path, uint32_t total_chunks);
uint64_t currentTimeMs();
void sleepMs(uint64_t ms);
bool ensureDir(const std::string &path);
std::string sanitizeFilename(const std::string &input);

#endif
