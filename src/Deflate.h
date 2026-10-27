#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Compressing zip entries
namespace Luwow::Zip {
    // Raw deflate (RFC 1951): LZ77 matches over the 32 KB window, coded with deflate's fixed Huffman codes
    std::string deflate(const uint8_t* data, size_t length);
} // namespace Luwow::Zip
