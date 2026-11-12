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

#include <io/zar_mount.h>

#include <util/log.h>

#include <zarchive/zarchivereader.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <shared_mutex>

namespace zarfs {

namespace {

struct Mount {
    std::string host_prefix; // normalized, no trailing slash
    std::string inner_prefix; // no trailing slash, may be empty
    ReaderPtr reader;
    std::time_t mtime = 0;
};

std::shared_mutex mounts_mutex;
std::vector<Mount> mounts;

// Generic (forward slash) form without "." / ".." / duplicate or trailing slashes.
std::string normalize(const fs::path &path) {
    std::string result = fs_utils::path_to_utf8(path.lexically_normal().generic_path());
    while (result.size() > 1 && result.back() == '/')
        result.pop_back();
    if (result.size() > 2 && result.ends_with("/."))
        result.erase(result.size() - 2);
    return result;
}

char fold(char c) {
#ifdef _WIN32
    // Host paths are case-insensitive on Windows
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
#else
    return c;
#endif
}

// Returns the part of path below prefix ("" when equal), or nullopt when path is not inside prefix.
std::optional<std::string> strip_prefix(const std::string &path, const std::string &prefix) {
    if (path.size() < prefix.size())
        return std::nullopt;
    if (!std::equal(prefix.begin(), prefix.end(), path.begin(), [](char a, char b) { return fold(a) == fold(b); }))
        return std::nullopt;
    if (path.size() == prefix.size())
        return std::string{};
    if (path[prefix.size()] != '/')
        return std::nullopt;
    return path.substr(prefix.size() + 1);
}

std::string join(const std::string &base, const std::string &rel) {
    if (base.empty())
        return rel;
    if (rel.empty())
        return base;
    return base + "/" + rel;
}

} // namespace

uint64_t File::read(void *data, uint64_t size) {
    if (position >= node.size || size == 0)
        return 0;
    const uint64_t read = node.reader->ReadFromFile(node.handle, position, size, data);
    position += read;
    return read;
}

bool File::seek(int64_t offset, int whence) {
    int64_t base = 0;
    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = static_cast<int64_t>(position); break;
    case SEEK_END: base = static_cast<int64_t>(node.size); break;
    default: return false;
    }
    const int64_t target = base + offset;
    if (target < 0)
        return false;
    position = static_cast<uint64_t>(target);
    return true;
}

ReaderPtr open_archive(const fs::path &zar_path) {
    return ReaderPtr(ZArchiveReader::OpenFromFile(std::filesystem::path(zar_path.native())));
}

bool mount(const fs::path &host_path, const fs::path &zar_path, const std::string &inner_path) {
    ReaderPtr reader = open_archive(zar_path);
    if (!reader) {
        LOG_ERROR("Failed to open .zar archive: {}", zar_path);
        return false;
    }
    if (reader->LookUp(inner_path, false, true) == ZARCHIVE_INVALID_NODE) {
        LOG_ERROR("Directory {} not found inside .zar archive: {}", inner_path, zar_path);
        return false;
    }

    Mount m;
    m.host_prefix = normalize(host_path);
    m.inner_prefix = inner_path;
    m.reader = std::move(reader);
    boost::system::error_code ec;
    m.mtime = fs::last_write_time(zar_path, ec);

    std::unique_lock lock(mounts_mutex);
    std::erase_if(mounts, [&](const Mount &other) { return other.host_prefix == m.host_prefix; });
    LOG_INFO("Mounted {} [{}] at {}", zar_path, inner_path, m.host_prefix);
    mounts.push_back(std::move(m));
    return true;
}

void unmount_all() {
    std::unique_lock lock(mounts_mutex);
    mounts.clear();
}

bool has_mounts() {
    std::shared_lock lock(mounts_mutex);
    return !mounts.empty();
}

bool write_link(const fs::path &app_dir, const fs::path &zar_path, const std::string &inner_path) {
    fs::ofstream out(app_dir / LINK_FILE_NAME, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out.is_open())
        return false;
    boost::system::error_code ec;
    const fs::path absolute = fs::absolute(zar_path, ec);
    out << fs_utils::path_to_utf8(ec ? zar_path : absolute) << '\n'
        << inner_path << '\n';
    return out.good();
}

bool read_link(const fs::path &app_dir, fs::path &zar_path, std::string &inner_path) {
    fs::ifstream in(app_dir / LINK_FILE_NAME, std::ios::in | std::ios::binary);
    if (!in.is_open())
        return false;
    std::string archive;
    if (!std::getline(in, archive) || !std::getline(in, inner_path))
        return false;
    const auto trim = [](std::string &s) {
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n'))
            s.pop_back();
    };
    trim(archive);
    trim(inner_path);
    if (archive.empty())
        return false;
    zar_path = fs_utils::utf8_to_path(archive);
    return true;
}

bool is_linked(const fs::path &app_dir) {
    boost::system::error_code ec;
    return fs::exists(app_dir / LINK_FILE_NAME, ec);
}

bool mount_from_link(const fs::path &app_dir) {
    if (!is_linked(app_dir))
        return true;

    fs::path zar_path;
    std::string inner_path;
    if (!read_link(app_dir, zar_path, inner_path)) {
        LOG_ERROR("Invalid .zar link file in {}", app_dir);
        return false;
    }
    boost::system::error_code ec;
    if (!fs::exists(zar_path, ec)) {
        LOG_ERROR("The .zar archive of this game was moved or deleted: {}. Add the .zar file again.", zar_path);
        return false;
    }
    return mount(app_dir, zar_path, inner_path);
}

std::optional<Node> lookup(const fs::path &host_path) {
    std::shared_lock lock(mounts_mutex);
    if (mounts.empty())
        return std::nullopt;

    const std::string path = normalize(host_path);
    for (const auto &m : mounts) {
        const auto rel = strip_prefix(path, m.host_prefix);
        if (!rel)
            continue;
        const auto handle = m.reader->LookUp(join(m.inner_prefix, *rel), true, true);
        if (handle == ZARCHIVE_INVALID_NODE)
            return std::nullopt;

        Node node;
        node.reader = m.reader;
        node.handle = handle;
        node.is_dir = m.reader->IsDirectory(handle);
        node.size = node.is_dir ? 0 : m.reader->GetFileSize(handle);
        node.mtime = m.mtime;
        return node;
    }
    return std::nullopt;
}

bool exists(const fs::path &host_path) {
    return lookup(host_path).has_value();
}

bool is_file(const fs::path &host_path) {
    const auto node = lookup(host_path);
    return node && !node->is_dir;
}

bool is_directory(const fs::path &host_path) {
    const auto node = lookup(host_path);
    return node && node->is_dir;
}

std::shared_ptr<File> open(const fs::path &host_path) {
    const auto node = lookup(host_path);
    if (!node || node->is_dir)
        return nullptr;
    return std::make_shared<File>(*node);
}

bool read_all(const fs::path &host_path, std::vector<uint8_t> &out) {
    const auto node = lookup(host_path);
    if (!node || node->is_dir)
        return false;
    out.resize(node->size);
    return (node->size == 0) || (node->reader->ReadFromFile(node->handle, 0, node->size, out.data()) == node->size);
}

bool list_dir(const fs::path &host_path, std::vector<DirEntry> &out) {
    const auto node = lookup(host_path);
    if (!node || !node->is_dir)
        return false;
    const uint32_t count = node->reader->GetDirEntryCount(node->handle);
    for (uint32_t i = 0; i < count; i++) {
        ZArchiveReader::DirEntry entry;
        if (node->reader->GetDirEntry(node->handle, i, entry))
            out.push_back({ std::string(entry.name), entry.isDirectory });
    }
    return true;
}

} // namespace zarfs
