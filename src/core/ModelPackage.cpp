// PPet - C++ 移植版
// 模型包（zip）导入实现。详见 ModelPackage.hpp。
#include "core/ModelPackage.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>

#include "core/FileIO.hpp"
#include "core/Log.hpp"
#include "core/ZipArchive.hpp"

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
        std::string rel = relative.generic_string();
        std::replace(rel.begin(), rel.end(), '\\', '/');
        out.push_back(rel);
    }
}

/// 是否是 Live2D 的模型配置文件。
bool IsLive2DManifest(const std::string& fileName) {
    const std::string lower = [&] {
        std::string s = fileName;
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }();
    return lower.find(".model3.json") != std::string::npos ||
           lower.find(".model.json") != std::string::npos;
}

/// Live2D 模型配置的优先级：Cubism 3（model3.json）优先于 Cubism 2（model.json）。
int Live2DPriority(const std::string& fileName) {
    const std::string lower = [&] {
        std::string s = fileName;
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }();
    if (lower.find(".model3.json") != std::string::npos) return 3;
    if (lower.find(".model.json") != std::string::npos) return 2;
    return 0;
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

bool FindModelInDirectory(const std::string& directory, std::string& out) {
    out.clear();
    std::error_code ec;
    if (!fs::is_directory(fs::path(directory), ec)) return false;

    std::vector<std::string> files;
    CollectFiles(fs::path(directory), fs::path(directory), files);
    if (files.empty()) return false;

    // 逐个文件判断类型，优先级：Live2D(model3.json) > PMX > VRM。
    // PMX / VRM 都用扩展名判断；同名时取路径最短的那个（通常在包根目录）。
    int bestPriority = -1;
    std::size_t bestLength = 0;

    for (const std::string& rel : files) {
        const std::string ext = FileExtension(rel);
        int priority = 0;
        if (IsLive2DManifest(rel)) {
            priority = 100 + Live2DPriority(rel);
        } else if (ext == "pmx") {
            priority = 50;
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

    std::error_code ec;
    fs::create_directories(fs::path(cacheRoot), ec);
    if (ec) {
        error = "无法创建缓存目录：" + cacheRoot;
        return false;
    }

    const std::string dirName = MakeCacheDirName(zipPath);
    const std::string destDir = JoinPath(cacheRoot, dirName);

    // 每次导入都重新解压：模型包可能被用户更新过。
    fs::remove_all(fs::path(destDir), ec);
    if (!ExtractZipToDirectory(zipPath, destDir, error)) return false;

    std::string modelPath;
    if (!FindModelInDirectory(destDir, modelPath)) {
        error = "压缩包内没有找到模型文件"
                "（支持 .model3.json / .pmx / .vrm）";
        return false;
    }

    out.directory = destDir;
    out.modelPath = modelPath;
    out.modelFileName = FileName(modelPath);
    PPET_LOG("模型包已解压：%s -> %s\n", zipPath.c_str(), modelPath.c_str());
    return true;
}

}  // namespace ppet
