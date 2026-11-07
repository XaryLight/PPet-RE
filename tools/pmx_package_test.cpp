// PPet - C++ 移植版
// PMX 模型包（.zip）导入专项测试。
//
// 覆盖：
//   1. ExtractModelPackage 的统计字段（种类 / 条目数 / 贴图数 / 来源包 / 基准目录）；
//   2. 完整包 -> PmxModel 加载后无缺失资源，DebugInfo 的规模与格式字段正确；
//   3. 缺贴图的包 -> MissingResources() 精确报告缺失（PMX 不内嵌图形数据）；
//   4. 贴图大小写不一致 / 放在子目录 -> ResolveTexturePath 容错后仍能找到。
//
// 不创建 GL 上下文：PmxModel::Load 只做 CPU 侧解析，GPU 资源在 Render() 才建立。
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <src/tools/core/FileIO.hpp>
#include <src/tools/core/Log.hpp>
#include <src/tools/core/ModelPackage.hpp>
#include <src/tools/model/pmx/PmxModel.hpp>

namespace {

int g_failures = 0;

void Check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what.c_str());
    if (!ok) ++g_failures;
}

struct Case {
    const char* file;
    std::size_t expectedEntries;
    std::size_t expectedTextures;
    bool expectMissing;
    const char* label;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "用法: %s <zip 目录>\n", argv[0]);
        return 2;
    }
    const std::string zipDir = argv[1];
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path cache = fs::temp_directory_path() / "ppet_pmx_package_cache";
    fs::remove_all(cache, ec);

    ppet::Logger::Init("");
    ppet::Logger::SetConsoleLevel(ppet::LogLevel::Off);
    std::printf("== PMX 模型包导入专项测试 ==\n");

    const Case cases[] = {
        {"pmx_full.zip", 2, 1, false, "完整包（pmx + 贴图同目录）"},
        {"pmx_no_tex.zip", 1, 0, true, "缺贴图（应报告缺失资源）"},
        {"pmx_case.zip", 2, 1, false, "贴图大小写不一致（大小写容错）"},
        {"pmx_subdir.zip", 2, 1, false, "贴图在子目录（按文件名兜底）"},
    };

    for (const Case& c : cases) {
        std::printf("-- %s (%s) --\n", c.label, c.file);
        const std::string zipPath = ppet::JoinPath(zipDir, c.file);
        Check(ppet::FileExists(zipPath), std::string(c.file) + "：测试包存在");

        ppet::ModelPackage package;
        std::string error;
        const bool extracted =
            ppet::ExtractModelPackage(zipPath, cache.string(), package, error);
        Check(extracted,
              std::string(c.file) + "：解压成功" + (extracted ? "" : "：" + error));
        if (!extracted) continue;

        Check(package.kind == ppet::PackageModelKind::Pmx,
              std::string(c.file) + "：识别为 PMX 包");
        Check(package.entryCount == c.expectedEntries,
              std::string(c.file) + "：条目数 " + std::to_string(package.entryCount));
        Check(package.textureCount == c.expectedTextures,
              std::string(c.file) + "：贴图数 " + std::to_string(package.textureCount));
        Check(!package.sourceArchive.empty() && !package.modelDirectory.empty() &&
                  !package.modelPath.empty(),
              std::string(c.file) + "：来源包/基准目录/模型路径均已记录");
        Check(ppet::FileExists(package.modelPath),
              std::string(c.file) + "：定位到的 .pmx 真实存在");

        ppet::pmx::PmxModel model;
        std::string loadError;
        const bool loaded = model.Load(package.modelPath, loadError);
        Check(loaded, std::string(c.file) + "：PmxModel::Load 成功" +
                          (loaded ? "" : "：" + loadError));
        if (!loaded) continue;

        const std::vector<std::string> missing = model.MissingResources();
        Check(missing.empty() != c.expectMissing,
              std::string(c.file) + "：缺失资源数 " + std::to_string(missing.size()) +
                  (c.expectMissing ? "（预期 >0）" : "（预期 0）"));

        const ppet::ModelDebugInfo info = model.DebugInfo();
        Check(info.formatVersion == "PMX 2.0",
              std::string(c.file) + "：格式版本 " + info.formatVersion);
        Check(info.textEncoding == "UTF-16LE",
              std::string(c.file) + "：文本编码 " + info.textEncoding);
        Check(info.vertexCount > 0 && info.triangleCount > 0,
              std::string(c.file) + "：诊断快照含网格规模");
        Check(info.type == ppet::ModelType::Pmx && info.loaded,
              std::string(c.file) + "：类型与加载状态");

        // 贴图路径确实被解析成了磁盘上的真实文件（大小写/目录容错的结果）。
        const auto& paths = model.TexturePaths();
        Check(!paths.empty(), std::string(c.file) + "：PMX 引用了贴图");
        if (!paths.empty() && !c.expectMissing) {
            Check(ppet::FileExists(paths[0]),
                  std::string(c.file) + "：贴图解析到真实文件 " + paths[0]);
        }
    }

    fs::remove_all(cache, ec);
    ppet::Logger::Shutdown();
    std::printf("== 结果：%s（失败 %d 项）==\n", g_failures == 0 ? "全部通过" : "存在失败",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
