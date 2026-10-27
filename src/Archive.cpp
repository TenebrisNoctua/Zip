#include "Archive.h"

#include <array>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Luwow::Zip {
    static uint16_t read16(const uint8_t* p) {
        return static_cast<uint16_t>(p[0] | (p[1] << 8));
    }

    static uint32_t read32(const uint8_t* p) {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
            (static_cast<uint32_t>(p[3]) << 24);
    }

    static uint64_t read64(const uint8_t* p) {
        return static_cast<uint64_t>(read32(p)) | (static_cast<uint64_t>(read32(p + 4)) << 32);
    }

    // Archive

    Archive::~Archive() {
#if defined(_WIN32)
        if (data) UnmapViewOfFile(data);
        if (mapping) CloseHandle(mapping);
        if (file) CloseHandle(file);
#else
        if (data) munmap(const_cast<uint8_t*>(data), length);
#endif
    }

    bool Archive::open(const std::filesystem::path& path, std::string& error) {
#if defined(_WIN32)
        HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            error = "could not open the archive";
            return false;
        }
        file = handle;
        LARGE_INTEGER size{};
        GetFileSizeEx(handle, &size);
        length = static_cast<size_t>(size.QuadPart);
        if (length > 0) {
            mapping = CreateFileMappingW(handle, nullptr, PAGE_READONLY, 0, 0, nullptr);
            if (mapping) data = static_cast<const uint8_t*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
            if (!data) {
                error = "could not map the archive";
                return false;
            }
        }
#else
        int descriptor = ::open(path.c_str(), O_RDONLY);
        if (descriptor < 0) {
            error = "could not open the archive";
            return false;
        }
        struct stat info {};
        fstat(descriptor, &info);
        length = static_cast<size_t>(info.st_size);
        if (length > 0) {
            void* mapped = mmap(nullptr, length, PROT_READ, MAP_PRIVATE, descriptor, 0);
            if (mapped != MAP_FAILED) data = static_cast<const uint8_t*>(mapped);
        }
        ::close(descriptor);
        if (length > 0 && !data) {
            error = "could not map the archive";
            return false;
        }
#endif

        // The end of central directory record, before a comment of up to 64 KB
        if (length < 22) {
            error = "not a zip archive";
            return false;
        }
        size_t end = SIZE_MAX;
        size_t lowest = length > 22 + 65535 ? length - 22 - 65535 : 0;
        for (size_t i = length - 22 + 1; i-- > lowest;) {
            if (read32(data + i) == 0x06054b50) {
                end = i;
                break;
            }
        }
        if (end == SIZE_MAX) {
            error = "not a zip archive";
            return false;
        }

        uint64_t count = read16(data + end + 10);
        uint64_t directorySize = read32(data + end + 12);
        uint64_t directoryOffset = read32(data + end + 16);

        // Zip64 keeps the real values in its own record, found through a locator just before
        if ((count == 0xFFFF || directoryOffset == 0xFFFFFFFF) && end >= 20 && read32(data + end - 20) == 0x07064b50) {
            uint64_t record = read64(data + end - 20 + 8);
            if (record + 56 > length || read32(data + record) != 0x06064b50) {
                error = "the archive's zip64 record is damaged";
                return false;
            }
            count = read64(data + record + 32);
            directorySize = read64(data + record + 40);
            directoryOffset = read64(data + record + 48);
        }
        if (directoryOffset + directorySize > length) {
            error = "the archive's directory is damaged";
            return false;
        }

        entries.reserve(static_cast<size_t>(count));
        size_t at = static_cast<size_t>(directoryOffset);
        for (uint64_t i = 0; i < count; ++i) {
            if (at + 46 > length || read32(data + at) != 0x02014b50) {
                error = "the archive's directory is damaged";
                return false;
            }
            const uint8_t* header = data + at;
            Entry entry;
            entry.method = read16(header + 10);
            // The high byte of "made by" is the host system, 3 for Unix, whose mode is in the attributes' top half
            if ((read16(header + 4) >> 8) == 3) entry.unixMode = read32(header + 38) >> 16;
            entry.crc = read32(header + 16);
            entry.packedSize = read32(header + 20);
            entry.size = read32(header + 24);
            uint16_t nameLength = read16(header + 28);
            uint16_t extraLength = read16(header + 30);
            uint16_t commentLength = read16(header + 32);
            entry.localOffset = read32(header + 42);
            if (at + 46 + nameLength + extraLength + commentLength > length) {
                error = "the archive's directory is damaged";
                return false;
            }
            entry.name.assign(reinterpret_cast<const char*>(header + 46), nameLength);

            // Sizes too big for the header are in the zip64 extra field, in this order
            const uint8_t* extra = header + 46 + nameLength;
            for (size_t e = 0; e + 4 <= extraLength;) {
                uint16_t id = read16(extra + e);
                uint16_t size = read16(extra + e + 2);
                if (id == 0x0001) {
                    size_t field = e + 4;
                    if (entry.size == 0xFFFFFFFF && field + 8 <= e + 4 + size) {
                        entry.size = read64(extra + field);
                        field += 8;
                    }
                    if (entry.packedSize == 0xFFFFFFFF && field + 8 <= e + 4 + size) {
                        entry.packedSize = read64(extra + field);
                        field += 8;
                    }
                    if (entry.localOffset == 0xFFFFFFFF && field + 8 <= e + 4 + size) entry.localOffset = read64(extra + field);
                }
                e += 4 + size;
            }

            entry.isDirectory = !entry.name.empty() && (entry.name.back() == '/' || entry.name.back() == '\\');
            byName.emplace(entry.name, entries.size());
            entries.push_back(std::move(entry));
            at += 46 + nameLength + extraLength + commentLength;
        }
        return true;
    }

    const Entry* Archive::find(const std::string& name) const {
        auto it = byName.find(name);
        return it == byName.end() ? nullptr : &entries[it->second];
    }

    bool Archive::read(const Entry& entry, std::string& out, std::string& error) const {
        if (entry.localOffset + 30 > length || read32(data + entry.localOffset) != 0x04034b50) {
            error = "the entry's header is damaged";
            return false;
        }
        const uint8_t* header = data + entry.localOffset;
        uint64_t start = entry.localOffset + 30 + read16(header + 26) + read16(header + 28);
        if (start + entry.packedSize > length) {
            error = "the entry is cut off";
            return false;
        }
        if (read16(header + 6) & 1) {
            error = "encrypted entries aren't supported";
            return false;
        }

        out.assign(static_cast<size_t>(entry.size), '\0');
        if (entry.method == 0) {
            if (entry.packedSize != entry.size) {
                error = "the stored entry's sizes differ";
                return false;
            }
            if (entry.size > 0) memcpy(out.data(), data + start, static_cast<size_t>(entry.size));
        } else if (entry.method == 8) {
            if (!inflate(data + start, static_cast<size_t>(entry.packedSize), out, error)) return false;
        } else {
            error = "compression method " + std::to_string(entry.method) + " isn't supported";
            return false;
        }

        if (crc32(reinterpret_cast<const uint8_t*>(out.data()), out.size()) != entry.crc) {
            error = "the entry's checksum doesn't match";
            return false;
        }
        return true;
    }

    // Inflate (RFC 1951)

    namespace {
        // Bits are read from the low end of each byte, as deflate packs them
        struct BitReader {
            const uint8_t* at;
            const uint8_t* end;
            uint64_t buffer = 0;
            int count = 0;
            // Zero bytes made up past the end, too many of them means the data is cut off
            size_t overrun = 0;

            void refill() {
                while (count <= 56) {
                    uint64_t byte = 0;
                    if (at < end) byte = *at++;
                    else ++overrun;
                    buffer |= byte << count;
                    count += 8;
                }
            }

            uint32_t bits(int n) {
                if (n == 0) return 0;
                if (count < n) refill();
                uint32_t value = static_cast<uint32_t>(buffer & ((1ull << n) - 1));
                buffer >>= n;
                count -= n;
                return value;
            }

            void alignToByte() {
                int drop = count % 8;
                buffer >>= drop;
                count -= drop;
            }
        };

        constexpr int FastBits = 10;

        // Canonical Huffman codes, decoded through a table for the short ones and by counting for the rest
        struct Huffman {
            // symbol | length << 9, 0 for codes longer than FastBits
            std::array<uint16_t, 1 << FastBits> fast{};
            std::array<uint16_t, 16> counts{};
            std::array<uint16_t, 288> symbols{};

            bool build(const uint8_t* lengths, int n) {
                fast.fill(0);
                counts.fill(0);
                for (int i = 0; i < n; ++i) counts[lengths[i]]++;
                counts[0] = 0;

                // Over-subscribed codes are invalid, incomplete ones are allowed
                int left = 1;
                for (int length = 1; length < 16; ++length) {
                    left <<= 1;
                    left -= counts[length];
                    if (left < 0) return false;
                }

                std::array<uint16_t, 16> offsets{};
                for (int length = 1; length < 15; ++length) offsets[length + 1] = offsets[length] + counts[length];
                for (int i = 0; i < n; ++i) {
                    if (lengths[i]) symbols[offsets[lengths[i]]++] = static_cast<uint16_t>(i);
                }

                // The codes in order, reversed into the order their bits arrive in
                uint32_t code = 0;
                int index = 0;
                for (int length = 1; length <= FastBits; ++length) {
                    for (int k = 0; k < counts[length]; ++k, ++index, ++code) {
                        uint32_t reversed = 0;
                        for (int b = 0; b < length; ++b) reversed |= ((code >> b) & 1) << (length - 1 - b);
                        uint16_t value = static_cast<uint16_t>(symbols[index] | (length << 9));
                        for (uint32_t fill = reversed; fill < (1u << FastBits); fill += 1u << length) fast[fill] = value;
                    }
                    code <<= 1;
                }
                return true;
            }

            int decode(BitReader& reader) const {
                if (reader.count < 15) reader.refill();
                uint16_t value = fast[reader.buffer & ((1u << FastBits) - 1)];
                if (value) {
                    int length = value >> 9;
                    reader.buffer >>= length;
                    reader.count -= length;
                    return value & 0x1FF;
                }

                int code = 0, first = 0, index = 0;
                for (int length = 1; length < 16; ++length) {
                    code |= static_cast<int>((reader.buffer >> (length - 1)) & 1);
                    int count = counts[length];
                    if (code - count < first) {
                        reader.buffer >>= length;
                        reader.count -= length;
                        return symbols[index + (code - first)];
                    }
                    index += count;
                    first += count;
                    first <<= 1;
                    code <<= 1;
                }
                return -1;
            }
        };

        constexpr uint16_t LengthBase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99,
            115, 131, 163, 195, 227, 258 };
        constexpr uint8_t LengthExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
        constexpr uint16_t DistanceBase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
            1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
        constexpr uint8_t DistanceExtra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12,
            12, 13, 13 };
    } // namespace

    bool inflate(const uint8_t* source, size_t sourceLength, std::string& out, std::string& error) {
        BitReader reader{ source, source + sourceLength };
        uint8_t* output = reinterpret_cast<uint8_t*>(out.data());
        size_t capacity = out.size();
        size_t written = 0;
        Huffman literals, distances;

        auto damaged = [&](const char* why) {
            error = std::string("the compressed data is damaged: ") + why;
            return false;
        };

        bool last = false;
        while (!last) {
            last = reader.bits(1) != 0;
            uint32_t type = reader.bits(2);

            if (type == 0) {
                reader.alignToByte();
                uint32_t blockLength = reader.bits(16);
                uint32_t check = reader.bits(16);
                if ((blockLength ^ 0xFFFF) != check) return damaged("a stored block's length");
                if (written + blockLength > capacity) return damaged("it's longer than the entry");
                for (uint32_t i = 0; i < blockLength; ++i) output[written++] = static_cast<uint8_t>(reader.bits(8));
                if (reader.overrun > 8) return damaged("it's cut off");
                continue;
            }

            if (type == 1) {
                uint8_t lengths[288 + 30];
                for (int i = 0; i < 144; ++i) lengths[i] = 8;
                for (int i = 144; i < 256; ++i) lengths[i] = 9;
                for (int i = 256; i < 280; ++i) lengths[i] = 7;
                for (int i = 280; i < 288; ++i) lengths[i] = 8;
                for (int i = 0; i < 30; ++i) lengths[288 + i] = 5;
                literals.build(lengths, 288);
                distances.build(lengths + 288, 30);
            } else if (type == 2) {
                int literalCount = static_cast<int>(reader.bits(5)) + 257;
                int distanceCount = static_cast<int>(reader.bits(5)) + 1;
                int codeCount = static_cast<int>(reader.bits(4)) + 4;
                if (literalCount > 286 || distanceCount > 30) return damaged("a block's code counts");

                static constexpr uint8_t Order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
                uint8_t codeLengths[19] = {};
                for (int i = 0; i < codeCount; ++i) codeLengths[Order[i]] = static_cast<uint8_t>(reader.bits(3));
                Huffman lengthCode;
                if (!lengthCode.build(codeLengths, 19)) return damaged("a block's code lengths");

                uint8_t lengths[286 + 30] = {};
                int total = literalCount + distanceCount;
                for (int i = 0; i < total;) {
                    int symbol = lengthCode.decode(reader);
                    if (symbol < 0) return damaged("a code length");
                    if (symbol < 16) {
                        lengths[i++] = static_cast<uint8_t>(symbol);
                        continue;
                    }
                    int repeat = 0;
                    uint8_t value = 0;
                    if (symbol == 16) {
                        if (i == 0) return damaged("a repeat with nothing before it");
                        value = lengths[i - 1];
                        repeat = 3 + static_cast<int>(reader.bits(2));
                    } else if (symbol == 17) {
                        repeat = 3 + static_cast<int>(reader.bits(3));
                    } else {
                        repeat = 11 + static_cast<int>(reader.bits(7));
                    }
                    if (i + repeat > total) return damaged("too many code lengths");
                    while (repeat--) lengths[i++] = value;
                }
                if (lengths[256] == 0) return damaged("a block without an end");
                if (!literals.build(lengths, literalCount) || !distances.build(lengths + literalCount, distanceCount)) {
                    return damaged("a block's codes");
                }
            } else {
                return damaged("an unknown block type");
            }

            while (true) {
                int symbol = literals.decode(reader);
                if (symbol < 0) return damaged("a literal");
                if (symbol < 256) {
                    if (written >= capacity) return damaged("it's longer than the entry");
                    output[written++] = static_cast<uint8_t>(symbol);
                    continue;
                }
                if (symbol == 256) break;

                symbol -= 257;
                if (symbol >= 29) return damaged("a length");
                size_t copyLength = LengthBase[symbol] + reader.bits(LengthExtra[symbol]);
                int distanceSymbol = distances.decode(reader);
                if (distanceSymbol < 0 || distanceSymbol >= 30) return damaged("a distance");
                size_t distance = DistanceBase[distanceSymbol] + reader.bits(DistanceExtra[distanceSymbol]);
                if (distance > written) return damaged("a distance before the start");
                if (written + copyLength > capacity) return damaged("it's longer than the entry");

                uint8_t* to = output + written;
                const uint8_t* from = to - distance;
                if (distance >= copyLength) {
                    memcpy(to, from, copyLength);
                } else {
                    for (size_t i = 0; i < copyLength; ++i) to[i] = from[i];
                }
                written += copyLength;
            }
            if (reader.overrun > 8) return damaged("it's cut off");
        }

        if (written != capacity) return damaged("it's shorter than the entry");
        return true;
    }

    // CRC-32, eight bytes at a time

    namespace {
        struct CrcTables {
            uint32_t table[8][256];
            CrcTables() {
                for (uint32_t i = 0; i < 256; ++i) {
                    uint32_t c = i;
                    for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                    table[0][i] = c;
                }
                for (uint32_t i = 0; i < 256; ++i) {
                    for (int t = 1; t < 8; ++t) table[t][i] = (table[t - 1][i] >> 8) ^ table[0][table[t - 1][i] & 0xFF];
                }
            }
        };
    } // namespace

    uint32_t crc32(const uint8_t* data, size_t length) {
        static const CrcTables tables;
        const auto& t = tables.table;
        uint32_t crc = 0xFFFFFFFFu;
        while (length >= 8) {
            uint32_t low = read32(data) ^ crc;
            uint32_t high = read32(data + 4);
            crc = t[7][low & 0xFF] ^ t[6][(low >> 8) & 0xFF] ^ t[5][(low >> 16) & 0xFF] ^ t[4][low >> 24] ^ t[3][high & 0xFF] ^
                t[2][(high >> 8) & 0xFF] ^ t[1][(high >> 16) & 0xFF] ^ t[0][high >> 24];
            data += 8;
            length -= 8;
        }
        while (length--) crc = t[0][(crc ^ *data++) & 0xFF] ^ (crc >> 8);
        return crc ^ 0xFFFFFFFFu;
    }
} // namespace Luwow::Zip
