// PPet - C++ 移植版
// 帧像素证据统计（FrameCapture.hpp 的 AnalyzeFramePixels）单元测试。
//
// 背景：诊断面板曾经只凭「网格就绪 + GPU 就绪 + GL 错误为 0」就报「正常渲染」，
// 但主循环漏了清屏时，draw calls / 贴图 / FPS / GL 错误全部正常，帧缓冲却是
// 全空的 —— 屏幕上什么都看不到。这个纯函数是「是否真的出图」的最终裁决。
//
// 覆盖：退化输入、全透明、alpha 与 RGB 阈值边界、混合覆盖率、数值精度，
// 以及「RGB 有颜色但 alpha=0」（透明窗口合成下依然不可见）这一关键情形。
#include <cstdio>
#include <string>
#include <vector>

#include <src/tools/render/FrameCapture.hpp>

namespace {

int failures = 0;
int checks = 0;

void Check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) ++failures;
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what.c_str());
}

void CheckStats(const ppet::FramePixelStats& s, int w, int h, long long total,
                long long nonTransparent, long long nonBlack, double coverage,
                const std::string& what) {
    const bool ok = s.width == w && s.height == h && s.totalPixels == total &&
                    s.nonTransparent == nonTransparent && s.nonBlack == nonBlack &&
                    (s.coveragePercent - coverage < 1e-9) &&
                    (coverage - s.coveragePercent < 1e-9);
    Check(ok, what + " -> " + std::to_string(s.width) + "x" + std::to_string(s.height) +
                  " 非透明=" + std::to_string(s.nonTransparent) +
                  " 非黑=" + std::to_string(s.nonBlack) + " 覆盖率=" +
                  std::to_string(s.coveragePercent) + "%");
}

/// 构造 w*h 的 RGBA 帧，每像素同色。
std::vector<unsigned char> MakeFrame(int w, int h, unsigned char r, unsigned char g,
                                     unsigned char b, unsigned char a) {
    std::vector<unsigned char> px(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i < px.size(); i += 4) {
        px[i] = r;
        px[i + 1] = g;
        px[i + 2] = b;
        px[i + 3] = a;
    }
    return px;
}

}  // namespace

int main() {
    using ppet::AnalyzeFramePixels;

    std::printf("== 退化输入 ==\n");
    CheckStats(AnalyzeFramePixels(nullptr, 10, 10), 0, 0, 0, 0, 0, 0.0, "空指针");
    CheckStats(AnalyzeFramePixels(MakeFrame(2, 2, 0, 0, 0, 0).data(), 0, 2),
               0, 0, 0, 0, 0, 0.0, "宽度为 0");
    CheckStats(AnalyzeFramePixels(MakeFrame(2, 2, 0, 0, 0, 0).data(), 2, -1),
               0, 0, 0, 0, 0, 0.0, "高度为负");

    std::printf("== 全透明：这正是「指标全绿但没渲染」的帧 ==\n");
    {
        const auto f = MakeFrame(4, 4, 0, 0, 0, 0);
        CheckStats(AnalyzeFramePixels(f.data(), 4, 4), 4, 4, 16, 0, 0, 0.0,
                   "全透明黑：覆盖率为 0");
    }
    {
        // alpha=0 时 RGB 再有颜色，Windows 分层窗口合成后依然完全透明。
        const auto f = MakeFrame(4, 4, 255, 128, 64, 0);
        const auto s = AnalyzeFramePixels(f.data(), 4, 4);
        CheckStats(s, 4, 4, 16, 0, 16, 0.0,
                   "RGB 有颜色但 alpha=0：非黑=16，覆盖率仍为 0");
        Check(s.nonBlack == 16 && s.nonTransparent == 0,
              "非黑不能替代非透明作为出图依据");
    }

    std::printf("== alpha 阈值边界（> 10 才算非透明）==\n");
    CheckStats(AnalyzeFramePixels(MakeFrame(2, 2, 0, 0, 0, 10).data(), 2, 2),
               2, 2, 4, 0, 0, 0.0, "alpha=10 恰好不计");
    CheckStats(AnalyzeFramePixels(MakeFrame(2, 2, 0, 0, 0, 11).data(), 2, 2),
               2, 2, 4, 4, 0, 100.0, "alpha=11 计入，覆盖率 100%");
    CheckStats(AnalyzeFramePixels(MakeFrame(2, 2, 0, 0, 0, 255).data(), 2, 2),
               2, 2, 4, 4, 0, 100.0, "全不透明黑：有像素但非黑为 0");

    std::printf("== RGB 阈值边界（> 8 才算非黑）==\n");
    CheckStats(AnalyzeFramePixels(MakeFrame(2, 2, 8, 8, 8, 255).data(), 2, 2),
               2, 2, 4, 4, 0, 100.0, "RGB=8 恰好不计非黑");
    CheckStats(AnalyzeFramePixels(MakeFrame(2, 2, 9, 0, 0, 255).data(), 2, 2),
               2, 2, 4, 4, 4, 100.0, "单通道 9 即计入非黑");

    std::printf("== 混合覆盖率与精度 ==\n");
    {
        // 4 像素中 1 个不透明 -> 25%。
        std::vector<unsigned char> f(4 * 4, 0);
        f[3] = 255;
        CheckStats(AnalyzeFramePixels(f.data(), 2, 2), 2, 2, 4, 1, 0, 25.0,
                   "1/4 不透明 -> 25%");
    }
    {
        // 与诊断面板视口同尺寸：350 x 600，模拟约 20% 覆盖率的真实模型帧。
        const int w = 350, h = 600;
        std::vector<unsigned char> f(static_cast<std::size_t>(w) * h * 4, 0);
        const long long opaque = 43581;  // 真实问题模型离屏验证得到的像素数
        for (long long i = 0; i < opaque; ++i) f[static_cast<std::size_t>(i) * 4 + 3] = 255;
        const auto s = AnalyzeFramePixels(f.data(), w, h);
        Check(s.totalPixels == 210000 && s.nonTransparent == opaque,
              "350x600 帧：总数与非透明计数正确");
        Check(s.coveragePercent > 20.7 && s.coveragePercent < 20.8,
              "350x600 帧：覆盖率约 20.75%，与 render_verify 结论一致");
    }

    std::printf("\nframe_evidence_test: %d 项检查，%d 项失败\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
