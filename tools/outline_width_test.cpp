// OutlineWidth.hpp 纯逻辑单测：VRM 0.x / 1.0 描边宽度单位换算。
#include <src/tools/model/vrm/OutlineWidth.hpp>

#include <cmath>
#include <cstdio>

using namespace ppet::vrm;

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool cond, const char* what) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::printf("  FAIL: %s\n", what);
    }
}

bool Near(float a, float b) { return std::fabs(a - b) < 1e-6f; }

// 1. VRM 0.x world 模式：0.08 面板值 -> 0.008 米（关键修复）。
void TestVrm0World() {
    std::printf("-- VRM 0.x world 描边 *0.1 --\n");
    Check(Near(ResolveOutlineWidthMeters(0.08f, "worldCoordinates", true), 0.008f),
          "0.x world 0.08 -> 0.008m");
    Check(Near(ResolveOutlineWidthMeters(0.076f, "worldCoordinates", true), 0.0076f),
          "0.x world 0.076 -> 0.0076m");
    Check(Near(ResolveOutlineWidthMeters(0.0677f, "worldCoordinates", true), 0.00677f),
          "0.x world 0.0677 -> 0.00677m");
}

// 2. VRM 1.0 world 模式：原样返回米，不受影响（保护既有契约）。
void TestVrm1World() {
    std::printf("-- VRM 1.0 world 描边直接用米 --\n");
    Check(Near(ResolveOutlineWidthMeters(0.02f, "worldCoordinates", false), 0.02f),
          "1.0 world 0.02 -> 0.02m");
    Check(Near(ResolveOutlineWidthMeters(0.08f, "worldCoordinates", false), 0.08f),
          "1.0 world 不乘 0.1");
}

// 3. screen 模式：解析层保留原始比例（VrmModel 按模型高度折算）。
void TestScreen() {
    std::printf("-- screen 模式保留原始比例 --\n");
    Check(Near(ResolveOutlineWidthMeters(0.005f, "screenCoordinates", true), 0.005f),
          "0.x screen 原样");
    Check(Near(ResolveOutlineWidthMeters(0.005f, "screenCoordinates", false), 0.005f),
          "1.0 screen 原样");
}

// 4. none / 未知模式：原样返回。
void TestOtherModes() {
    std::printf("-- 其它模式与边界 --\n");
    Check(Near(ResolveOutlineWidthMeters(0.01f, "none", true), 0.01f), "none 原样");
    Check(Near(ResolveOutlineWidthMeters(0.01f, "", true), 0.01f), "空模式原样");
}

// 5. 非法/负值：返回 0，防止负宽度或 NaN 破坏渲染。
void TestInvalid() {
    std::printf("-- 非有限与负值防御 --\n");
    Check(Near(ResolveOutlineWidthMeters(0.0f, "worldCoordinates", true), 0.0f), "0 -> 0");
    Check(Near(ResolveOutlineWidthMeters(-0.5f, "worldCoordinates", true), 0.0f), "负值 -> 0");
    Check(Near(ResolveOutlineWidthMeters(NAN, "worldCoordinates", true), 0.0f), "NaN -> 0");
    Check(Near(ResolveOutlineWidthMeters(INFINITY, "worldCoordinates", true), 0.0f), "Inf -> 0");
}

}  // namespace

int main() {
    std::printf("=== VRM MToon 描边宽度换算测试 ===\n");
    TestVrm0World();
    TestVrm1World();
    TestScreen();
    TestOtherModes();
    TestInvalid();
    std::printf("结果: %d/%d 通过\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
