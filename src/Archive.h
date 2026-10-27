#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

// Reading zip archives: the central directory
namespace Luwow::Zip {
    struct Entry {
        // As the archive names it, with the separators it uses
        std::string name;
        uint64_t size = 0;
        uint64_t packedSize = 0;
        uint32_t crc = 0;
        uint16_t method = 0;
        uint64_t localOffset = 0;
        bool isDirectory = false;
        // The Unix file mode from archives made on Unix, such as a macOS app's executables and symlinks, else 0
        uint32_t unixMode = 0;

        bool isSymlink() const { return (unixMode & 0170000) == 0120000; }
    };

    // An archive mapped into memory rather than read, only the pages being inflated are loaded
    class Archive {
    public:
        Archive() = default;
        ~Archive();
        Archive(const Archive&) = delete;
        Archive& operator=(const Archive&) = delete;

        bool open(const std::filesystem::path& path, std::string& error);

        const std::vector<Entry>& getEntries() const { return entries; }
        const Entry* find(const std::string& name) const;

        // The entry's contents, checked against its CRC
        bool read(const Entry& entry, std::string& out, std::string& error) const;

    private:
        const uint8_t* data = nullptr;
        size_t length = 0;
        void* file = nullptr;
        void* mapping = nullptr;
        std::vector<Entry> entries;
        std::unordered_map<std::string, size_t> byName;
    };

    // Raw deflate, into out, which already has the expected size
    bool inflate(const uint8_t* source, size_t sourceLength, std::string& out, std::string& error);

    uint32_t crc32(const uint8_t* data, size_t length);

} // namespace Luwow::Zip
