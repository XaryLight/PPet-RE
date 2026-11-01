// PPet - C++ 移植版
// 图片解码（PNG / JPEG）封装。
//
// 为什么单独一个文件：
//   stb_image 的公开接口是 C 链接的全局符号。如果渲染层（src/render/Renderer.cpp）
//   也以 STB_IMAGE_IMPLEMENTATION 的方式包含 stb_image，两者会在链接期产生
//   重复符号。本文件通过 #define 把 stb_image 的公开符号统一改名为 vrm_stbi_*，
//   从而与渲染层互不冲突——即使两边同时包含 stb_image 也能正常链接。
//   stb_image 内部以 static 声明的辅助函数（stbi__*）具有内部链接，无需改名。
//
// 说明：这里只依赖 stb_image 的 PNG / JPEG 解码能力，
// 其它格式（KTX2 / HDR 等）解码失败时返回 false，由调用方回退到纯色。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ppet::vrm {

/// 解码后的图片（统一转换成 RGBA 四个通道）。
struct DecodedImage {
    int width = 0;
    int height = 0;
    int channels = 0;  ///< 固定为 4
    std::vector<unsigned char> pixels;
};

/// 解码内存中的 PNG / JPEG 图片。
/// @param data  图片字节
/// @param size  字节数
/// @param out   解码结果
/// @return 成功返回 true；失败返回 false（out 保持不变）。
bool DecodeImage(const unsigned char* data, std::size_t size, DecodedImage& out);

}  // namespace ppet::vrm
