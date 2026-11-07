// PPet - C++ 移植版
// 模型包（.zip）导入：把 zip 解压到缓存目录，并定位其中的模型文件。
//
// 【为什么 PMX 强烈推荐用 .zip 导入】
//   PMX 文件本身只存「贴图的文件名/相对路径」，并不内嵌任何图形数据。
//   一个能正常显示的 MMD 模型通常是这样的目录：
//       model.pmx
//       tex/body.png, tex/face.png, ...   ← 贴图
//       toon/, sph/, vmd/ ...
//   单独把 model.pmx 拖进程序，程序只能读到一堆指向不存在文件的路径，
//   结果就是「模型能加载但全身纯色/白模」。
//   因此本项目把 .zip 作为 PMX 的一等公民导入方式：整包解压后保留原目录结构，
//   PMX 里的相对路径才能原样解析到贴图。
//
// 支持的模型包形态（社区里常见的打包方式）：
//   - PMX：xxx.pmx + tex/*.png|tga|... （推荐，见上）
//   - Live2D：xxx.model3.json + xxx.moc3 + textures/*.png
//     （注意：Cubism 2 的老格式 xxx.moc + xxx.model.json 本项目暂不支持）
//   - VRM：xxx.vrm + *.png 贴图（.vrm 通常是单文件，zip 内一般还有贴图）
#ifndef PPET_CORE_MODELPACKAGE_HPP
#define PPET_CORE_MODELPACKAGE_HPP

#include <cstddef>
#include <string>
#include <vector>

namespace ppet {

/// 包内定位到的模型种类（core 层自用，不依赖 model/Model.hpp 的 ModelType）。
enum class PackageModelKind {
    Unknown,
    Live2D,
    Pmx,
    Vrm,
};

/// 可读名称（"PMX" / "VRM" / "Live2D" / "未知"），用于日志与调试面板。
const char* PackageModelKindName(PackageModelKind kind);

/// 判断归档内的条目名是不是模型贴图（按扩展名，大小写不敏感）。
/// 覆盖 MMD / VRM / Live2D 常见格式：png jpg jpeg tga bmp dds webp gif tif tiff sph spa。
bool IsModelTextureFile(const std::string& name);

/// 模型包解包结果。
struct ModelPackage {
    /// 解压后的缓存目录。
    std::string directory;
    /// 定位到的模型主文件路径（.model3.json / .pmx / .vrm）。
    std::string modelPath;
    /// 定位到的模型主文件名（不含路径）。
    std::string modelFileName;
    /// 模型主文件所在目录 —— PMX 贴图相对路径的基准。
    std::string modelDirectory;
    /// 原始 zip 路径（用于日志与「来源」显示）。
    std::string sourceArchive;
    /// 定位到的模型种类。
    PackageModelKind kind = PackageModelKind::Unknown;
    /// 归档内的文件条目数（不含目录条目）。
    std::size_t entryCount = 0;
    /// 归档内的贴图文件数。PMX 包为 0 时几乎必然是白模。
    std::size_t textureCount = 0;
    /// 解压后的总字节数。
    std::size_t totalBytes = 0;
};

/// 解压模型包并定位其中的模型文件，同时统计包内容。
///
/// @param zipPath   zip 文件路径。
/// @param cacheRoot 缓存根目录（解压目录会建在它下面）。
/// @param out       解包结果（含统计信息）。
/// @param error     失败原因。
/// @return 成功返回 true。
bool ExtractModelPackage(const std::string& zipPath, const std::string& cacheRoot,
                         ModelPackage& out, std::string& error);

/// 在一个已解压的目录里查找模型主文件。
///
/// 多个 PMX 候选时，优先选「同目录（含子目录）贴图最多」的那个 —— 社区包里
/// 常同时附带多个版本的 .pmx，只有贴图齐全的那个能正常渲染。
///
/// @param directory 目录。
/// @param out       找到的模型文件路径（未找到时为空）。
/// @return 找到返回 true。
bool FindModelInDirectory(const std::string& directory, std::string& out);

}  // namespace ppet

#endif  // PPET_CORE_MODELPACKAGE_HPP
