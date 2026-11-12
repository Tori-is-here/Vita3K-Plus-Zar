// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#pragma once

#include <util/fs.h>

#include <cstdint>
#include <ctime>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class ZArchiveReader;

/**
 * Read-only mounts of .zar (ZArchive) game packages.
 *
 * A mount maps a host directory (e.g. <vita_fs>/ux0/app/PCSA00029) onto a directory inside a .zar file
 * (e.g. app/PCSA00029). Host files always take precedence, the archive is only consulted for paths
 * that do not exist on the host, so the game is played straight from the archive without extraction.
 */
namespace zarfs {

// Name of the file placed in ux0/app/<TITLE_ID>/ that points at the .zar holding the game data.
inline constexpr const char *LINK_FILE_NAME = "zar_link.txt";

using ReaderPtr = std::shared_ptr<ZArchiveReader>;

struct Node {
    ReaderPtr reader;
    uint32_t handle = 0;
    bool is_dir = false;
    uint64_t size = 0;
    std::time_t mtime = 0;
};

struct DirEntry {
    std::string name;
    bool is_dir = false;
};

// An opened file inside a mounted archive (own read position, shared reader).
class File {
public:
    explicit File(const Node &node)
        : node(node) {}

    uint64_t read(void *data, uint64_t size);
    bool seek(int64_t offset, int whence); // whence: SEEK_SET / SEEK_CUR / SEEK_END
    uint64_t tell() const { return position; }
    uint64_t size() const { return node.size; }

private:
    Node node;
    uint64_t position = 0;
};

ReaderPtr open_archive(const fs::path &zar_path);

// Mount inner_path of zar_path at host_path. Returns false if the archive or inner directory is missing.
bool mount(const fs::path &host_path, const fs::path &zar_path, const std::string &inner_path);
void unmount_all();
bool has_mounts();

// Link file handling: <app_dir>/zar_link.txt holds the archive path and the inner directory.
bool write_link(const fs::path &app_dir, const fs::path &zar_path, const std::string &inner_path);
bool read_link(const fs::path &app_dir, fs::path &zar_path, std::string &inner_path);
bool is_linked(const fs::path &app_dir);
// Mounts app_dir from its link file, if it has one. Returns false only if a link exists but cannot be mounted.
bool mount_from_link(const fs::path &app_dir);

// Lookups by host path. All of them return "not found" when no mount covers the path.
std::optional<Node> lookup(const fs::path &host_path);
bool exists(const fs::path &host_path);
bool is_file(const fs::path &host_path);
bool is_directory(const fs::path &host_path);
std::shared_ptr<File> open(const fs::path &host_path);
bool read_all(const fs::path &host_path, std::vector<uint8_t> &out);
bool list_dir(const fs::path &host_path, std::vector<DirEntry> &out);

} // namespace zarfs
