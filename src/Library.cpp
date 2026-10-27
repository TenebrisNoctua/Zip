#include "Library.h"
#include "Archive.h"
#include "Hash.h"
#include "Writer.h"
#include "Utils.h"
#include "OffThread.h"

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace Luwow::Zip {
    using Fs::checkPath;
    using Fs::errorText;
    using Fs::pathToString;

    // The host, for the work done off the state
    static ILuauHost* zipHost = nullptr;

    ILuauModule* Library::initialize(ILuauHost* host) {
        zipHost = host;
        return new Library();
    }

    // zip.read(path: string) -> { { name, size, packedSize, isDirectory } }, the archive's entries in order
    static int zip_read(lua_State* L) {
        fs::path path = checkPath(L, 1);

        return Luwow::Engine::runOffThread(L, zipHost, [path]() {
            Luwow::Engine::OffThreadResult result;
            auto archive = std::make_shared<Archive>();
            std::string error;
            if (!archive->open(path, error)) {
                result.error = errorText("Could not read archive", path, error);
                return result;
            }
            result.push = [archive](lua_State* L) {
                const auto& entries = archive->getEntries();
                lua_createtable(L, static_cast<int>(entries.size()), 0);
                for (size_t i = 0; i < entries.size(); ++i) {
                    const Entry& entry = entries[i];
                    lua_createtable(L, 0, 6);
                    lua_pushlstring(L, entry.name.data(), entry.name.size());
                    lua_setfield(L, -2, "name");
                    lua_pushnumber(L, static_cast<double>(entry.size));
                    lua_setfield(L, -2, "size");
                    lua_pushnumber(L, static_cast<double>(entry.packedSize));
                    lua_setfield(L, -2, "packedSize");
                    lua_pushboolean(L, entry.isDirectory);
                    lua_setfield(L, -2, "isDirectory");
                    lua_pushboolean(L, entry.isSymlink());
                    lua_setfield(L, -2, "isSymlink");
                    lua_pushinteger(L, static_cast<int>(entry.unixMode & 07777));
                    lua_setfield(L, -2, "mode");
                    lua_rawseti(L, -2, static_cast<int>(i + 1));
                }
                return 1;
            };
            return result;
        });
    }

    // The names in the table at index, or none when it's nil
    static std::vector<std::string> optionalNames(lua_State* L, int index) {
        std::vector<std::string> names;
        if (lua_isnoneornil(L, index)) return names;
        luaL_checktype(L, index, LUA_TTABLE);
        int count = lua_objlen(L, index);
        for (int i = 1; i <= count; ++i) {
            lua_rawgeti(L, index, i);
            size_t length = 0;
            const char* name = lua_tolstring(L, -1, &length);
            if (!name) luaL_error(L, "entry names must be strings");
            names.emplace_back(name, length);
            lua_pop(L, 1);
        }
        return names;
    }

    // zip.hashEntries(path: string, names: { string }?, algorithm: string?) -> { [string]: string }
    // The hex digest of each entry's contents, every file's when names is nil. MD5 by default.
    static int zip_hashEntries(lua_State* L) {
        fs::path path = checkPath(L, 1);
        std::vector<std::string> names = optionalNames(L, 2);
        int algorithm = luaL_checkoption(L, 3, "md5", Fs::Hash::Algorithms);

        return Luwow::Engine::runOffThread(L, zipHost, [path, names, algorithm]() {
            Luwow::Engine::OffThreadResult result;
            Archive archive;
            std::string error;
            if (!archive.open(path, error)) {
                result.error = errorText("Could not read archive", path, error);
                return result;
            }

            std::vector<const Entry*> wanted;
            if (names.empty()) {
                for (const Entry& entry : archive.getEntries()) {
                    if (!entry.isDirectory) wanted.push_back(&entry);
                }
            }
            for (const std::string& name : names) {
                const Entry* entry = archive.find(name);
                if (!entry) {
                    result.error = errorText("Could not read archive", path, "it has no entry " + name);
                    return result;
                }
                wanted.push_back(entry);
            }

            auto digests = std::make_shared<std::vector<std::pair<std::string, std::string>>>();
            std::string contents;
            for (const Entry* entry : wanted) {
                if (!archive.read(*entry, contents, error)) {
                    result.error = errorText("Could not read archive", path, entry->name + ": " + error);
                    return result;
                }
                digests->emplace_back(entry->name, Fs::Hash::digest(contents, algorithm));
            }
            result.push = [digests](lua_State* L) {
                lua_createtable(L, 0, static_cast<int>(digests->size()));
                for (const auto& [name, digest] : *digests) {
                    lua_pushlstring(L, name.data(), name.size());
                    lua_pushlstring(L, digest.data(), digest.size());
                    lua_settable(L, -3);
                }
                return 1;
            };
            return result;
        });
    }

    // Writes an entry's contents to a file, making its folders.
    static bool writeEntry(const fs::path& target, const std::string& contents, const Entry& entry) {
        std::error_code ec;
        if (target.has_parent_path()) fs::create_directories(target.parent_path(), ec);
        // A link already there would be written through rather than replaced
        if (fs::is_symlink(fs::symlink_status(target, ec))) fs::remove(target, ec);

#if !defined(_WIN32)
        if (entry.isSymlink()) {
            fs::remove(target, ec);
            return ::symlink(contents.c_str(), target.c_str()) == 0;
        }
#endif

        std::ofstream file(target, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) return false;
        file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        bool ok = static_cast<bool>(file);
        file.close();

#if !defined(_WIN32)
        if (ok && (entry.unixMode & 07777) != 0) ::chmod(target.c_str(), static_cast<mode_t>(entry.unixMode & 07777));
#endif
        return ok;
    }

    // Where an entry goes in a folder, nothing when its name would leave the folder, such as "../x" or "/x"
    static std::optional<fs::path> entryTarget(const fs::path& destination, const std::string& name) {
        fs::path relative = fs::path(name).lexically_normal();
        if (relative.empty() || relative.is_absolute() || relative.has_root_name() || relative.has_root_directory()) return std::nullopt;
        for (const fs::path& part : relative) {
            if (part == "..") return std::nullopt;
        }
        return destination / relative;
    }

    // zip.extract(path: string, targets: { [string]: string | { string } } | string) -> { written: number, failed: { string } }
    // Writes each named entry to its paths, making their folders and replacing what's there. Given a folder
    // instead, writes every entry into it, as the archive lays them out. Paths that can't be written, such as
    // files in use, are listed in failed; a damaged archive raises.
    static int zip_extract(lua_State* L) {
        fs::path path = checkPath(L, 1);

        std::optional<fs::path> destination;
        std::vector<std::pair<std::string, std::vector<fs::path>>> targets;
        if (lua_type(L, 2) == LUA_TSTRING) {
            destination = checkPath(L, 2);
        } else {
            luaL_checktype(L, 2, LUA_TTABLE);
            lua_pushnil(L);
            while (lua_next(L, 2)) {
                if (lua_type(L, -2) != LUA_TSTRING) luaL_error(L, "targets must be keyed by entry names");
                size_t nameLength = 0;
                const char* name = lua_tolstring(L, -2, &nameLength);
                std::vector<fs::path> paths;
                if (lua_type(L, -1) == LUA_TSTRING) {
                    paths.push_back(checkPath(L, -1));
                } else if (lua_istable(L, -1)) {
                    int count = lua_objlen(L, -1);
                    for (int i = 1; i <= count; ++i) {
                        lua_rawgeti(L, -1, i);
                        if (lua_type(L, -1) != LUA_TSTRING) luaL_error(L, "target paths must be strings");
                        paths.push_back(checkPath(L, -1));
                        lua_pop(L, 1);
                    }
                } else {
                    luaL_error(L, "each target must be a path or a list of paths");
                }
                targets.emplace_back(std::string(name, nameLength), std::move(paths));
                lua_pop(L, 1);
            }
        }

        return Luwow::Engine::runOffThread(L, zipHost, [path, destination, targets]() {
            Luwow::Engine::OffThreadResult result;
            Archive archive;
            std::string error;
            if (!archive.open(path, error)) {
                result.error = errorText("Could not read archive", path, error);
                return result;
            }

            auto failed = std::make_shared<std::vector<std::string>>();
            int written = 0;
            std::string contents;

            auto extract = [&](const Entry& entry, const std::vector<fs::path>& paths) {
                if (!archive.read(entry, contents, error)) {
                    result.error = errorText("Could not read archive", path, entry.name + ": " + error);
                    return false;
                }
                for (const fs::path& target : paths) {
                    if (writeEntry(target, contents, entry)) ++written;
                    else failed->push_back(pathToString(target));
                }
                return true;
            };

            if (destination) {
                // Nothing is written from an archive with an entry that would leave the folder
                for (const Entry& entry : archive.getEntries()) {
                    if (!entryTarget(*destination, entry.name)) {
                        result.error = errorText("Could not read archive", path, "the entry " + entry.name + " would be written outside the folder");
                        return result;
                    }
                }

                // Links last, so a link to a folder doesn't stand where the folder's files go
                std::vector<const Entry*> links;
                for (const Entry& entry : archive.getEntries()) {
                    std::optional<fs::path> target = entryTarget(*destination, entry.name);
                    if (entry.isDirectory) {
                        std::error_code ec;
                        fs::create_directories(*target, ec);
                        continue;
                    }
                    if (entry.isSymlink()) {
                        links.push_back(&entry);
                        continue;
                    }
                    if (!extract(entry, { *target })) return result;
                }
                for (const Entry* entry : links) {
                    if (!extract(*entry, { *entryTarget(*destination, entry->name) })) return result;
                }
            } else {
                for (const auto& [name, paths] : targets) {
                    const Entry* entry = archive.find(name);
                    if (!entry) {
                        result.error = errorText("Could not read archive", path, "it has no entry " + name);
                        return result;
                    }
                    if (!extract(*entry, paths)) return result;
                }
            }

            result.push = [written, failed](lua_State* L) {
                lua_createtable(L, 0, 2);
                lua_pushinteger(L, written);
                lua_setfield(L, -2, "written");
                lua_createtable(L, static_cast<int>(failed->size()), 0);
                for (size_t i = 0; i < failed->size(); ++i) {
                    lua_pushlstring(L, (*failed)[i].data(), (*failed)[i].size());
                    lua_rawseti(L, -2, static_cast<int>(i + 1));
                }
                lua_setfield(L, -2, "failed");
                return 1;
            };
            return result;
        });
    }

    // A file's Unix mode for the archive, none on Windows
    static uint32_t unixModeOf(const fs::path& path) {
#if defined(_WIN32)
        (void)path;
        return 0;
#else
        struct stat info {};
        if (::lstat(path.c_str(), &info) != 0) return 0;
        return static_cast<uint32_t>(info.st_mode) & 0xFFFF;
#endif
    }

    static std::time_t modifiedOf(const fs::path& path) {
        std::error_code ec;
        fs::file_time_type time = fs::last_write_time(path, ec);
        if (ec) return std::time(nullptr);
        return std::chrono::system_clock::to_time_t(Fs::toSystemTime(time));
    }

    // Adds what's at path under name: a file, or a folder with everything in it, its entries in name order. On
    // Unix-like systems a symlink is stored as a symlink, as zip.extract makes one. The archive being written is
    // left out, for an archive written into the folder it packs.
    static bool packPath(Writer& writer, const std::string& name, const fs::path& path, const fs::path& output, bool compress,
        std::string& error) {
        std::error_code ec;
        fs::file_status status = fs::symlink_status(path, ec);
        if (ec) {
            error = "could not read '" + pathToString(path) + "': " + ec.message();
            return false;
        }

        if (fs::is_symlink(status)) {
#if defined(_WIN32)
            status = fs::status(path, ec);
#else
            fs::path target = fs::read_symlink(path, ec);
            if (ec) {
                error = "could not read the link '" + pathToString(path) + "': " + ec.message();
                return false;
            }
            return writer.addFile(name, target.string(), unixModeOf(path), modifiedOf(path), false, error);
#endif
        }

        if (fs::is_directory(status)) {
            if (!name.empty() && !writer.addDirectory(name + "/", unixModeOf(path), modifiedOf(path), error)) return false;
            std::vector<fs::path> children;
            for (fs::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec)) children.push_back(it->path());
            if (ec) {
                error = "could not list '" + pathToString(path) + "': " + ec.message();
                return false;
            }
            std::sort(children.begin(), children.end());
            for (const fs::path& child : children) {
                std::error_code same;
                if (fs::equivalent(child, output, same)) continue;
                std::string childName = pathToString(child.filename());
                if (!packPath(writer, name.empty() ? childName : name + "/" + childName, child, output, compress, error)) return false;
            }
            return true;
        }

        if (!fs::is_regular_file(status)) return true; // Sockets, devices and the like have nothing to store
        if (name.empty()) {
            error = "the file '" + pathToString(path) + "' needs an entry name";
            return false;
        }
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            error = "could not open '" + pathToString(path) + "'";
            return false;
        }
        std::string contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (file.bad()) {
            error = "could not read '" + pathToString(path) + "'";
            return false;
        }
        return writer.addFile(name, contents, unixModeOf(path), modifiedOf(path), compress, error);
    }

    // zip.pack(path: string, sources: string | { [string]: string }, options: { compress: boolean? }?) -> { entries: number, size: number }
    // Writes a zip archive, replacing a file already there. Given a folder, everything in it, named from the folder.
    // Given a table, each entry name and the file or folder that goes under it. Entries are deflated unless
    // options.compress is false, each one only when that makes it smaller.
    static int zip_pack(lua_State* L) {
        fs::path path = checkPath(L, 1);

        std::vector<std::pair<std::string, fs::path>> sources;
        if (lua_type(L, 2) == LUA_TSTRING) {
            sources.emplace_back("", checkPath(L, 2));
        } else {
            luaL_checktype(L, 2, LUA_TTABLE);
            lua_pushnil(L);
            while (lua_next(L, 2)) {
                if (lua_type(L, -2) != LUA_TSTRING) luaL_error(L, "sources must be keyed by entry names");
                if (lua_type(L, -1) != LUA_TSTRING) luaL_error(L, "each source must be a path");
                size_t length = 0;
                const char* key = lua_tolstring(L, -2, &length);
                std::string name(key, length);
                for (char& c : name) if (c == '\\') c = '/';
                while (!name.empty() && name.back() == '/') name.pop_back();
                // An entry zip.extract would refuse to write isn't written either
                if (name.empty() || !entryTarget(fs::path("."), name)) luaL_error(L, "the entry name '%s' would be extracted outside the folder", key);
                sources.emplace_back(std::move(name), checkPath(L, -1));
                lua_pop(L, 1);
            }
            std::sort(sources.begin(), sources.end());
        }

        bool compress = true;
        if (!lua_isnoneornil(L, 3)) {
            luaL_checktype(L, 3, LUA_TTABLE);
            lua_getfield(L, 3, "compress");
            if (!lua_isnil(L, -1)) {
                luaL_checktype(L, -1, LUA_TBOOLEAN);
                compress = lua_toboolean(L, -1);
            }
            lua_pop(L, 1);
        }

        return Luwow::Engine::runOffThread(L, zipHost, [path, sources, compress]() {
            Luwow::Engine::OffThreadResult result;
            std::string error;
            size_t count = 0;
            uint64_t size = 0;
            {
                Writer writer;
                if (writer.open(path, error)) {
                    for (const auto& [name, source] : sources) {
                        std::error_code ec;
                        if (!fs::exists(fs::symlink_status(source, ec))) {
                            error = "there's nothing at '" + pathToString(source) + "'";
                            break;
                        }
                        if (!packPath(writer, name, source, path, compress, error)) break;
                    }
                    if (error.empty()) writer.finish(error);
                }
                count = writer.entryCount();
                size = writer.size();
            }
            // A partly written archive isn't left behind
            if (!error.empty()) {
                std::error_code ec;
                fs::remove(path, ec);
                result.error = errorText("Could not write archive", path, error);
                return result;
            }

            result.push = [count, size](lua_State* L) {
                lua_createtable(L, 0, 2);
                lua_pushinteger(L, static_cast<int>(count));
                lua_setfield(L, -2, "entries");
                lua_pushnumber(L, static_cast<double>(size));
                lua_setfield(L, -2, "size");
                return 1;
            };
            return result;
        });
    }

    const char* Library::getModuleName() const {
        return "zip";
    }

    const char* Library::getModuleAlias() const {
        return "Luwow";
    }

    static LuauExport exports[] = {
        { "read", zip_read, Luwow::Engine::finishOffThread },
        { "hashEntries", zip_hashEntries, Luwow::Engine::finishOffThread },
        { "extract", zip_extract, Luwow::Engine::finishOffThread },
        { "pack", zip_pack, Luwow::Engine::finishOffThread },
        { nullptr, nullptr }
    };

    Luwow::Engine::RunMode Library::getRunMode() const {
        return LUWOW_MODULE_RUN_MODE;
    }

    const LuauExport* Library::getExports() const {
        return exports;
    }
} // namespace Luwow::Zip

LUWOW_REGISTER_MODULE(Luwow::Zip::Library)
