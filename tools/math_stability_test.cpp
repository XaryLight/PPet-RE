// PPet - 矩阵/数值稳定性单测（报告 2.5.5）。纯逻辑、零依赖。
#include <src/tools/core/Math.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

namespace {

int g_failures = 0;
void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) ++g_failures;
}

using namespace ppet;

}  // namespace

int main() {
    std::printf("== 矩阵/数值稳定性单测 ==\n");

    // --- ClampRectInside 极端坐标（多显示器，int 溢出 UB 防护）---
    {
        const IntRect big{100, 100, 100000, 100000};
        const IntRect desktop{-2000000000, -2000000000, 2000, 2000};
        const IntRect out = ClampRectInside(big, desktop);
        Check(out.x >= desktop.x && out.x <= desktop.x + desktop.width,
              "极端负坐标下 x 被钳制进桌面范围且不溢出");
        Check(out.y >= desktop.y && out.y <= desktop.y + desktop.height,
              "极端负坐标下 y 被钳制进桌面范围且不溢出");

        const IntRect huge{0, 0, 3, 3};
        const IntRect dt{2000000000, 2000000000, 100, 100};
        const IntRect out2 = ClampRectInside(huge, dt);
        Check(out2.x >= 0 && out2.x <= 2000000000,
              "极端正坐标下 x 钳制不回绕成负数");
    }

    // --- Length / Normalize：大分量不溢出，小向量不误归零 ---
    {
        const float big = 1e30f;
        const float len = Length(Vec3{big, big, big});
        Check(std::isfinite(len), "大分量 Length 不溢出成 inf");
        Check(len > 0.0f, "大分量 Length 仍为正");

        const Vec3 n = Normalize(Vec3{big, big, big});
        Check(std::isfinite(n.x) && std::fabs(n.x - 0.57735f) < 1e-3f,
              "大分量 Normalize 归一到单位向量");

        const Vec3 tiny{1e-8f, 0.0f, 0.0f};
        const Vec3 tn = Normalize(tiny);
        Check(std::fabs(tn.x - 1.0f) < 1e-4f,
              "微小向量 Normalize 不误归零（旧绝对阈值会归零）");

        Check(Length(Vec3{0, 0, 0}) == 0.0f, "零向量 Length 为 0");
        Check(std::fabs(Length(Vec3{3, 4, 0}) - 5.0f) < 1e-5f,
              "常规向量 Length 正确（3-4-5）");
    }

    // --- Mat4::TryInverse：大平移矩阵不再误判不可逆 ---
    {
        const Mat4 m = Mat4::Translation(Vec3{100000.0f, -200000.0f, 300000.0f});
        Mat4 inv;
        const bool ok = m.TryInverse(inv);
        Check(ok, "大平移矩阵可逆（旧绝对阈值可能误判）");
        if (ok) {
            const Vec3 back = (m * inv).TransformPoint(Vec3{1, 2, 3});
            Check(std::fabs(back.x - 1) < 1e-2f && std::fabs(back.y - 2) < 1e-2f &&
                      std::fabs(back.z - 3) < 1e-2f,
                  "M * M^-1 还原原点");
        }

        Mat4 singular;
        singular.At(0, 0) = 0.0f;  // 奇异矩阵
        Mat4 out;
        Check(!singular.TryInverse(out), "奇异矩阵 TryInverse 返回 false");
    }

    // --- Perspective / Ortho：除零参数返回单位阵而非 NaN ---
    {
        const Mat4 bad = Perspective(1.0f, 0.0f, 0.1f, 100.0f);
        Check(bad.At(0, 0) == 1.0f && bad.At(1, 1) == 1.0f,
              "Perspective aspect=0 返回单位阵");
        const Mat4 bad2 = Perspective(1.0f, 1.5f, 1.0f, 1.0f);
        Check(bad2.At(2, 2) == 1.0f, "Perspective near==far 返回单位阵");
        const Mat4 good = Perspective(1.0f, 1.5f, 0.1f, 100.0f);
        Check(std::isfinite(good.At(0, 0)) && good.At(3, 2) == -1.0f,
              "Perspective 合法参数正常");

        const Mat4 o = Ortho(0.0f, 0.0f, -1.0f, 1.0f, 0.1f, 100.0f);
        Check(o.At(0, 0) == 1.0f, "Ortho r==l 返回单位阵");
        const Mat4 oGood = Ortho(-1.0f, 1.0f, -1.0f, 1.0f, 0.1f, 100.0f);
        Check(std::isfinite(oGood.At(0, 0)) && std::fabs(oGood.At(0, 0) - 1.0f) < 1e-5f,
              "Ortho 合法参数正常");
    }

    std::printf("\n结果：%d 项失败\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
