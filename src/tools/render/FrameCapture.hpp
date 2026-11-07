// PPet - C++ 移植版
// 渲染帧抓图：把当前 OpenGL 默认帧缓冲读回并写成 PNG。
//
// 用途：模型诊断面板的「图片渲染」页要显示**当前实际渲染出来的画面**，
// 而不只是模型的参数。仅当诊断面板可见时每 200 ms 调一次，开销可控。
#pragma once

#include <string>

namespace ppet {

/// 抓取当前帧。
/// @param pngPath   输出 PNG 路径（父目录需已存在）
/// @param outWidth  输出宽度
/// @param outHeight 输出高度
/// @param error     失败原因
/// @return 成功返回 true。
bool CaptureRenderFrame(const std::string& pngPath, int& outWidth, int& outHeight,
                        std::string& error);

}  // namespace ppet
