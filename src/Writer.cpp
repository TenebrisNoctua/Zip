#include "Writer.h"
#include "Archive.h"
#include "Deflate.h"

namespace Luwow::Zip {
    // Classic records hold 32 bit sizes and offsets and 16 bit counts, these mark a value kept in zip64's
    constexpr uint32_t Zip64Size = 0xFFFFFFFFu;
    constexpr uint16_t Zip64Count = 0xFFFF;

    // Made on Unix (3) or MS-DOS (0), which says whether the external attributes' high half is a Unix mode
#if defined(_WIN32)
    constexpr uint16_t MadeBy = 0 << 8;
#else
    constexpr uint16_t MadeBy = 3 << 8;
#endif
    // Bit 11: names are UTF-8
    constexpr uint16_t Utf8Flag = 0x0800;

    static void put16(std::string& out, uint32_t value) {
        out.push_back(static_cast<char>(value & 0xFF));
        out.push_back(static_cast<char>((value >> 8) & 0xFF));
    }

    static void put32(std::string& out, uint32_t value) {
        put16(out, value & 0xFFFF);
        put16(out, value >> 16);
    }

    static void put64(std::string& out, uint64_t value) {
        put32(out, static_cast<uint32_t>(value & 0xFFFFFFFFu));
        put32(out, static_cast<uint32_t>(value >> 32));
    }

    static uint32_t dosTime(std::time_t time) {
        std::tm local{};
#if defined(_WIN32)
        if (localtime_s(&local, &time) != 0) return (1u << 21) | (1u << 16);
#else
        if (!localtime_r(&time, &local)) return (1u << 21) | (1u << 16);
#endif
        if (local.tm_year < 80) return (1u << 21) | (1u << 16);
        uint32_t date = (static_cast<uint32_t>(local.tm_year - 80) << 9) | (static_cast<uint32_t>(local.tm_mon + 1) << 5) |
            static_cast<uint32_t>(local.tm_mday);
        uint32_t clock = (static_cast<uint32_t>(local.tm_hour) << 11) | (static_cast<uint32_t>(local.tm_min) << 5) |
            static_cast<uint32_t>(local.tm_sec / 2);
        return (date << 16) | clock;
    }

    // MS-DOS attributes in the low half (0x10 a folder), the Unix mode in the high one
    static uint32_t externalAttributes(uint32_t unixMode, bool directory) {
        return (unixMode << 16) | (directory ? 0x10u : 0u);
    }

    bool Writer::open(const std::filesystem::path& path, std::string& error) {
        out.open(path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            error = "could not create the archive";
            return false;
        }
        return true;
    }

    bool Writer::write(const std::string& bytes, std::string& error) {
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            error = "could not write the archive";
            return false;
        }
        offset += bytes.size();
        return true;
    }

    bool Writer::addFile(const std::string& name, const std::string& contents, uint32_t unixMode, std::time_t modified,
        bool compress, std::string& error) {
        Written entry;
        entry.name = name;
        entry.size = contents.size();
        entry.crc = crc32(reinterpret_cast<const uint8_t*>(contents.data()), contents.size());
        entry.dosTime = dosTime(modified);
        entry.externalAttributes = externalAttributes(unixMode, false);

        // Deflated when that's smaller, stored otherwise: already compressed data such as images often isn't
        if (compress && !contents.empty()) {
            std::string packed = deflate(reinterpret_cast<const uint8_t*>(contents.data()), contents.size());
            if (packed.size() < contents.size()) {
                entry.method = 8;
                entry.packedSize = packed.size();
                return add(std::move(entry), packed, error);
            }
        }
        entry.method = 0;
        entry.packedSize = contents.size();
        return add(std::move(entry), contents, error);
    }

    bool Writer::addDirectory(const std::string& name, uint32_t unixMode, std::time_t modified, std::string& error) {
        Written entry;
        entry.name = name;
        entry.dosTime = dosTime(modified);
        entry.externalAttributes = externalAttributes(unixMode, true);
        return add(std::move(entry), std::string(), error);
    }

    // The local header, then the data. Sizes too big for it go in a zip64 extra field.
    bool Writer::add(Written entry, const std::string& data, std::string& error) {
        entry.localOffset = offset;
        bool zip64 = entry.size >= Zip64Size || entry.packedSize >= Zip64Size;

        std::string header;
        put32(header, 0x04034B50);
        put16(header, zip64 ? 45 : 20);
        put16(header, Utf8Flag);
        put16(header, entry.method);
        put32(header, entry.dosTime);
        put32(header, entry.crc);
        put32(header, zip64 ? Zip64Size : static_cast<uint32_t>(entry.packedSize));
        put32(header, zip64 ? Zip64Size : static_cast<uint32_t>(entry.size));
        put16(header, static_cast<uint32_t>(entry.name.size()));
        put16(header, zip64 ? 20 : 0);
        header += entry.name;
        if (zip64) {
            put16(header, 0x0001);
            put16(header, 16);
            put64(header, entry.size);
            put64(header, entry.packedSize);
        }

        if (!write(header, error) || !write(data, error)) return false;
        entries.push_back(std::move(entry));
        return true;
    }

    bool Writer::finish(std::string& error) {
        uint64_t directoryOffset = offset;
        std::string directory;
        for (const Written& entry : entries) {
            // The zip64 extra field holds, in this order, only the values too big for their classic fields
            bool bigSize = entry.size >= Zip64Size;
            bool bigPacked = entry.packedSize >= Zip64Size;
            bool bigOffset = entry.localOffset >= Zip64Size;
            std::string extra;
            if (bigSize) put64(extra, entry.size);
            if (bigPacked) put64(extra, entry.packedSize);
            if (bigOffset) put64(extra, entry.localOffset);
            bool zip64 = !extra.empty();

            put32(directory, 0x02014B50);
            put16(directory, MadeBy | (zip64 ? 45 : 20));
            put16(directory, zip64 ? 45 : 20);
            put16(directory, Utf8Flag);
            put16(directory, entry.method);
            put32(directory, entry.dosTime);
            put32(directory, entry.crc);
            put32(directory, bigPacked ? Zip64Size : static_cast<uint32_t>(entry.packedSize));
            put32(directory, bigSize ? Zip64Size : static_cast<uint32_t>(entry.size));
            put16(directory, static_cast<uint32_t>(entry.name.size()));
            put16(directory, zip64 ? static_cast<uint32_t>(extra.size() + 4) : 0);
            put16(directory, 0); // Comment
            put16(directory, 0); // Disk
            put16(directory, 0); // Internal attributes
            put32(directory, entry.externalAttributes);
            put32(directory, bigOffset ? Zip64Size : static_cast<uint32_t>(entry.localOffset));
            directory += entry.name;
            if (zip64) {
                put16(directory, 0x0001);
                put16(directory, static_cast<uint32_t>(extra.size()));
                directory += extra;
            }
        }
        if (!write(directory, error)) return false;

        uint64_t directorySize = directory.size();
        uint64_t count = entries.size();
        bool zip64 = count >= Zip64Count || directorySize >= Zip64Size || directoryOffset >= Zip64Size;

        std::string end;
        if (zip64) {
            // The zip64 end of central directory record, and the locator pointing at it
            uint64_t recordOffset = offset;
            put32(end, 0x06064B50);
            put64(end, 44);
            put16(end, MadeBy | 45);
            put16(end, 45);
            put32(end, 0); // This disk
            put32(end, 0); // The directory's disk
            put64(end, count);
            put64(end, count);
            put64(end, directorySize);
            put64(end, directoryOffset);

            put32(end, 0x07064B50);
            put32(end, 0);
            put64(end, recordOffset);
            put32(end, 1); // Disks
        }
        put32(end, 0x06054B50);
        put16(end, 0);
        put16(end, 0);
        put16(end, zip64 ? Zip64Count : static_cast<uint32_t>(count));
        put16(end, zip64 ? Zip64Count : static_cast<uint32_t>(count));
        put32(end, zip64 ? Zip64Size : static_cast<uint32_t>(directorySize));
        put32(end, zip64 ? Zip64Size : static_cast<uint32_t>(directoryOffset));
        put16(end, 0); // Comment
        if (!write(end, error)) return false;

        out.close();
        if (!out) {
            error = "could not write the archive";
            return false;
        }
        return true;
    }
} // namespace Luwow::Zip
