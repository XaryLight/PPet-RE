// P0 安全边界回归测试（报告缺陷 2.1.1/2.1.2/2.1.5/2.6.4）。
// 直接构造恶意字节流，验证：
//   - ZIP 解压炸弹（压缩比超限）被拒绝，不 OOM；
//   - CRC 被篡改的条目被拒绝；
//   - EOCD 偏移回绕不会越界读；
//   - 非法类型 / 非法 UTF-8 的配置不会崩溃。
// 不依赖真实模型文件，三构建配置均可运行。
#include <src/tools/core/Config.hpp>
#include <src/tools/core/FileIO.hpp>
#include <src/tools/core/Log.hpp>
#include <src/tools/core/ZipArchive.hpp>

#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "zlib/zlib.h"

namespace fs = std::filesystem;

namespace {
int g_failures = 0;
void Check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "通过" : "失败", what);
    if (!ok) ++g_failures;
}

void PutU16(std::vector<unsigned char>& b, std::uint16_t v) {
    b.push_back(static_cast<unsigned char>(v & 0xFF));
    b.push_back(static_cast<unsigned char>((v >> 8) & 0xFF));
}
void PutU32(std::vector<unsigned char>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<unsigned char>((v >> (8 * i)) & 0xFF));
}

// 构造一个 ZIP：单条目，store 方式，原始内容 raw，声明的 CRC 为 crc。
// 用于测试 CRC 校验：真 CRC 应通过，篡改 CRC 应失败。
std::vector<unsigned char> MakeStoreZip(const std::string& name,
                                        const std::vector<unsigned char>& raw,
                                        std::uint32_t crc) {
    std::vector<unsigned char> z;
    const std::uint32_t localOffset = 0;
    // 本地文件头 PK\3\4
    PutU32(z, 0x04034b50);
    PutU16(z, 20);  // version needed
    PutU16(z, 0);   // flags
    PutU16(z, 0);   // method = store
    PutU16(z, 0); PutU16(z, 0);  // time/date
    PutU32(z, crc);
    PutU32(z, static_cast<std::uint32_t>(raw.size()));
    PutU32(z, static_cast<std::uint32_t>(raw.size()));
    PutU16(z, static_cast<std::uint16_t>(name.size()));
    PutU16(z, 0);  // extra len
    z.insert(z.end(), name.begin(), name.end());
    z.insert(z.end(), raw.begin(), raw.end());

    const std::uint32_t localSize = static_cast<std::uint32_t>(z.size());
    // 中央目录 PK\1\2
    PutU32(z, 0x02014b50);
    PutU16(z, 20); PutU16(z, 20);  // version made / needed
    PutU16(z, 0); PutU16(z, 0);    // flags / method
    PutU16(z, 0); PutU16(z, 0);
    PutU32(z, crc);
    PutU32(z, static_cast<std::uint32_t>(raw.size()));
    PutU32(z, static_cast<std::uint32_t>(raw.size()));
    PutU16(z, static_cast<std::uint16_t>(name.size()));
    PutU16(z, 0); PutU16(z, 0); PutU16(z, 0); PutU16(z, 0);
    PutU32(z, 0);
    PutU32(z, localOffset);
    z.insert(z.end(), name.begin(), name.end());
    const std::uint32_t dirSize = static_cast<std::uint32_t>(z.size() - localSize);

    // EOCD PK\5\6
    PutU32(z, 0x06054b50);
    PutU16(z, 0); PutU16(z, 0);
    PutU16(z, 1); PutU16(z, 1);
    PutU32(z, dirSize);
    PutU32(z, localSize);
    PutU16(z, 0);
    return z;
}

std::uint32_t Crc32Of(const std::vector<unsigned char>& data) {
    // 简易 CRC32（与 zlib 一致的多项式），避免测试依赖 zlib 内部符号。
    std::uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char c : data) {
        crc ^= c;
        for (int k = 0; k < 8; ++k) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
        }
    }
    return ~crc;
}

bool WriteBytes(const std::string& path, const std::vector<unsigned char>& data) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return true;
}

}  // namespace

int main() {
    ppet::Logger::Init("");
    const std::string tmp = (fs::temp_directory_path() / "ppet_p0_security").string();
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    std::printf("== P0 安全边界回归 ==\n");

    // --- 1. CRC 正确 -> 通过；CRC 篡改 -> 拒绝 ---
    {
        const std::vector<unsigned char> raw = { 'h', 'e', 'l', 'l', 'o' };
        const std::uint32_t goodCrc = Crc32Of(raw);
        const std::string goodZip = tmp + "/good.zip";
        Check(WriteBytes(goodZip, MakeStoreZip("a.txt", raw, goodCrc)), "写入 CRC 正确的 ZIP");
        std::vector<unsigned char> out;
        Check(ppet::ReadZipEntry(goodZip, "a.txt", out) && out == raw,
              "CRC 正确时正常解压");

        const std::string badZip = tmp + "/badcrc.zip";
        Check(WriteBytes(badZip, MakeStoreZip("a.txt", raw, goodCrc ^ 0xFFFFu)),
              "写入 CRC 被篡改的 ZIP");
        out.clear();
        Check(!ppet::ReadZipEntry(badZip, "a.txt", out),
              "CRC 被篡改时拒绝解压（缺陷 2.6.4）");
    }

    // --- 2. EOCD 偏移回绕 -> 拒绝，不越界 ---
    {
        std::vector<unsigned char> z(22, 0);
        PutU32(z, 0x06054b50);
        PutU16(z, 0); PutU16(z, 0);
        PutU16(z, 1); PutU16(z, 1);
        PutU32(z, 0xFFFFFFF0u);  // dirSize 极大
        PutU32(z, 0x20u);        // dirOffset 较小；32 位相加回绕
        PutU16(z, 0);
        const std::string wrapZip = tmp + "/wrap.zip";
        Check(WriteBytes(wrapZip, z), "写入 EOCD 回绕的 ZIP");
        std::vector<ppet::ZipEntry> entries;
        Check(!ppet::ListZipEntries(wrapZip, entries),
              "EOCD 32 位回绕被拒绝，不越界读（缺陷 2.1.2）");
    }

    // --- 2b. ZIP 炸弹：声明很小、实际解压巨大 -> 拒绝，不 OOM（缺陷 2.1.1） ---
    {
        // 用 zlib 生成裸 deflate 流：把 8MB 的 0 压缩到几十 KB。
        std::vector<unsigned char> huge(8u * 1024u * 1024u, 0);
        z_stream def{};
        deflateInit2(&def, 9, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
        std::vector<unsigned char> compressed(compressBound(static_cast<uLong>(huge.size())));
        def.next_in = huge.data();
        def.avail_in = static_cast<uInt>(huge.size());
        def.next_out = compressed.data();
        def.avail_out = static_cast<uInt>(compressed.size());
        deflate(&def, Z_FINISH);
        const std::size_t compSize = compressed.size() - def.avail_out;
        deflateEnd(&def);
        compressed.resize(compSize);

        // 构造 ZIP：中央目录把 uncompressedSize 声明为 100（远小于实际 8MB）。
        std::vector<unsigned char> z;
        const std::string name = "bomb.bin";
        PutU32(z, 0x04034b50);  // local header
        PutU16(z, 20); PutU16(z, 0); PutU16(z, 8);  // method = deflate
        PutU16(z, 0); PutU16(z, 0);
        PutU32(z, 0);            // crc 占位（炸弹会在校验前被拒）
        PutU32(z, 100);          // 声明压缩后 100
        PutU32(z, 100);          // 声明解压后 100（关键：远小于真实）
        PutU16(z, static_cast<std::uint16_t>(name.size())); PutU16(z, 0);
        z.insert(z.end(), name.begin(), name.end());
        z.insert(z.end(), compressed.begin(), compressed.end());
        const std::uint32_t localSize = static_cast<std::uint32_t>(z.size());
        PutU32(z, 0x02014b50);   // central directory
        PutU16(z, 20); PutU16(z, 20); PutU16(z, 0); PutU16(z, 8);
        PutU16(z, 0); PutU16(z, 0);
        PutU32(z, 0);
        PutU32(z, static_cast<std::uint32_t>(compSize));  // 真实压缩大小
        PutU32(z, 100);                                   // 声明解压 100
        PutU16(z, static_cast<std::uint16_t>(name.size()));
        PutU16(z, 0); PutU16(z, 0); PutU16(z, 0); PutU16(z, 0);
        PutU32(z, 0); PutU32(z, 0);
        z.insert(z.end(), name.begin(), name.end());
        const std::uint32_t dirSize = static_cast<std::uint32_t>(z.size() - localSize);
        PutU32(z, 0x06054b50);   // EOCD
        PutU16(z, 0); PutU16(z, 0); PutU16(z, 1); PutU16(z, 1);
        PutU32(z, dirSize); PutU32(z, localSize); PutU16(z, 0);

        const std::string bombZip = tmp + "/bomb.zip";
        Check(WriteBytes(bombZip, z), "写入压缩比异常的 ZIP 炸弹");
        std::vector<unsigned char> out;
        Check(!ppet::ReadZipEntry(bombZip, "bomb.bin", out),
              "解压炸弹（声明 100 / 实际 8MB）被预算拒绝（缺陷 2.1.1）");
        Check(out.size() < 1024u, "炸弹未撑爆输出缓冲区");
    }

    // --- 3. 配置：超范围整数 -> 不崩溃，回退默认值（缺陷 2.1.5） ---
    {
        const std::string cfg = tmp + "/config.json";
        // windowX 是超出 int64 的整数：nlohmann 会解析成 number_float，
        // get_int 的 is_number_integer() 判断会跳过它，保留默认窗口宽度。
        const std::string badJson =
            "{\"windowX\": 99999999999999999999, \"renderScale\": 1.0, "
            "\"windowWidth\": 123}";
        {
            std::ofstream f(cfg, std::ios::binary);
            f.write(badJson.data(), static_cast<std::streamsize>(badJson.size()));
        }
        ppet::ConfigStore store;
        std::string error;
        const bool loaded = store.Load(cfg, error);
        Check(loaded, "超范围整数的配置仍可加载，不崩溃（缺陷 2.1.5）");
        Check(store.config().windowWidth == 123, "合法字段仍被正确读取");
    }

    // --- 4. 配置：含非法 UTF-8 字节的字段序列化 -> 不崩溃（缺陷 2.1.5） ---
    {
        ppet::ConfigStore store;
        std::string error;
        // 模拟 ModelNaming 按纯字节保留下来的非法编码显示名。
        store.config().modelNames.push_back(std::string("bad\xFF\xFE", 5));
        store.config().modelList.push_back(std::string("path\xFF", 5));
        const std::string saved = tmp + "/saved.json";
        const bool ok = store.Save(saved, error);
        Check(ok, "含非法编码字段的配置可安全序列化落盘（缺陷 2.1.5）");
        if (ok) {
            std::string text;
            Check(ppet::ReadFileText(saved, text, nullptr), "落盘的配置可被重新读取");
        }
    }

    fs::remove_all(tmp);
    ppet::Logger::Shutdown();
    std::printf("== %s ==\n", g_failures == 0 ? "全部通过" : "存在失败项");
    return g_failures == 0 ? 0 : 1;
}
