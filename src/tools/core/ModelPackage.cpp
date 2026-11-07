// PPet - C++ 移植版
// 模型包（zip）导入实现。详见 ModelPackage.hpp。
#include <src/tools/core/ModelPackage.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <unordered_map>

#include <src/tools/core/FileIO.hpp>
#include <src/tools/core/Log.hpp>
#include <src/tools/core/ZipArchive.hpp>

namespace ppet {
namespace {

namespace fs = std::filesystem;

/// 递归收集目录下的所有文件（相对路径用 '/' 分隔）。
void CollectFiles(const fs::path& root, const fs::path& current,
                  std::vector<std::string>& out) {
    std::error_code ec;
    if (!fs::is_directory(current, ec)) return;
    for (fs::directory_iterator it(current, ec), end; it != end && !ec; it.increment(ec)) {
        if (ec) break;
        const fs::path& entry = it->path();
        if (fs::is_directory(entry, ec)) {
            CollectFiles(root, entry, out);
            continue;
        }
        fs::path relative = fs::relative(entry, root, ec);
        if (ec) continue;
        // 相对路径统一用 '/' 分隔；转 UTF-8 以免中文路径在 Windows 上乱码。
        out.push_back(FromFsPathGeneric(relative));
    }
}

/// 取 '/' 分隔路径的目录部分（"a/b/c.png" -> "a/b"；"c.png" -> ""）。
std::string DirOfGeneric(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string LowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// 是否是 Live2D 的模型配置文件。
bool IsLive2DManifest(const std::string& fileName) {
    const std::string lower = LowerAscii(fileName);
    // C++23：contains 比 find(...) != npos 直接得多（老标准库自动回退）。
#if defined(__cpp_lib_string_contains)
    return lower.contains(".model3.json") || lower.contains(".model.json");
#else
    return lower.find(".model3.json") != std::string::npos ||
           lower.find(".model.json") != std::string::npos;
#endif
}

/// Live2D 模型配置的优先级：Cubism 3（model3.json）优先于 Cubism 2（model.json）。
int Live2DPriority(const std::string& fileName) {
    const std::string lower = LowerAscii(fileName);
#if defined(__cpp_lib_string_contains)
    if (lower.contains(".model3.json")) return 3;
    if (lower.contains(".model.json")) return 2;
#else
    if (lower.find(".model3.json") != std::string::npos) return 3;
    if (lower.find(".model.json") != std::string::npos) return 2;
#endif
    return 0;
}

/// 由模型主文件名判断种类。
PackageModelKind KindOfModelFile(const std::string& rel) {
    const std::string ext = FileExtension(rel);
    if (IsLive2DManifest(rel)) return PackageModelKind::Live2D;
    if (ext == "pmx") return PackageModelKind::Pmx;
    if (ext == "vrm") return PackageModelKind::Vrm;
    return PackageModelKind::Unknown;
}

/// 统计「每个目录（含其子目录）下共有多少张贴图」。
/// 用于多个 PMX 候选时挑出真正带有贴图的那个。
std::unordered_map<std::string, int> BuildTextureScore(const std::vector<std::string>& files) {
    std::unordered_map<std::string, int> score;
    for (const std::string& rel : files) {
        if (!IsModelTextureFile(rel)) continue;
        std::string dir = DirOfGeneric(rel);
        for (;;) {
            ++score[dir];
            if (dir.empty()) break;
            const std::size_t slash = dir.find_last_of('/');
            if (slash == std::string::npos) {
                dir.clear();
            } else {
                dir.resize(slash);
            }
        }
    }
    return score;
}

/// 生成解压目录名：用 zip 文件名 + 内容指纹，避免同名包互相覆盖。
std::string MakeCacheDirName(const std::string& zipPath) {
    const std::string base = FileName(zipPath);
    std::string stem = base;
    const std::size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos) stem = stem.substr(0, dot);

    // 文件名可能含中文与非法字符，这里只保留「字母数字、连字符、下划线」，
    // 其余替换成下划线，并追加路径哈希以保证唯一性。
    std::string safe;
    safe.reserve(stem.size());
    for (char c : stem) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) || c == '-' || c == '_') {
            safe.push_back(c);
        } else {
            safe.push_back('_');
        }
    }
    if (safe.empty()) safe = "model";

    // 用路径的简单 FNV-1a 哈希做区分。
    std::uint64_t hash = 1469598103934665603ull;
    for (char c : zipPath) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ull;
    }
    char suffix[32];
    std::snprintf(suffix, sizeof(suffix), "%08x", static_cast<unsigned>(hash & 0xFFFFFFFFull));
    return safe + "_" + suffix;
}

}  // namespace

const char* PackageModelKindName(PackageModelKind kind) {
    switch (kind) {
        case PackageModelKind::Live2D: return "Live2D";
        case PackageModelKind::Pmx:    return "PMX";
        case PackageModelKind::Vrm:    return "VRM";
        case PackageModelKind::Unknown: break;
    }
    return "未知";
}

bool IsModelTextureFile(const std::string& name) {
    const std::string ext = FileExtension(name);  // 已转小写
    static constexpr const char* kExtensions[] = {
        "png", "jpg", "jpeg", "tga", "bmp", "dds", "webp",
        "gif", "tif", "tiff", "sph", "spa", "ktx", "ktx2",
    };
    for (const char* candidate : kExtensions) {
        if (ext == candidate) return true;
    }
    return false;
}

bool FindModelInDirectory(const std::string& directory, std::string& out) {
    out.clear();
    std::error_code ec;
    const fs::path root = ToFsPath(directory);
    if (!fs::is_directory(root, ec)) return false;

    std::vector<std::string> files;
    CollectFiles(root, root, files);
    if (files.empty()) return false;

    const auto textureScore = BuildTextureScore(files);

    // 逐个文件判断类型，优先级：Live2D(model3.json) > PMX > VRM。
    // PMX 额外加分：同目录（含子目录）贴图越多越可能是「可直接渲染」的完整模型。
    // 加分上限 40，保证不会反超 Live2D 的 100。
    int bestPriority = -1;
    std::size_t bestLength = 0;

    for (const std::string& rel : files) {
        const std::string ext = FileExtension(rel);
        int priority = 0;
        if (IsLive2DManifest(rel)) {
            priority = 100 + Live2DPriority(rel);
        } else if (ext == "pmx") {
            const auto it = textureScore.find(DirOfGeneric(rel));
            const int bonus = it == textureScore.end() ? 0 : std::min(it->second, 40);
            priority = 50 + bonus;
        } else if (ext == "vrm") {
            priority = 40;
        }
        if (priority == 0) continue;

        if (priority > bestPriority ||
            (priority == bestPriority && rel.size() < bestLength)) {
            bestPriority = priority;
            bestLength = rel.size();
            out = JoinPath(directory, rel);
        }
    }
    return bestPriority > 0;
}

bool ExtractModelPackage(const std::string& zipPath, const std::string& cacheRoot,
                         ModelPackage& out, std::string& error) {
    error.clear();
    out = ModelPackage();

    const auto started = std::chrono::steady_clock::now();
    PPET_LOG_TAG_DEBUG("PKG", "开始导入模型包：%s", zipPath.c_str());

    std::error_code ec;
    fs::create_directories(ToFsPath(cacheRoot), ec);
    if (ec) {
        error = "无法创建缓存目录：" + cacheRoot;
        PPET_LOG_TAG_ERR("PKG", "%s", error.c_str());
        return false;
    }

    // 先读中央目录做统计：条目数、贴图数、解压后总字节数。
    // 这一步不写磁盘，失败不致命（老 zip 或加密包可能列不出条目，仍继续解压）。
    std::vector<ZipEntry> entries;
    if (ListZipEntries(zipPath, entries)) {
        out.entryCount = entries.size();
        out.totalBytes = 0;
        for (const ZipEntry& entry : entries) {
            out.totalBytes += entry.size;
            if (IsModelTextureFile(entry.name)) ++out.textureCount;
        }
    } else {
        PPET_LOG_TAG_WARN("PKG", "无法读取 ZIP 中央目录统计信息：%s（仍尝试解压）",
                          zipPath.c_str());
    }

    const std::string dirName = MakeCacheDirName(zipPath);
    const std::string destDir = JoinPath(cacheRoot, dirName);

    // 每次导入都重新解压：模型包可能被用户更新过。
    fs::remove_all(ToFsPath(destDir), ec);
    if (!ExtractZipToDirectory(zipPath, destDir, error)) {
        PPET_LOG_TAG_ERR("PKG", "解压失败：%s -> %s：%s", zipPath.c_str(),
                         destDir.c_str(), error.c_str());
        return false;
    }

    std::string modelPath;
    if (!FindModelInDirectory(destDir, modelPath)) {
        error = "压缩包内没有找到模型文件（支持 .model3.json / .pmx / .vrm）";
        PPET_LOG_TAG_ERR("PKG", "%s：%s", error.c_str(), zipPath.c_str());
        return false;
    }

    out.directory = destDir;
    out.modelPath = modelPath;
    out.modelFileName = FileName(modelPath);
    out.modelDirectory = ParentDirectory(modelPath);
    out.sourceArchive = zipPath;
    out.kind = KindOfModelFile(out.modelFileName);

    const double elapsedMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
    PPET_LOG_TAG("PKG", "模型包已解压：%s（%zu 条目 / %zu 贴图 / %.2f MiB / %.1f ms）",
                 out.sourceArchive.c_str(), out.entryCount, out.textureCount,
                 static_cast<double>(out.totalBytes) / (1024.0 * 1024.0), elapsedMs);
    PPET_LOG_TAG("PKG", "定位到 %s 模型：%s（基准目录 %s）",
                 PackageModelKindName(out.kind), out.modelPath.c_str(),
                 out.modelDirectory.c_str());

    // PMX 不内嵌图形数据：包里一张贴图都没有时，几乎必然渲染成白模，提前说清楚。
    if (out.kind == PackageModelKind::Pmx && out.textureCount == 0) {
        PPET_LOG_TAG_WARN(
            "PKG",
            "该 PMX 包内没有发现任何贴图文件。PMX 本身不包含图形数据，"
            "只有贴图路径；请把 .pmx 与 tex/ 等贴图目录一起打包成 .zip 再导入，"
            "否则模型会显示为纯色/白模。");
    }
    return true;
}

}  // namespace ppet
