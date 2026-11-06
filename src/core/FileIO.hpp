// PPet - C++ 移植版
// 跨平台文件读写辅助。
//
// 为什么需要它：
//   Windows 上 `std::ifstream(const std::string&)` 走的是 ANSI 代码页，
//   当路径里含中文（例如 "H:/Downloads/深空之眼…/白衣1.0.pmx"）时会直接
//   打不开文件。而本项目内部统一用 UTF-8 保存路径（std::string），
//   所以 Windows 分支必须先把 UTF-8 转成 UTF-16（wchar_t）再调用宽字符 API。
//
// 其它平台（Linux/macOS）的文件名本身就是字节串，直接用窄字符 API 即可。
#ifndef PPET_CORE_FILEIO_HPP
#define PPET_CORE_FILEIO_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace ppet {

/// 读取整个文件为字节序列。
///
/// @param path  文件路径（内部按 UTF-8 解释；Windows 下会自动转 UTF-16）。
/// @param out   读到的内容（成功时覆盖，失败时保持不变）。
/// @return 成功返回 true；失败返回 false（文件不存在、无权限、被占用等）。
bool ReadFileBytes(const std::string& path, std::vector<std::uint8_t>& out);

/// 读取整个文件为文本（不做换行转换，按原始字节返回）。
bool ReadFileText(const std::string& path, std::string& out);

/// 写文本到文件（覆盖写）。用于日志与配置落盘。
bool WriteFileText(const std::string& path, const std::string& text);

/// 写二进制数据到文件（覆盖写）。用于解压归档等场景。
bool WriteFileBytes(const std::string& path, const std::vector<std::uint8_t>& data);

/// 文件是否存在。
bool FileExists(const std::string& path);

/// 取路径所在目录（不含尾部斜杠）。无目录部分时返回空串。
std::string ParentDirectory(const std::string& path);

/// 路径的扩展名（小写，不含点）。例如 "a/b.PMX" -> "pmx"。
std::string FileExtension(const std::string& path);

/// 路径的文件名部分（不含目录）。
std::string FileName(const std::string& path);

/// 路径拼接（自动处理分隔符）。
std::string JoinPath(const std::string& dir, const std::string& name);

}  // namespace ppet

#endif  // PPET_CORE_FILEIO_HPP
