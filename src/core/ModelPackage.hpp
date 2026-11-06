// PPet - C++ 移植版
// 模型包（zip）导入：把 zip 解压到缓存目录，并定位其中的模型文件。
//
// 支持的模型包形态（社区里常见的打包方式）：
//   - Live2D：xxx.model3.json + xxx.moc3 + textures/*.png
//     （注意：Cubism 2 的老格式 xxx.moc + xxx.model.json 本项目暂不支持）
//   - MMD：xxx.pmx + *.png / *.tga 贴图
//   - VRM：xxx.vrm + *.png 贴图（.vrm 通常是单文件，zip 内一般还有贴图）
#ifndef PPET_CORE_MODELPACKAGE_HPP
#define PPET_CORE_MODELPACKAGE_HPP

#include <string>
#include <vector>

namespace ppet {

/// 模型包解包结果。
struct ModelPackage {
    /// 解压后的缓存目录。
    std::string directory;
    /// 定位到的模型主文件路径（.model3.json / .pmx / .vrm）。
    std::string modelPath;
    /// 定位到的模型类型对应的主文件名（不含路径）。
    std::string modelFileName;
};

/// 解压模型包并定位其中的模型文件。
///
/// @param zipPath   zip 文件路径。
/// @param cacheRoot 缓存根目录（解压目录会建在它下面）。
/// @param out       解包结果。
/// @param error     失败原因。
/// @return 成功返回 true。
bool ExtractModelPackage(const std::string& zipPath, const std::string& cacheRoot,
                         ModelPackage& out, std::string& error);

/// 在一个已解压的目录里查找模型主文件。
///
/// @param directory 目录。
/// @param out       找到的模型文件路径（未找到时为空）。
/// @return 找到返回 true。
bool FindModelInDirectory(const std::string& directory, std::string& out);

}  // namespace ppet

#endif  // PPET_CORE_MODELPACKAGE_HPP
