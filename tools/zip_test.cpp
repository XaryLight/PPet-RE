// ZIP 模型包导入测试：解压 -> 定位模型 -> 校验关键文件存在。
//
// 用法：ppet_zip_test <zip 路径> [期望的模型文件名]
// 不传 zip 路径时使用内置生成的测试包（tools/testdata 里）。
#include <cstdio>
#include <filesystem>
#include <string>

#include <src/tools/core/FileIO.hpp>
#include <src/tools/core/Log.hpp>
#include <src/tools/core/ModelPackage.hpp>
#include <src/tools/core/ZipArchive.hpp>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (condition) {
        std::printf("  [通过] %s\n", what);
    } else {
        std::printf("  [失败] %s\n", what);
        ++g_failures;
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "用法: %s <zip 路径> [期望模型文件名]\n", argv[0]);
        return 2;
    }
    const std::string zipPath = argv[1];
    const std::string expected = argc >= 3 ? argv[2] : std::string();

    ppet::Logger::Init("");

    std::printf("== ZIP 模型包导入测试：%s ==\n", zipPath.c_str());

    // --- 1. 列出条目 ---
    std::vector<ppet::ZipEntry> entries;
    const bool listed = ppet::ListZipEntries(zipPath, entries);
    Check(listed, "能解析 ZIP 中央目录");
    if (!listed) {
        ppet::Logger::Shutdown();
        return 1;
    }
    std::printf("  条目数：%zu\n", entries.size());
    Check(!entries.empty(), "条目列表非空");
    for (const ppet::ZipEntry& entry : entries) {
        std::printf("    %s (%zu 字节)\n", entry.name.c_str(), entry.size);
    }

    // --- 2. 解压并定位模型 ---
    const std::string cacheRoot =
        (fs::temp_directory_path() / "ppet_zip_test_cache").string();
    fs::remove_all(fs::path(cacheRoot));

    ppet::ModelPackage package;
    std::string error;
    const bool extracted =
        ppet::ExtractModelPackage(zipPath, cacheRoot, package, error);
    Check(extracted, extracted ? "能解压模型包" : ("解压失败：" + error).c_str());
    if (!extracted) {
        ppet::Logger::Shutdown();
        return 1;
    }

    std::printf("  解压目录：%s\n", package.directory.c_str());
    std::printf("  定位模型：%s\n", package.modelPath.c_str());

    Check(ppet::FileExists(package.modelPath), "定位到的模型文件真实存在");
    Check(!package.directory.empty(), "解压目录非空");

    // 配套资源校验：Live2D 需要同名 .moc3 与贴图；PMX/VRM 可能是单文件模型。
    const std::string modelDir = ppet::ParentDirectory(package.modelPath);
    const std::string modelStem =
        package.modelFileName.substr(0, package.modelFileName.find_last_of('.'));
    const bool live2dCompanion =
        ppet::FileExists(ppet::JoinPath(modelDir, modelStem + ".moc3"));
    const bool hasTextures =
        ppet::FileExists(ppet::JoinPath(modelDir, "textures"));
    const bool singleFile = package.modelFileName.find(".pmx") != std::string::npos ||
                            package.modelFileName.find(".vrm") != std::string::npos;
    Check(live2dCompanion || hasTextures || singleFile,
          "模型配套资源（moc3 / 贴图目录 / 单文件模型）随包解压");

    if (!expected.empty()) {
        Check(package.modelFileName == expected,
              ("定位到的模型文件名 == " + expected).c_str());
    }

    // --- 3. 幂等性：重复导入不应失败 ---
    ppet::ModelPackage second;
    const bool again =
        ppet::ExtractModelPackage(zipPath, cacheRoot, second, error);
    Check(again, "重复导入同一压缩包仍然成功");

    // --- 4. 非法输入 ---
    std::vector<ppet::ZipEntry> bogus;
    Check(!ppet::ListZipEntries("/tmp/__not_exist__.zip", bogus),
          "不存在的文件返回失败");

    fs::remove_all(fs::path(cacheRoot));

    ppet::Logger::Shutdown();
    std::printf("== %s ==\n", g_failures == 0 ? "全部通过" : "存在失败项");
    return g_failures == 0 ? 0 : 1;
}
