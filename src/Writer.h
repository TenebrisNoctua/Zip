#pragma once

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// Writing zip archives.
namespace Luwow::Zip {
    class Writer {
    public:
        bool open(const std::filesystem::path& path, std::string& error);

        // A file's contents under name. unixMode is the file's Unix mode, 0 where there's none (Windows); a
        // symlink's contents are where it points. modified is the time shown in the archive.
        bool addFile(const std::string& name, const std::string& contents, uint32_t unixMode, std::time_t modified, bool compress,
            std::string& error);
        // A folder, name ending with "/"
        bool addDirectory(const std::string& name, uint32_t unixMode, std::time_t modified, std::string& error);

        // Writes the central directory and closes the file
        bool finish(std::string& error);

        size_t entryCount() const { return entries.size(); }
        uint64_t size() const { return offset; }

    private:
        struct Written {
            std::string name;
            uint32_t crc = 0;
            uint64_t size = 0;
            uint64_t packedSize = 0;
            uint64_t localOffset = 0;
            uint16_t method = 0;
            uint32_t dosTime = 0;
            uint32_t externalAttributes = 0;
        };

        bool add(Written entry, const std::string& data, std::string& error);
        bool write(const std::string& bytes, std::string& error);

        std::ofstream out;
        uint64_t offset = 0;
        std::vector<Written> entries;
    };
} // namespace Luwow::Zip
