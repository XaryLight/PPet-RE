// PPet - C++ 移植版
// 渲染帧抓图与「像素证据」统计。
//
// 用途：
//   1. 模型诊断面板的「图片渲染」页要显示**当前实际渲染出来的画面**；
//   2. 更要紧的是回答「模型到底有没有出图」——draw calls、GL 错误、贴图、
//      FPS 全绿并不能证明帧缓冲里有像素。只有读回 RGBA 统计非透明像素，
//      才能给出可信结论（见 FramePixelStats / AnalyzeFramePixels）。
#pragma once

#include <cstddef>
#include <string>

namespace ppet {

/// 一帧 RGBA 数据的可见像素统计。
///
/// 阈值与 tools/render_verify.cpp 保持一致，便于两边结论互相对照：
///   alpha > 10 视为非透明，RGB 任一通道 > 8 视为非黑。
struct FramePixelStats {
    int width = 0;
    int height = 0;
    long long totalPixels = 0;
    long long nonTransparent = 0;  ///< alpha > 10 的像素数
    long long nonBlack = 0;        ///< RGB 有可见颜色的像素数
    double coveragePercent = 0.0; ///< 非透明像素占比（0~100）
};

/// 纯函数：统计 RGBA 帧数据的可见像素。
/// 内联在头文件里：不依赖任何 GL 符号，单测无需链接渲染层即可覆盖；
/// 运行时抓帧与离屏验证工具都复用同一套阈值与算法。
/// @param rgba   行序不限（统计与行序无关），每像素 4 字节
inline FramePixelStats AnalyzeFramePixels(const unsigned char* rgba, int width,
                                          int height) {
    FramePixelStats stats;
    if (rgba == nullptr || width <= 0 || height <= 0) return stats;
    stats.width = width;
    stats.height = height;
    stats.totalPixels = static_cast<long long>(width) * static_cast<long long>(height);
    for (long long i = 0; i < stats.totalPixels; ++i) {
        const unsigned char* p = rgba + static_cast<std::size_t>(i) * 4;
        // 阈值与 tools/render_verify.cpp 的覆盖率判定保持一致。
        if (p[3] > 10) ++stats.nonTransparent;
        if (p[0] > 8 || p[1] > 8 || p[2] > 8) ++stats.nonBlack;
    }
    stats.coveragePercent = 100.0 * static_cast<double>(stats.nonTransparent) /
                            static_cast<double>(stats.totalPixels);
    return stats;
}

/// 抓取当前帧。
/// @param pngPath   输出 PNG 路径（父目录需已存在）；**传空字符串表示只统计
///                  像素、不写文件**（用于不打开诊断面板时的低频运行时自检）
/// @param outWidth  输出宽度
/// @param outHeight 输出高度
/// @param error     失败原因
/// @param outStats  可选：输出本帧像素证据统计
/// @return 成功返回 true。
bool CaptureRenderFrame(const std::string& pngPath, int& outWidth, int& outHeight,
                        std::string& error, FramePixelStats* outStats = nullptr);

}  // namespace ppet
