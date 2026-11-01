// PPet - C++ 移植版
// 图片解码实现（基于 stb_image，符号已改名以避免与渲染层冲突）。
#include "model/vrm/VrmImage.hpp"

// stb_image 的公开符号统一改名，避免与渲染层重复定义。
// 这里的顺序很重要：必须在包含 stb_image.h 之前全部定义完成。
#define stbi_load                         vrm_stbi_load
#define stbi_load_from_memory             vrm_stbi_load_from_memory
#define stbi_load_from_callbacks          vrm_stbi_load_from_callbacks
#define stbi_load_from_file               vrm_stbi_load_from_file
#define stbi_load_from_file_16            vrm_stbi_load_from_file_16
#define stbi_load_16                      vrm_stbi_load_16
#define stbi_load_16_from_memory          vrm_stbi_load_16_from_memory
#define stbi_load_16_from_callbacks       vrm_stbi_load_16_from_callbacks
#define stbi_loadf                        vrm_stbi_loadf
#define stbi_loadf_from_memory            vrm_stbi_loadf_from_memory
#define stbi_loadf_from_callbacks         vrm_stbi_loadf_from_callbacks
#define stbi_loadf_from_file              vrm_stbi_loadf_from_file
#define stbi_load_gif_from_memory         vrm_stbi_load_gif_from_memory
#define stbi_info                         vrm_stbi_info
#define stbi_info_from_memory             vrm_stbi_info_from_memory
#define stbi_info_from_callbacks          vrm_stbi_info_from_callbacks
#define stbi_info_from_file               vrm_stbi_info_from_file
#define stbi_is_16_bit                    vrm_stbi_is_16_bit
#define stbi_is_16_bit_from_memory        vrm_stbi_is_16_bit_from_memory
#define stbi_is_16_bit_from_callbacks     vrm_stbi_is_16_bit_from_callbacks
#define stbi_is_16_bit_from_file          vrm_stbi_is_16_bit_from_file
#define stbi_is_hdr                       vrm_stbi_is_hdr
#define stbi_is_hdr_from_memory           vrm_stbi_is_hdr_from_memory
#define stbi_is_hdr_from_callbacks        vrm_stbi_is_hdr_from_callbacks
#define stbi_is_hdr_from_file             vrm_stbi_is_hdr_from_file
#define stbi_failure_reason               vrm_stbi_failure_reason
#define stbi_image_free                   vrm_stbi_image_free
#define stbi_set_flip_vertically_on_load  vrm_stbi_set_flip_vertically_on_load
#define stbi_set_flip_vertically_on_load_thread \
    vrm_stbi_set_flip_vertically_on_load_thread
#define stbi_set_unpremultiply_on_load vrm_stbi_set_unpremultiply_on_load
#define stbi_set_unpremultiply_on_load_thread \
    vrm_stbi_set_unpremultiply_on_load_thread
#define stbi_convert_iphone_png_to_rgb vrm_stbi_convert_iphone_png_to_rgb
#define stbi_convert_iphone_png_to_rgb_thread vrm_stbi_convert_iphone_png_to_rgb_thread
#define stbi_convert_wchar_to_utf8      vrm_stbi_convert_wchar_to_utf8
#define stbi_hdr_to_ldr_gamma           vrm_stbi_hdr_to_ldr_gamma
#define stbi_hdr_to_ldr_scale           vrm_stbi_hdr_to_ldr_scale
#define stbi_ldr_to_hdr_gamma           vrm_stbi_ldr_to_hdr_gamma
#define stbi_ldr_to_hdr_scale           vrm_stbi_ldr_to_hdr_scale
#define stbi_zlib_decode_buffer         vrm_stbi_zlib_decode_buffer
#define stbi_zlib_decode_malloc         vrm_stbi_zlib_decode_malloc
#define stbi_zlib_decode_malloc_guesssize vrm_stbi_zlib_decode_malloc_guesssize
#define stbi_zlib_decode_malloc_guesssize_headerflag \
    vrm_stbi_zlib_decode_malloc_guesssize_headerflag
#define stbi_zlib_decode_noheader_buffer  vrm_stbi_zlib_decode_noheader_buffer
#define stbi_zlib_decode_noheader_malloc  vrm_stbi_zlib_decode_noheader_malloc

#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

namespace ppet::vrm {

bool DecodeImage(const unsigned char* data, std::size_t size, DecodedImage& out) {
    if (data == nullptr || size == 0) return false;
    // stb 的接口用 int 表示长度，超过 2 GB 的图片没有意义。
    if (size > 0x7FFFFFFFu) return false;

    int width = 0;
    int height = 0;
    int channels = 0;
    // 统一请求 4 通道（RGBA），渲染层不需要区分原始通道数。
    stbi_uc* pixels = vrm_stbi_load_from_memory(data, static_cast<int>(size), &width, &height,
                                                &channels, 4);
    if (pixels == nullptr) return false;
    if (width <= 0 || height <= 0) {
        vrm_stbi_image_free(pixels);
        return false;
    }
    const std::size_t pixelCount = static_cast<std::size_t>(width) *
                                   static_cast<std::size_t>(height) * 4u;
    out.width = width;
    out.height = height;
    out.channels = 4;
    out.pixels.assign(pixels, pixels + pixelCount);
    vrm_stbi_image_free(pixels);
    return true;
}

}  // namespace ppet::vrm
