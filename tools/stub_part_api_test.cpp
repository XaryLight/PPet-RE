// PPet - C++ 移植版
// stub 配置下的 Part API 可测性回归（报告 2.2.7）：
//   验证 PartCount / FindPart / SetPartOpacity / ResetPartOpacities 在 stub
//   后端不再 no-op，且 part 不透明度会按真实 Core 语义传播到 drawable。
//
// 之前 stub 不支持 part 数据，PartOpacity 这一关键修复（互斥身体透明度）
// 无法被单测覆盖，削弱了三配置矩阵的意义。本测试程序化生成一个最小 stub moc
// （含两个 drawable），走完整 adapter API，断言 part 写入可被 drawable 观测。
//
// 仅在 PPET_WITH_CUBISM_STUB 配置下执行；真实 SDK / 无后端配置跳过（返回 0）。
#include <api/live2d/CubismCore.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool cond, const char* msg) {
    if (!cond) {
        std::printf("[FAIL] %s\n", msg);
        ++g_failures;
    }
}

bool Near(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) <= eps;
}

// --- 小端写入辅助 ---
void PutU16(std::vector<unsigned char>& b, std::uint16_t v) {
    b.push_back(static_cast<unsigned char>(v & 0xFF));
    b.push_back(static_cast<unsigned char>((v >> 8) & 0xFF));
}
void PutU32(std::vector<unsigned char>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<unsigned char>((v >> (8 * i)) & 0xFF));
}
void PutI32(std::vector<unsigned char>& b, std::int32_t v) {
    PutU32(b, static_cast<std::uint32_t>(v));
}
void PutF32(std::vector<unsigned char>& b, float v) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    PutU32(b, bits);
}
void PutStr(std::vector<unsigned char>& b, const std::string& s) {
    for (char c : s) b.push_back(static_cast<unsigned char>(c));
}

// 生成一个最小 stub moc：0 参数 + 2 个 drawable（各 1 三角形、不透明度 1.0）。
std::vector<unsigned char> MakeMinimalMoc() {
    std::vector<unsigned char> b;
    const char magic[8] = {'P', 'P', 'E', 'T', 'M', 'O', 'C', '1'};
    for (char c : magic) b.push_back(static_cast<unsigned char>(c));
    PutU32(b, 1);  // version
    // payloadSize 稍后回填。
    std::size_t payloadPos = b.size();
    PutU32(b, 0);
    PutU32(b, 0);  // paramCount
    PutU32(b, 2);  // drawableCount
    PutF32(b, 1024.0f);  // canvasWidth
    PutF32(b, 1024.0f);  // canvasHeight
    PutF32(b, 512.0f);   // originX
    PutF32(b, 512.0f);   // originY
    PutF32(b, 1.0f);     // pixelsPerUnit

    const char* ids[2] = {"BodyA", "BodyB"};
    for (int d = 0; d < 2; ++d) {
        const std::string id = ids[d];
        PutU16(b, static_cast<std::uint16_t>(id.size()));
        PutStr(b, id);
        PutI32(b, 0);    // textureIndex
        PutF32(b, 1.0f); // opacity（drawable 基准值）
        PutU32(b, 3);    // vertexCount
        PutU32(b, 3);    // indexCount
        // 3 个顶点（x,y）
        for (int v = 0; v < 3; ++v) {
            PutF32(b, static_cast<float>(v));
            PutF32(b, static_cast<float>(v));
        }
        // 3 个 UV
        for (int v = 0; v < 3; ++v) {
            PutF32(b, static_cast<float>(v) * 0.5f);
            PutF32(b, static_cast<float>(v) * 0.5f);
        }
        // 3 个 u16 索引
        for (int i = 0; i < 3; ++i) PutU16(b, static_cast<std::uint16_t>(i));
    }

    const std::uint32_t payload = static_cast<std::uint32_t>(b.size());
    for (int i = 0; i < 4; ++i) {
        b[payloadPos + i] = static_cast<unsigned char>((payload >> (8 * i)) & 0xFF);
    }
    return b;
}

}  // namespace

int main() {
#if !defined(PPET_WITH_CUBISM_STUB)
    std::printf("[SKIP] stub Part API 测试仅在 PPET_WITH_CUBISM_STUB 配置下运行\n");
    return 0;
#else
    using namespace ppet::live2d;
    using namespace ppet::live2d::cubism;

    const auto tmp = std::filesystem::temp_directory_path() / "ppet_stub_part_api_test";
    std::filesystem::create_directories(tmp);
    const auto mocPath = tmp / "minimal.moc";
    {
        const auto bytes = MakeMinimalMoc();
        std::ofstream f(mocPath, std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }

    Model* model = CreateModel();
    Check(model != nullptr, "CreateModel 成功");
    std::string error;
    const bool opened = OpenModel(model, mocPath.string(), error);
    Check(opened, ("OpenModel 成功: " + error).c_str());
    if (!opened) {
        DestroyModel(model);
        std::filesystem::remove_all(tmp);
        return g_failures == 0 ? 0 : 1;
    }

    // --- PartCount / FindPart ---
    const int parts = PartCount(model);
    Check(parts == 2, "stub 下 PartCount 应返回 drawable 数（2）");
    Check(FindPart(model, "BodyA") == 0, "FindPart(BodyA) == 0");
    Check(FindPart(model, "BodyB") == 1, "FindPart(BodyB) == 1");
    Check(FindPart(model, "NoSuch") == -1, "FindPart 未知返回 -1");

    // --- SetPartOpacity 在 stub 下真实生效并经 Update 传播到 drawable ---
    SetPartOpacity(model, 0, 0.25f);
    UpdateModel(model);
    Drawable d0, d1;
    Check(GetDrawable(model, 0, d0), "GetDrawable(0) 成功");
    Check(GetDrawable(model, 1, d1), "GetDrawable(1) 成功");
    Check(Near(d0.opacity, 0.25f), "part0=0.25 应传播到 drawable0 不透明度");
    Check(Near(d1.opacity, 1.0f), "未改动的 part1 保持 1.0");

    // SetPartOpacity 钳制到 [0,1]
    SetPartOpacity(model, 0, 5.0f);
    UpdateModel(model);
    Check(GetDrawable(model, 0, d0) && Near(d0.opacity, 1.0f),
          "part 不透明度应钳制到上界 1.0");
    SetPartOpacity(model, 0, -3.0f);
    UpdateModel(model);
    Check(GetDrawable(model, 0, d0) && Near(d0.opacity, 0.0f),
          "part 不透明度应钳制到下界 0.0");

    // --- ResetPartOpacities 恢复 ---
    ResetPartOpacities(model);
    UpdateModel(model);
    Check(GetDrawable(model, 0, d0) && Near(d0.opacity, 1.0f),
          "ResetPartOpacities 应恢复 drawable0 不透明度为 1.0");

    DestroyModel(model);
    std::filesystem::remove_all(tmp);

    if (g_failures == 0) {
        std::printf("[PASS] stub Part API 完全可测：Count/Find/Set/Reset + drawable 传播\n");
        return 0;
    }
    std::printf("[FAIL] %d 项失败\n", g_failures);
    return 1;
#endif
}
