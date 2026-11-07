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
#include <filesystem>
#include <string>
#include <vector>

namespace ppet {

/// 把内部使用的 UTF-8 路径转换成 std::filesystem::path。
///
/// 为什么需要它：
///   MSVC 的 std::filesystem::path 从窄字符（std::string）构造时，按「当前
///   ANSI 代码页」解释字节，而不是按 UTF-8。中文系统（代码页 936）上，
///   一个 UTF-8 的中文路径会被误解成乱码，导致目录遍历、建目录、删目录
///   全部失败——但同一个路径用 FileIO 的宽字符 API 却能正常打开。
///
///   MinGW 的 libstdc++ 用 codecvt_utf8_utf16 做转换，行为正确；
///   但为了让 MSVC / MinGW 表现一致，这里统一显式转换：
///   Windows 上先 UTF-8 -> UTF-16 再构造 path，其它平台直接用窄字符。
///
/// @param utf8Path 以 UTF-8 编码的路径。
/// @return 可直接交给 std::filesystem 使用的 path 对象。
std::filesystem::path ToFsPath(const std::string& utf8Path);

/// 把 std::filesystem::path 转换回内部的 UTF-8 字符串。
///
/// 与 ToFsPath 成对使用。Windows 上不能直接用 path::string()：
///   它按 ANSI 代码页把宽字符转回窄字符，中文会变成乱码，
///   导致「能读到文件、但再存回去路径就坏了」这类隐蔽故障。
///
/// @param path  待转换的 path 对象。
/// @return UTF-8 编码的路径字符串。
std::string FromFsPath(const std::filesystem::path& path);

/// 把 path 转换成 UTF-8 字符串，并统一使用 '/' 作为分隔符。
///
/// 用于「相对路径」这类需要跨平台一致分隔符的场景
/// （例如 ZIP 包内的条目名、目录相对路径）。
///
/// @param path  待转换的 path 对象。
/// @return UTF-8 编码、以 '/' 分隔的路径字符串。
std::string FromFsPathGeneric(const std::filesystem::path& path);

/// 把系统 ANSI 代码页编码的字节解码成 UTF-8。
///
/// 用于解码那些「按本地代码页存储、却没有标记编码」的外部数据，
/// 例如 ZIP 里未置 UTF-8 标志位的中文文件名。
///
/// 非 Windows 平台没有 ANSI 代码页概念，直接按字节透传（假定已是 UTF-8）。
///
/// @param ansi  ANSI 代码页编码的字节序列。
/// @return UTF-8 编码的字符串；无法解码时返回空串。
std::string DecodeAnsiToUtf8(const std::string& ansi);

/// 读取环境变量，返回 UTF-8 编码的值（未设置时返回空串）。
///
/// Windows 上刻意使用宽字符 API：std::getenv 返回的是「系统 ANSI 代码页」
/// 编码的字节（例如中文系统上是 GBK），而本项目内部统一用 UTF-8。
/// 若直接 getenv 再当 UTF-8 用，用户目录含中文时（如 C:\Users\张三\AppData）
/// 路径会变成乱码。
///
/// @param name  环境变量名（ASCII）。
/// @return 环境变量的 UTF-8 值；未设置或为空时返回空串。
std::string GetEnvVar(const char* name);

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
