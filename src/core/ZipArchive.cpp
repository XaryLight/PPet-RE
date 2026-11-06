// PPet - C++ 移植版
// ZIP 归档读取实现（详见 ZipArchive.hpp）。
#include "core/ZipArchive.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>

#include "core/FileIO.hpp"
#include "core/Log.hpp"
#include "zlib/zlib.h"

namespace ppet {
namespace {

namespace fs = std::filesystem;

/// ZIP 通用位标记。
constexpr std::uint16_t kFlagEncrypted = 0x0001;
constexpr std::uint16_t kFlagDataDescriptor = 0x0008;
constexpr std::uint16_t kFlagStrongEncryption = 0x0040;

/// 压缩方式。
constexpr std::uint16_t kMethodStore = 0;
constexpr std::uint16_t kMethodDeflate = 8;

/// 中央目录文件头签名 "PK\1\2"。
constexpr std::uint32_t kSignatureCentral = 0x02014b50;
/// 本地文件头签名 "PK\3\4"。
constexpr std::uint32_t kSignatureLocal = 0x04034b50;
/// 中央目录结束记录签名 "PK\5\6"。
constexpr std::uint32_t kSignatureEnd = 0x06054b50;

std::uint16_t ReadU16(const unsigned char* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t ReadU32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

/// ZIP 条目在中央目录中的原始信息。
struct CentralEntry {
    std::string name;
    std::uint16_t method = 0;
    std::uint16_t flags = 0;
    std::uint32_t crc = 0;
    std::uint64_t compressedSize = 0;
    std::uint64_t uncompressedSize = 0;
    std::uint64_t localHeaderOffset = 0;
};

/// 解析中央目录。
bool ParseCentralDirectory(const std::vector<unsigned char>& data,
                            std::vector<CentralEntry>& entries) {
    if (data.size() < 22) return false;

    // 从尾部向前找中央目录结束记录（EOCD）。
    // EOCD 固定 22 字节，注释最长 65535 字节。
    std::size_t eocd = 0;
    bool found = false;
    const std::size_t maxBack = std::min<std::size_t>(data.size(), 22 + 65535);
    for (std::size_t back = 22; back <= maxBack; ++back) {
        const std::size_t pos = data.size() - back;
        if (ReadU32(&data[pos]) == kSignatureEnd) {
            eocd = pos;
            found = true;
            break;
        }
    }
    if (!found) return false;

    const std::uint16_t count = ReadU16(&data[eocd + 10]);
    const std::uint32_t dirSize = ReadU32(&data[eocd + 12]);
    const std::uint32_t dirOffset = ReadU32(&data[eocd + 16]);

    if (dirOffset > data.size() || dirOffset + dirSize > data.size()) return false;

    std::size_t pos = dirOffset;
    for (std::uint16_t i = 0; i < count; ++i) {
        if (pos + 46 > data.size()) return false;
        if (ReadU32(&data[pos]) != kSignatureCentral) return false;

        CentralEntry entry;
        entry.flags = ReadU16(&data[pos + 8]);
        entry.method = ReadU16(&data[pos + 10]);
        entry.crc = ReadU32(&data[pos + 16]);
        entry.compressedSize = ReadU32(&data[pos + 20]);
        entry.uncompressedSize = ReadU32(&data[pos + 24]);
        const std::uint16_t nameLen = ReadU16(&data[pos + 28]);
        const std::uint16_t extraLen = ReadU16(&data[pos + 30]);
        const std::uint16_t commentLen = ReadU16(&data[pos + 32]);
        entry.localHeaderOffset = ReadU32(&data[pos + 42]);

        if (pos + 46 + nameLen > data.size()) return false;
        entry.name.assign(reinterpret_cast<const char*>(&data[pos + 46]), nameLen);

        // ZIP64 扩展：真实的 64 位大小写在 extra field 里。
        // 模型包一般不会超过 4 GB，这里只做「存在扩展就拒绝」的保守处理，
        // 避免解析出被截断的尺寸。
        if (entry.uncompressedSize == 0xFFFFFFFFu ||
            entry.compressedSize == 0xFFFFFFFFu ||
            entry.localHeaderOffset == 0xFFFFFFFFu) {
            PPET_LOG_ERR("ZIP 含 ZIP64 扩展，当前不支持：%s\n", entry.name.c_str());
            return false;
        }

        entries.push_back(std::move(entry));
        pos += 46u + nameLen + extraLen + commentLen;
    }
    return true;
}

/// 用 raw inflate 解压数据（ZIP 的 deflate 不带 zlib 头尾）。
bool RawInflate(const unsigned char* src, std::size_t srcSize,
                std::vector<unsigned char>& out, std::size_t expectedSize) {
    out.clear();
    if (expectedSize > 0) out.reserve(expectedSize);

    z_stream stream{};
    // -MAX_WBITS 表示「裸 deflate」，正好对应 ZIP 里的压缩方式 8。
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) return false;

    stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(src));
    stream.avail_in = static_cast<uInt>(srcSize);

    unsigned char chunk[64 * 1024];
    int status = Z_OK;
    do {
        stream.next_out = chunk;
        stream.avail_out = static_cast<uInt>(sizeof(chunk));
        status = inflate(&stream, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END && status != Z_BUF_ERROR) {
            inflateEnd(&stream);
            return false;
        }
        const std::size_t produced = sizeof(chunk) - stream.avail_out;
        if (produced > 0) out.insert(out.end(), chunk, chunk + produced);
        // Z_BUF_ERROR 且没有产出：输入耗尽但流未结束，属于数据截断。
        if (status == Z_BUF_ERROR && produced == 0) {
            inflateEnd(&stream);
            return false;
        }
    } while (status != Z_STREAM_END);

    inflateEnd(&stream);
    return true;
}

/// 读取某个条目的原始（可能已压缩）数据。
bool ReadRawEntryData(const std::vector<unsigned char>& archive,
                       const CentralEntry& entry, std::vector<unsigned char>& out) {
    const std::size_t local = static_cast<std::size_t>(entry.localHeaderOffset);
    if (local + 30 > archive.size()) return false;
    if (ReadU32(&archive[local]) != kSignatureLocal) return false;

    const std::uint16_t nameLen = ReadU16(&archive[local + 26]);
    const std::uint16_t extraLen = ReadU16(&archive[local + 28]);
    const std::size_t dataStart = local + 30u + nameLen + extraLen;
    if (dataStart + entry.compressedSize > archive.size()) return false;

    out.assign(archive.begin() + static_cast<std::ptrdiff_t>(dataStart),
               archive.begin() + static_cast<std::ptrdiff_t>(dataStart + entry.compressedSize));
    return true;
}

/// 解压单个条目到内存。
bool DecodeEntry(const std::vector<unsigned char>& archive, const CentralEntry& entry,
                 std::vector<unsigned char>& out) {
    if (entry.flags & (kFlagEncrypted | kFlagStrongEncryption)) {
        PPET_LOG_ERR("ZIP 条目已加密，无法解压：%s\n", entry.name.c_str());
        return false;
    }

    std::vector<unsigned char> raw;
    if (!ReadRawEntryData(archive, entry, raw)) return false;

    if (entry.method == kMethodStore) {
        out.swap(raw);
        return true;
    }
    if (entry.method != kMethodDeflate) {
        PPET_LOG_ERR("ZIP 条目使用了不支持的压缩方式 %u：%s\n",
                     static_cast<unsigned>(entry.method), entry.name.c_str());
        return false;
    }
    return RawInflate(raw.data(), raw.size(), out,
                      static_cast<std::size_t>(entry.uncompressedSize));
}

/// 校验解压后的数据大小与声明一致（防止畸形包越界）。
bool CheckSize(const CentralEntry& entry, std::size_t actual) {
    const std::size_t expected = static_cast<std::size_t>(entry.uncompressedSize);
    if (expected != actual) {
        PPET_LOG_ERR("ZIP 条目大小不符：%s（期望 %u，实际 %u）\n", entry.name.c_str(),
                     static_cast<unsigned>(expected), static_cast<unsigned>(actual));
        return false;
    }
    return true;
}

/// 把条目名里的反斜杠统一成 '/',并拒绝路径穿越。
std::string NormalizeEntryName(const std::string& raw) {
    std::string name = raw;
    for (char& c : name) {
        if (c == '\\') c = '/';
    }
    return name;
}

/// 判断条目是否是目录。
bool IsDirectoryEntry(const CentralEntry& entry) {
    return !entry.name.empty() && entry.name.back() == '/';
}

/// 校验目标路径没有跳出解压目录。
bool IsSafeRelativePath(const std::string& name) {
    if (name.empty()) return false;
    if (name[0] == '/' || name[0] == '\\') return false;
    // Windows 盘符（如 "C:evil"）也要拒绝。
    if (name.size() >= 2 && name[1] == ':') return false;
    std::size_t start = 0;
    while (start <= name.size()) {
        const std::size_t slash = name.find('/', start);
        const std::string part = name.substr(
            start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part == "..") return false;
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return true;
}

}  // namespace

bool ListZipEntries(const std::string& zipPath, std::vector<ZipEntry>& entries) {
    entries.clear();
    std::vector<unsigned char> archive;
    if (!ReadFileBytes(zipPath, archive)) {
        PPET_LOG_ERR("无法读取 ZIP 文件：%s\n", zipPath.c_str());
        return false;
    }
    std::vector<CentralEntry> central;
    if (!ParseCentralDirectory(archive, central)) {
        PPET_LOG_ERR("ZIP 文件格式非法：%s\n", zipPath.c_str());
        return false;
    }
    for (const CentralEntry& entry : central) {
        if (IsDirectoryEntry(entry)) continue;
        ZipEntry item;
        item.name = NormalizeEntryName(entry.name);
        item.size = static_cast<std::size_t>(entry.uncompressedSize);
        entries.push_back(std::move(item));
    }
    return true;
}

bool ReadZipEntry(const std::string& zipPath, const std::string& name,
                  std::vector<unsigned char>& out) {
    std::vector<unsigned char> archive;
    if (!ReadFileBytes(zipPath, archive)) return false;
    std::vector<CentralEntry> central;
    if (!ParseCentralDirectory(archive, central)) return false;

    const std::string wanted = NormalizeEntryName(name);
    for (const CentralEntry& entry : central) {
        if (NormalizeEntryName(entry.name) != wanted) continue;
        if (!DecodeEntry(archive, entry, out)) return false;
        return CheckSize(entry, out.size());
    }
    return false;
}

bool ExtractZipToDirectory(const std::string& zipPath, const std::string& destDir,
                            std::string& error) {
    error.clear();

    std::vector<unsigned char> archive;
    if (!ReadFileBytes(zipPath, archive)) {
        error = "无法读取 ZIP 文件：" + zipPath;
        return false;
    }
    std::vector<CentralEntry> central;
    if (!ParseCentralDirectory(archive, central)) {
        error = "ZIP 文件格式非法：" + zipPath;
        return false;
    }

    std::error_code ec;
    fs::create_directories(fs::path(destDir), ec);
    if (ec) {
        error = "无法创建解压目录：" + destDir;
        return false;
    }

    for (const CentralEntry& entry : central) {
        const std::string name = NormalizeEntryName(entry.name);
        if (name.empty()) continue;
        if (!IsSafeRelativePath(name)) {
            error = "ZIP 包含非法路径，已拒绝解压：" + entry.name;
            return false;
        }

        const fs::path target = fs::path(destDir) / fs::path(name);
        if (IsDirectoryEntry(entry)) {
            fs::create_directories(target, ec);
            continue;
        }

        // 创建父目录。
        const fs::path parent = target.parent_path();
        if (!parent.empty()) {
            fs::create_directories(parent, ec);
            if (ec) {
                error = "无法创建目录：" + parent.string();
                return false;
            }
        }

        std::vector<unsigned char> data;
        if (!DecodeEntry(archive, entry, data)) {
            error = "解压条目失败：" + name;
            return false;
        }
        if (!CheckSize(entry, data.size())) {
            error = "条目大小校验失败：" + name;
            return false;
        }
        if (!WriteFileBytes(target.string(), data)) {
            error = "无法写入文件：" + target.string();
            return false;
        }
    }
    return true;
}

}  // namespace ppet
