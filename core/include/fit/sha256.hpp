// SHA-256 (FIPS 180-4), used to check downloads (FitSuite updates, Raw Accel) before they are used.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace fit {

class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t n);
    std::string hex();  // finishes the hash; lower-case hex, 64 characters

private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buf_[64];
    size_t len_ = 0;
    uint64_t total_ = 0;
};

std::string sha256_hex(const std::string& data);
// Hash of a file's contents; nullopt if it cannot be read.
std::optional<std::string> sha256_file(const std::filesystem::path& p);

}  // namespace fit
