// PPet - C++ 移植版
// ZIP 归档读取（用于导入打包好的模型）。
//
// 为什么需要它：
//   Live2D / MMD 模型在社区里通常以 .zip 形式分发（一个 zip 里含
//   model3.json + moc3 + textures/，或 pmx + 贴图）。
//   直接让用户逐个文件导入体验很差，本模块把 zip 解压到缓存目录，
//   再由调用方定位其中的模型文件。
//
// 实现说明：
//   ZIP 的中央目录（central directory）位于文件尾部，解析它就能拿到
//   每个条目的名称、压缩方式与原始数据位置；数据本身用 zlib 的
//   raw inflate（-MAX_WBITS）解压。zlib 源码已随仓库提供（third_party/zlib），
//   因此本功能不依赖系统安装的 zlib，Windows 上开箱即用。
#ifndef PPET_CORE_ZIPARCHIVE_HPP
#define PPET_CORE_ZIPARCHIVE_HPP

#include <string>
#include <vector>

namespace ppet {

/// ZIP 归档中的一个文件条目。
struct ZipEntry {
    /// 归档内的完整路径（统一用 '/' 分隔，例如 "model/texture_00.png"）。
    std::string name;
    /// 解压后的原始大小（字节）。
    std::size_t size = 0;
};

/// 读取 ZIP 归档的文件列表（不含目录条目）。
///
/// @param zipPath  zip 文件路径（内部按 UTF-8 解释）。
/// @param entries  输出的条目列表。
/// @return 成功返回 true；文件不存在或格式非法返回 false。
bool ListZipEntries(const std::string& zipPath, std::vector<ZipEntry>& entries);

/// 把 ZIP 归档中的单个条目读进内存。
///
/// @param zipPath  zip 文件路径。
/// @param name     条目名（需与 ListZipEntries 返回的一致）。
/// @param out      解压后的数据。
/// @return 成功返回 true。
bool ReadZipEntry(const std::string& zipPath, const std::string& name,
                  std::vector<unsigned char>& out);

/// 把 ZIP 归档完整解压到目标目录。
///
/// 会自动创建子目录；条目名中的 ".." 会被拒绝（防止路径穿越）。
///
/// @param zipPath  zip 文件路径。
/// @param destDir  目标目录（不存在会自动创建）。
/// @param error    失败原因。
/// @return 成功返回 true。
bool ExtractZipToDirectory(const std::string& zipPath, const std::string& destDir,
                            std::string& error);

}  // namespace ppet

#endif  // PPET_CORE_ZIPARCHIVE_HPP
