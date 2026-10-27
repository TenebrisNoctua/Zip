#include "Deflate.h"

#include <algorithm>
#include <vector>

namespace Luwow::Zip {
    namespace {
        // Deflate packs bits least significant first, and Huffman codes most significant bit first
        struct BitWriter {
            std::string out;
            uint64_t buffer = 0;
            int count = 0;

            void write(uint32_t bits, int length) {
                buffer |= static_cast<uint64_t>(bits) << count;
                count += length;
                while (count >= 8) {
                    out.push_back(static_cast<char>(buffer & 0xFF));
                    buffer >>= 8;
                    count -= 8;
                }
            }

            void writeCode(uint32_t code, int length) {
                uint32_t reversed = 0;
                for (int i = 0; i < length; ++i) reversed |= ((code >> i) & 1u) << (length - 1 - i);
                write(reversed, length);
            }

            void flush() {
                if (count > 0) out.push_back(static_cast<char>(buffer & 0xFF));
                buffer = 0;
                count = 0;
            }
        };

        // The fixed literal/length code (RFC 1951 3.2.6)
        void writeSymbol(BitWriter& bits, int symbol) {
            if (symbol <= 143) bits.writeCode(0x30 + symbol, 8);
            else if (symbol <= 255) bits.writeCode(0x190 + (symbol - 144), 9);
            else if (symbol <= 279) bits.writeCode(symbol - 256, 7);
            else bits.writeCode(0xC0 + (symbol - 280), 8);
        }

        constexpr uint16_t LengthBase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99,
            115, 131, 163, 195, 227, 258 };
        constexpr uint8_t LengthExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
        constexpr uint16_t DistanceBase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025,
            1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
        constexpr uint8_t DistanceExtra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12,
            12, 13, 13 };

        void writeMatch(BitWriter& bits, size_t length, size_t distance) {
            int code = 28;
            while (LengthBase[code] > length) --code;
            writeSymbol(bits, 257 + code);
            if (LengthExtra[code]) bits.write(static_cast<uint32_t>(length - LengthBase[code]), LengthExtra[code]);

            // Distance codes are fixed 5 bit codes
            int distanceCode = 29;
            while (DistanceBase[distanceCode] > distance) --distanceCode;
            bits.writeCode(static_cast<uint32_t>(distanceCode), 5);
            if (DistanceExtra[distanceCode]) bits.write(static_cast<uint32_t>(distance - DistanceBase[distanceCode]), DistanceExtra[distanceCode]);
        }

        constexpr size_t WindowSize = 32768;
        constexpr size_t MinMatch = 3;
        constexpr size_t MaxMatch = 258;
        constexpr int HashBits = 15;
        // How many earlier places with the same three bytes are tried, trading speed for smaller output
        constexpr int MaxChain = 64;

        uint32_t hash(const uint8_t* p) {
            return ((static_cast<uint32_t>(p[0]) << 10) ^ (static_cast<uint32_t>(p[1]) << 5) ^ p[2]) & ((1u << HashBits) - 1);
        }
    } // namespace

    std::string deflate(const uint8_t* data, size_t length) {
        BitWriter bits;
        bits.write(1, 1); // The last block
        bits.write(1, 2); // With the fixed codes

        // The latest place each hash was seen at, and the one before each place in the window
        std::vector<int64_t> head(size_t(1) << HashBits, -1);
        std::vector<int64_t> previous(WindowSize, -1);
        auto insert = [&](size_t position) {
            if (position + MinMatch > length) return;
            uint32_t h = hash(data + position);
            previous[position % WindowSize] = head[h];
            head[h] = static_cast<int64_t>(position);
        };

        size_t i = 0;
        while (i < length) {
            size_t bestLength = 0;
            size_t bestDistance = 0;
            if (i + MinMatch <= length) {
                size_t maxLength = std::min(MaxMatch, length - i);
                int64_t candidate = head[hash(data + i)];
                for (int chain = 0; candidate >= 0 && chain < MaxChain; ++chain) {
                    size_t distance = i - static_cast<size_t>(candidate);
                    if (distance > WindowSize) break;
                    const uint8_t* earlier = data + candidate;
                    const uint8_t* here = data + i;
                    // A longer match has to agree at the current best's end first
                    if (earlier[bestLength] == here[bestLength]) {
                        size_t matched = 0;
                        while (matched < maxLength && earlier[matched] == here[matched]) ++matched;
                        if (matched > bestLength) {
                            bestLength = matched;
                            bestDistance = distance;
                            if (matched == maxLength) break;
                        }
                    }
                    candidate = previous[static_cast<size_t>(candidate) % WindowSize];
                }
            }

            if (bestLength >= MinMatch) {
                writeMatch(bits, bestLength, bestDistance);
                for (size_t k = 0; k < bestLength; ++k) insert(i + k);
                i += bestLength;
            } else {
                writeSymbol(bits, data[i]);
                insert(i);
                ++i;
            }
        }

        writeSymbol(bits, 256); // The end of the block
        bits.flush();
        return bits.out;
    }
} // namespace Luwow::Zip
