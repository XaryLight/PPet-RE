// PPet - C++ 移植版
// api/vendor：stb_image 的统一引用点（贴图解码）。
//
// stb 是 header-only：需要在**恰好一个**翻译单元里定义
//   #define STB_IMAGE_IMPLEMENTATION
// 再包含本头，其余翻译单元直接包含本头即可（只拿到声明）。
// 写 PNG 用 <stb/stb_image_write.h>，同理需 STB_IMAGE_WRITE_IMPLEMENTATION。
#pragma once

#include <stb/stb_image.h>

namespace ppet::api {
/// 贴图解码后的原始像素（RGBA，左上为原点；stb 已按 force_channels=4 统一）。
struct DecodedImage {
    int width = 0;
    int height = 0;
    int channels = 0;
};
}  // namespace ppet::api
