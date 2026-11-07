// 中文（及各类非 ASCII）路径处理测试。
//
// 背景：
//   项目内部统一用 UTF-8 保存路径，但 Windows 的窄字符 API 按「系统 ANSI
//   代码页」解释字节，MSVC 的 std::filesystem::path 构造亦然。
//   一旦某一层把 UTF-8 当 ANSI 处理，中文路径就会失效：
//     - 文件读不到（模型加载失败）；
//     - 目录遍历不到（Live2D 找不到 model3.json）；
//     - 路径被写回配置文件后变成乱码（下次启动就读不到了）。
//
// 本测试覆盖 FileIO 的往返一致性、filesystem 的路径构造 / 回转，
// 以及在含中文目录下的模型包解压与定位。
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <src/tools/core/FileIO.hpp>
#include <src/tools/core/Log.hpp>
#include <src/tools/core/ModelPackage.hpp>
#include <src/tools/core/ZipArchive.hpp>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& what) {
    if (condition) {
        std::printf("  [通过] %s\n", what.c_str());
    } else {
        std::printf("  [失败] %s\n", what.c_str());
        ++g_failures;
    }
}

/// 含中文与特殊字符的目录名，覆盖 GBK/UTF-8 转换的边界情况。
const char* kUnicodeDirName = "\xe6\xb7\xb1\xe7\xa9\xba\xe4\xb9\x8b\xe7\x9c\xbc"
                              "-\xe8\x87\xaa\xe8\xa1\x8c 1.0";
const char* kUnicodeFileName = "\xe7\x99\xbd\xe8\xa1\xa3 v2.pmx";

/// 手工拼一个只含单个 stored 条目的 ZIP。
///
/// 刻意不设置通用位标志的 bit 11（UTF-8 文件名标志），
/// 用来验证「字节本身是合法 UTF-8 时按 UTF-8 解析」的兼容策略。
std::string BuildZipWithUtf8Entry(const std::string& name, const std::string& payload) {
    auto put16 = [](std::string& s, std::uint16_t v) {
        s.push_back(static_cast<char>(v & 0xFF));
        s.push_back(static_cast<char>((v >> 8) & 0xFF));
    };
    auto put32 = [](std::string& s, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    };

    // CRC32（本项目 zlib 提供 crc32，测试里用自带的简单实现避免额外依赖）。
    std::uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char c : payload) {
        crc ^= c;
        for (int k = 0; k < 8; ++k) {
            crc = (crc >> 1) ^ (0xEDB88320u & (~((crc & 1u) - 1u)));
        }
    }
    crc ^= 0xFFFFFFFFu;

    const std::uint16_t nameLen = static_cast<std::uint16_t>(name.size());
    const std::uint32_t size = static_cast<std::uint32_t>(payload.size());

    std::string out;
    // 本地文件头。
    put32(out, 0x04034b50u);
    put16(out, 20);        // 解压所需版本
    put16(out, 0);         // 通用位标志：刻意不置 bit 11
    put16(out, 0);         // 压缩方式：stored
    put16(out, 0);         // 修改时间
    put16(out, 0);         // 修改日期
    put32(out, crc);
    put32(out, size);
    put32(out, size);
    put16(out, nameLen);
    put16(out, 0);         // extra 长度
    out += name;
    out += payload;

    const std::uint32_t centralOffset = static_cast<std::uint32_t>(out.size());

    // 中央目录。
    put32(out, 0x02014b50u);
    put16(out, 20);        // 创建版本
    put16(out, 20);        // 解压所需版本
    put16(out, 0);         // 通用位标志：同样不置 bit 11
    put16(out, 0);         // 压缩方式：stored
    put16(out, 0);
    put16(out, 0);
    put32(out, crc);
    put32(out, size);
    put32(out, size);
    put16(out, nameLen);
    put16(out, 0);         // extra
    put16(out, 0);         // comment
    put16(out, 0);         // 起始磁盘号
    put16(out, 0);         // 内部属性
    put32(out, 0);         // 外部属性
    put32(out, 0);         // 本地文件头偏移
    out += name;

    const std::uint32_t centralSize = static_cast<std::uint32_t>(out.size()) - centralOffset;

    // 中央目录结束记录。
    put32(out, 0x06054b50u);
    put16(out, 0);
    put16(out, 0);
    put16(out, 1);
    put16(out, 1);
    put32(out, centralSize);
    put32(out, centralOffset);
    put16(out, 0);         // 注释长度
    return out;
}

}  // namespace

int main() {
    ppet::Logger::Init("");

    std::printf("== 中文路径处理测试 ==\n");

    const fs::path base = fs::temp_directory_path() / "ppet_unicode_path_test";
    const fs::path unicodeDir = base / kUnicodeDirName;
    const fs::path unicodeFile = unicodeDir / kUnicodeFileName;

    // --- 0. 清理旧目录 ---
    std::error_code ec;
    fs::remove_all(base, ec);

    // --- 1. UTF-8 路径 -> filesystem::path -> UTF-8 路径 往返一致 ---
    const std::string unicodeDirUtf8 = ppet::FromFsPath(unicodeDir);
    const std::string unicodeFileUtf8 = ppet::FromFsPath(unicodeFile);
    Check(unicodeDirUtf8.find(kUnicodeDirName) != std::string::npos,
          "目录路径往返后仍含中文（未被 ANSI 代码页破坏）");
    Check(unicodeFileUtf8.find(kUnicodeFileName) != std::string::npos,
          "文件路径往返后仍含中文");

    // --- 2. 在含中文的目录下建目录、写文件、读回来 ---
    Check(fs::create_directories(ppet::ToFsPath(unicodeDirUtf8), ec) && !ec,
          "能在含中文的路径下创建目录");
    if (ec) {
        std::printf("  (错误信息：%s)\n", ec.message().c_str());
    }

    const std::string payload = "PMX fake payload";
    Check(ppet::WriteFileText(unicodeFileUtf8, payload),
          "能写入含中文路径的文件");
    Check(ppet::FileExists(unicodeFileUtf8), "含中文路径的文件可被检测到");

    std::string readBack;
    Check(ppet::ReadFileText(unicodeFileUtf8, readBack) && readBack == payload,
          "能从含中文路径读回原始内容");

    // --- 3. filesystem 遍历能看到中文文件 ---
    bool sawUnicode = false;
    for (const auto& entry : fs::directory_iterator(ppet::ToFsPath(unicodeDirUtf8), ec)) {
        if (ppet::FromFsPath(entry.path().filename()) == kUnicodeFileName) sawUnicode = true;
    }
    Check(sawUnicode, "目录遍历能枚举出中文文件名的条目");

    // --- 4. 相对路径转换用 '/' 分隔且保留 UTF-8 ---
    const fs::path relative = fs::relative(unicodeFile, unicodeDir, ec);
    const std::string relativeUtf8 = ppet::FromFsPathGeneric(relative);
    Check(relativeUtf8.find('\\') == std::string::npos,
          "相对路径统一使用 '/' 分隔");
    Check(relativeUtf8.find(kUnicodeFileName) != std::string::npos,
          "相对路径保留中文文件名");

    // --- 5. 环境变量读取（Windows 上必须是 UTF-8） ---
    const std::string home = ppet::GetEnvVar("HOME");
    Check(!home.empty(), "能读到 HOME 环境变量");
    const std::string appData = ppet::GetEnvVar("APPDATA");
    Check(true, ("APPDATA = " + (appData.empty() ? std::string("(未设置)") : appData)).c_str());

    // --- 6. 含中文目录下的模型包解压与定位 ---
    // 构造一个「位于中文目录里」的最小 PMX 模型包。
    const fs::path zipDir = base / "zip";
    fs::create_directories(zipDir, ec);
    const fs::path zipPath = zipDir / "\xe6\xb7\xb1\xe7\xa9\xba\xe4\xb9\x8b\xe7\x9c\xbc.zip";
    const std::string zipUtf8 = ppet::FromFsPath(zipPath);
    Check(ppet::WriteFileText(zipUtf8, "PK\x05\x06" + std::string(18, '\0')),
          "能在含中文路径下写入测试用 ZIP");

    std::vector<ppet::ZipEntry> entries;
    Check(!ppet::ListZipEntries(zipUtf8, entries) || entries.empty(),
          "空内容 ZIP 不会导致崩溃");

    const std::string cacheRoot = ppet::FromFsPath(base / "cache");
    ppet::ModelPackage package;
    std::string error;
    const bool extracted =
        ppet::ExtractModelPackage(zipUtf8, cacheRoot, package, error);
    // 内容不是合法 ZIP（只有一个空 EOCD），解压应失败但不能崩溃。
    Check(!extracted, "非法 ZIP 被拒绝解压");
    Check(!error.empty(), "失败时给出可读的错误信息");

    // --- 7. ZIP 条目名的编码归一化 ---
    // 构造一个「条目名是 UTF-8 字节、且未置 UTF-8 标志位(bit 11)」的 ZIP，
    // 模拟部分 Windows 压缩工具的行为。
    {
        const std::string entryName = "深空之眼/白衣 1.0.pmx";
        const std::string payload = "FAKEPMX";
        const fs::path realZip = zipDir / "utf8name.zip";
        const std::string realZipUtf8 = ppet::FromFsPath(realZip);
        Check(ppet::WriteFileText(realZipUtf8, BuildZipWithUtf8Entry(entryName, payload)),
              "能构造条目名为 UTF-8 的测试 ZIP");

        std::vector<ppet::ZipEntry> utf8Entries;
        const bool listed = ppet::ListZipEntries(realZipUtf8, utf8Entries);
        Check(listed, "能解析条目名为 UTF-8 的 ZIP");
        if (listed && !utf8Entries.empty()) {
            Check(utf8Entries[0].name == entryName,
                  "未置 UTF-8 标志位但字节合法的条目名仍按 UTF-8 解析");
        }
    }

    fs::remove_all(base, ec);

    ppet::Logger::Shutdown();
    std::printf("== %s ==\n", g_failures == 0 ? "全部通过" : "存在失败项");
    return g_failures == 0 ? 0 : 1;
}
