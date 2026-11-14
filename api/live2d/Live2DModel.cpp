// PPet - C++ 移植版
// Live2D 模型后端实现。
#include <api/live2d/Live2DModel.hpp>

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

#include <src/tools/core/FileIO.hpp>
#include <src/tools/core/Log.hpp>
#include <api/live2d/CubismRenderPlan.hpp>
#include <api/live2d/Live2DFrame.hpp>
#include <api/live2d/MotionQueue.hpp>

// stb_image：本文件是 Live2D 后端唯一一处 STB_IMAGE_IMPLEMENTATION。
//
// stb_image 的公开符号是 C 链接的全局符号，其它 TU 也可能包含它：
//   - src/render/Renderer.cpp 已把符号改名为 ppet_stbi_*；
//   - src/model/vrm/VrmImage.cpp 已把符号改名为 vrm_stbi_*。
// 如果这里再用原始名字，链接同一个可执行文件时会出现重复符号错误，
// 因此把公开符号统一改名为 l2d_stbi_*，三份实现互不冲突。
#define stbi_load                         l2d_stbi_load
#define stbi_load_from_memory             l2d_stbi_load_from_memory
#define stbi_load_from_callbacks          l2d_stbi_load_from_callbacks
#define stbi_load_from_file               l2d_stbi_load_from_file
#define stbi_load_from_file_16            l2d_stbi_load_from_file_16
#define stbi_load_16                      l2d_stbi_load_16
#define stbi_load_16_from_memory          l2d_stbi_load_16_from_memory
#define stbi_load_16_from_callbacks       l2d_stbi_load_16_from_callbacks
#define stbi_loadf                        l2d_stbi_loadf
#define stbi_loadf_from_memory            l2d_stbi_loadf_from_memory
#define stbi_loadf_from_callbacks         l2d_stbi_loadf_from_callbacks
#define stbi_loadf_from_file              l2d_stbi_loadf_from_file
#define stbi_load_gif_from_memory         l2d_stbi_load_gif_from_memory
#define stbi_info                         l2d_stbi_info
#define stbi_info_from_memory             l2d_stbi_info_from_memory
#define stbi_info_from_callbacks          l2d_stbi_info_from_callbacks
#define stbi_info_from_file               l2d_stbi_info_from_file
#define stbi_is_16_bit                    l2d_stbi_is_16_bit
#define stbi_is_16_bit_from_memory        l2d_stbi_is_16_bit_from_memory
#define stbi_is_16_bit_from_callbacks     l2d_stbi_is_16_bit_from_callbacks
#define stbi_is_16_bit_from_file          l2d_stbi_is_16_bit_from_file
#define stbi_is_hdr                       l2d_stbi_is_hdr
#define stbi_is_hdr_from_memory           l2d_stbi_is_hdr_from_memory
#define stbi_is_hdr_from_callbacks        l2d_stbi_is_hdr_from_callbacks
#define stbi_is_hdr_from_file             l2d_stbi_is_hdr_from_file
#define stbi_failure_reason               l2d_stbi_failure_reason
#define stbi_image_free                   l2d_stbi_image_free
#define stbi_set_flip_vertically_on_load  l2d_stbi_set_flip_vertically_on_load
#define stbi_set_flip_vertically_on_load_thread \
    l2d_stbi_set_flip_vertically_on_load_thread
#define stbi_set_unpremultiply_on_load l2d_stbi_set_unpremultiply_on_load
#define stbi_set_unpremultiply_on_load_thread \
    l2d_stbi_set_unpremultiply_on_load_thread
#define stbi_convert_iphone_png_to_rgb l2d_stbi_convert_iphone_png_to_rgb
#define stbi_convert_iphone_png_to_rgb_thread \
    l2d_stbi_convert_iphone_png_to_rgb_thread
#define stbi_convert_wchar_to_utf8      l2d_stbi_convert_wchar_to_utf8
#define stbi_hdr_to_ldr_gamma           l2d_stbi_hdr_to_ldr_gamma
#define stbi_hdr_to_ldr_scale           l2d_stbi_hdr_to_ldr_scale
#define stbi_ldr_to_hdr_gamma           l2d_stbi_ldr_to_hdr_gamma
#define stbi_ldr_to_hdr_scale           l2d_stbi_ldr_to_hdr_scale
#define stbi_zlib_decode_buffer         l2d_stbi_zlib_decode_buffer
#define stbi_zlib_decode_malloc         l2d_stbi_zlib_decode_malloc
#define stbi_zlib_decode_malloc_guesssize l2d_stbi_zlib_decode_malloc_guesssize
#define stbi_zlib_decode_malloc_guesssize_headerflag \
    l2d_stbi_zlib_decode_malloc_guesssize_headerflag
#define stbi_zlib_decode_noheader_buffer l2d_stbi_zlib_decode_noheader_buffer
#define stbi_zlib_decode_noheader_malloc l2d_stbi_zlib_decode_noheader_malloc

#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

#include <GL/glew.h>

#include <api/live2d/CubismCore.hpp>
#include <api/live2d/Live2DManifest.hpp>
#include <api/live2d/Live2DPhysics.hpp>
#include <src/tools/render/RenderUtils.hpp>
#include <src/tools/model/MotionRecognizer.hpp>

namespace ppet::live2d {
namespace {

namespace cubism = ppet::live2d::cubism;

/// 把纯逻辑渲染计划里的混合因子翻译成 GL 常量。
/// 这层映射放在 GL 侧，CubismRenderPlan.hpp 才能保持零依赖、可单测。
inline GLenum ToGlBlendFactor(BlendFactor factor) {
    switch (factor) {
        case BlendFactor::Zero: return GL_ZERO;
        case BlendFactor::SrcAlpha: return GL_SRC_ALPHA;
        case BlendFactor::OneMinusSrcAlpha: return GL_ONE_MINUS_SRC_ALPHA;
        case BlendFactor::DstColor: return GL_DST_COLOR;
        case BlendFactor::One:
        default: return GL_ONE;
    }
}

/// 收集一帧的 Drawable 元数据，交给 BuildRenderPlan() 决策。
inline std::vector<DrawableDesc> CollectDrawableDescs(
    cubism::Model* model, int drawableCount, const std::vector<std::vector<cubism::MaskRef>>& masks,
    const std::vector<char>& isMaskDrawable, int textureCount) {
    std::vector<DrawableDesc> descs;
    descs.reserve(static_cast<std::size_t>(std::max(0, drawableCount)));
    for (int i = 0; i < drawableCount; ++i) {
        cubism::Drawable d;
        if (!cubism::GetDrawable(model, i, d)) continue;
        DrawableDesc desc;
        desc.index = i;
        desc.visible = d.visible;
        desc.opacity = d.opacity;
        desc.renderOrder = d.renderOrder;
        desc.blend = d.additiveBlend
                         ? DrawableBlend::Additive
                         : (d.multiplicativeBlend ? DrawableBlend::Multiplicative
                                                  : DrawableBlend::Normal);
        desc.hasOwnTexture = d.textureIndex >= 0 && d.textureIndex < textureCount;
        desc.isMaskDrawable =
            static_cast<std::size_t>(i) < isMaskDrawable.size() && isMaskDrawable[static_cast<std::size_t>(i)] != 0;
        if (static_cast<std::size_t>(i) < masks.size() && !masks[static_cast<std::size_t>(i)].empty()) {
            desc.useMask = true;
            desc.maskCount = static_cast<int>(masks[static_cast<std::size_t>(i)].size());
            // 多遮罩里只要有一个是反转遮罩（挖洞），整体按反转处理。
            for (const cubism::MaskRef& ref : masks[static_cast<std::size_t>(i)]) {
                if (ref.inverted) { desc.invertMask = true; break; }
            }
        }
        descs.push_back(desc);
    }
    return descs;
}

/// 视线追踪使用的参数名（Cubism 的标准参数名）。
constexpr const char* kParamAngleX = "ParamAngleX";
constexpr const char* kParamAngleY = "ParamAngleY";
constexpr const char* kParamEyeBallX = "ParamEyeBallX";
constexpr const char* kParamEyeBallY = "ParamEyeBallY";
constexpr const char* kParamBodyAngleX = "ParamBodyAngleX";
constexpr const char* kParamBodyAngleZ = "ParamBodyAngleZ";
constexpr const char* kParamEyeLOpen = "ParamEyeLOpen";
constexpr const char* kParamEyeROpen = "ParamEyeROpen";
constexpr const char* kParamBreath = "ParamBreath";
constexpr const char* kParamMouthOpenY = "ParamMouthOpenY";
constexpr const char* kParamMouthForm = "ParamMouthForm";

/// 顶���着色器（GLSL 330 core）。
///
/// 与官方 Cubism 顶点着色器（#version 120）等价：
///   - 位置直接乘 MVP 矩阵；
///   - 纹理坐标翻转 V（Live2D 的 UV 原点在左上，OpenGL 在左下）。
const char* kVertexShader = R"(#version 330 core
layout(location = 0) in vec2 a_position;
layout(location = 1) in vec2 a_texCoord;

uniform mat4 u_matrix;

out vec2 v_texCoord;
out vec2 v_maskUV;

void main() {
    gl_Position = u_matrix * vec4(a_position, 0.0, 1.0);
    // Live2D 的 UV 原点在左上，OpenGL 纹理坐标原点在左下，需要翻转 V。
    v_texCoord = vec2(a_texCoord.x, 1.0 - a_texCoord.y);
    // 遮罩纹理用同一个 u_matrix 绘制，因此裁剪空间坐标就是遮罩纹理 UV。
    v_maskUV = gl_Position.xy * 0.5 + 0.5;
}
)";

/// 片元着色器（GLSL 330 core）。
///
/// 输出 premultiplied alpha：桌宠窗口是透明叠加窗口，
/// 需要用 GL_ONE / GL_ONE_MINUS_SRC_ALPHA 混合，
/// 因此 RGB 必须预先乘以 alpha。
const char* kFragmentShader = R"(#version 330 core
in vec2 v_texCoord;
in vec2 v_maskUV;

uniform sampler2D s_texture0;
uniform sampler2D s_mask;
uniform vec4 u_baseColor;
uniform float u_useMask;
uniform float u_invertMask;
/// 遮罩通道标志：>0.5 时只输出几何覆盖度（alpha），完全忽略贴图内容。
/// Cubism 的遮罩是「另一块部件的几何形状」，与它贴图上哪里透明无关；
/// 早期版本用贴图 alpha 当覆盖度，导致遮罩范围收缩 → 被裁剪部件出现透明破洞。
uniform float u_maskPass;
/// 加法混合（发光）标志：>0.5 时启用钳制式加法混合。
uniform float u_additive;

out vec4 fragColor;

void main() {
    // --- 遮罩通道：只输出几何覆盖度 ---
    // 遮罩纹理的 RGB 无关紧要（主 pass 只采样 .a），这里输出纯 alpha。
    // 用 GL_ONE/ONE_MINUS_SRC_ALPHA 叠加多个遮罩 => alpha 累积即「并集」，
    // 与官方 Cubism 把多个 mask 画进同一张裁剪缓冲的语义一致。
    if (u_maskPass > 0.5) {
        fragColor = vec4(u_baseColor.a);
        return;
    }

    vec4 texColor = texture(s_texture0, v_texCoord);

    // Live2D 纹理本身是 **premultiplied alpha**（官方 Cubism 导出规范即如此），
    // 因此 RGB 已经乘过 alpha，这里**不能**再乘一次
    // （二次预乘会让半透明区域变暗、边缘发黑）。
    vec4 color = texColor * u_baseColor;

    if (u_useMask > 0.5) {
        // 遮罩纹理里存的是遮罩部件的覆盖度（在 alpha 通道）。
        float m = texture(s_mask, v_maskUV).a;
        // 反转遮罩是「挖洞」：遮罩范围内不显示，范围外才显示。
        if (u_invertMask > 0.5) m = 1.0 - m;
        // premultiplied 下 RGB 与 A 一起乘才保持预乘关系。
        color *= m;
    }

    if (u_additive > 0.5) {
        // 加法混合（发光）：与目标色相加并**钳制到 1.0**。
        //
        // 【为什么必须钳制】官方 Cubism 5.3+ 的 ColorBlend 着色器对
        // ADD 模式写的是 min(colorSource + colorDestination, 1.0)。
        // 不钳制时，多个发光部件叠加会让 RGB 迅速溢出成纯白，
        // 整个角色被自己的高光糊住 —— 表现为「渲染异常偏白」。
        // 官方用离屏纹理实现（读回目标色再相加），
        // 本项目在主帧缓冲上做等价处理：加法部件用 GL_ONE/GL_ONE 叠加，
        // 由片元着色器把结果限制在 1.0 以内。
        color.rgb = min(color.rgb, vec3(1.0));
        // 发光不增加不透明度：alpha 通道保持目标值。
        color.a = min(color.a, 1.0);
    }

    fragColor = color;
}
)";

/// 编译着色器程序。
GLuint CompileProgram(const char* vertexSrc, const char* fragmentSrc) {
    auto compile = [](GLenum type, const char* src) -> GLuint {
        const GLuint shader = glCreateShader(type);
        if (!shader) return 0;
        glShaderSource(shader, 1, &src, nullptr);
        glCompileShader(shader);
        GLint ok = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024] = {0};
            glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
            std::fprintf(stderr, "[Live2D] 着色器编译失败：%s\n", log);
            glDeleteShader(shader);
            return 0;
        }
        return shader;
    };

    const GLuint vs = compile(GL_VERTEX_SHADER, vertexSrc);
    if (!vs) return 0;
    const GLuint fs = compile(GL_FRAGMENT_SHADER, fragmentSrc);
    if (!fs) {
        glDeleteShader(vs);
        return 0;
    }

    const GLuint program = glCreateProgram();
    if (!program) {
        glDeleteShader(vs);
        glDeleteShader(fs);
        return 0;
    }
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);

    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    // 链接后即可删除着色器对象，程序会持有它们的引用。
    glDetachShader(program, vs);
    glDetachShader(program, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);

    if (!ok) {
        char log[1024] = {0};
        glGetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
        std::fprintf(stderr, "[Live2D] 着色器链接失败：%s\n", log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

/// 创建 1x1 的白色占位纹理。
///
/// 纹理加载失败时用它顶上，保证模型仍能以「纯白块」的形式显示，
/// 而不是因为一张纹理缺失就整个模型不可见。
GLuint CreateWhiteTexture() {
    GLuint texture = 0;
    glGenTextures(1, &texture);
    if (!texture) return 0;
    glBindTexture(GL_TEXTURE_2D, texture);
    const unsigned char pixels[4] = {255, 255, 255, 255};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return texture;
}

/// 用 stb_image 读取纹理文件并上传到 GL。
///
/// Live2D 的纹理 UV 原点在左上，而 OpenGL 纹理坐标原点在左下。
/// 这里采用与官方 Cubism 相同的做法：**上传时不翻转行序**，
/// 由顶点着色器做 `1.0 - v` 还原。
///
/// @return 成功返回纹理 id；失败返回 0。
GLuint LoadTexture(const std::string& path) {
    // 先读进内存再解码：stb_image 的文件接口在 Windows 上打不开
    // 含中文的模型目录（Live2D 模型常放在中文路径下），会静默丢贴图。
    std::vector<unsigned char> bytes;
    if (!ReadFileBytes(path, bytes) || bytes.empty()) {
        std::fprintf(stderr, "[Live2D] 纹理文件无法读取：%s\n", path.c_str());
        return 0;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* pixels =
        l2d_stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                  &width, &height, &channels, 4);
    if (!pixels) {
        std::fprintf(stderr, "[Live2D] 纹理加载失败：%s（%s）\n", path.c_str(),
                     l2d_stbi_failure_reason());
        return 0;
    }
    if (width <= 0 || height <= 0) {
        l2d_stbi_image_free(pixels);
        return 0;
    }

    GLuint texture = 0;
    glGenTextures(1, &texture);
    if (!texture) {
        l2d_stbi_image_free(pixels);
        return 0;
    }
    // --- 预乘 alpha ---
    //
    // 【为什么必须在这里预乘】
    //   PNG 里存的是 **straight alpha**（RGB 是原始颜色，alpha 单独存放），
    //   而本项目的片元着色器按 **premultiplied alpha** 使用纹理
    //   （官方 Cubism 的 premultiplied 渲染路径：
    //    FragShaderSrcPremultipliedAlpha.frag 里就是 `texColor * u_baseColor`，
    //    不再乘一次 alpha）。
    //   两边约定不一致会出事：
    //     · 着色器按预乘用、纹理是 straight → 半透明区域偏亮，
    //       发光（加法混合）部件尤其明显，整屏被高光刷白；
    //     · 着色器按直通用、纹理是预乘 → 边缘发黑。
    //   在上传前把 RGB 乘上 alpha，两边就一致了。
    for (int i = 0; i < width * height; ++i) {
        const unsigned char a = pixels[i * 4 + 3];
        pixels[i * 4 + 0] = static_cast<unsigned char>(
            (pixels[i * 4 + 0] * a + 127) / 255);
        pixels[i * 4 + 1] = static_cast<unsigned char>(
            (pixels[i * 4 + 1] * a + 127) / 255);
        pixels[i * 4 + 2] = static_cast<unsigned char>(
            (pixels[i * 4 + 2] * a + 127) / 255);
    }

    glBindTexture(GL_TEXTURE_2D, texture);
    // 不翻转行序：Live2D 的 UV 原点在左上，由着色器做 1.0 - v。
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 pixels);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    l2d_stbi_image_free(pixels);
    return texture;
}

}  // namespace

// ---------------------------------------------------------------------------
// Impl：把「有状态的数据」收拢到一个结构里，避免头文件暴露实现细节。
// ---------------------------------------------------------------------------

struct Live2DModel::Impl {
    /// .model3.json 的解析结果。
    Live2DManifest manifest;

    /// Cubism 模型句柄。
    cubism::Model* model = nullptr;

    /// 画布信息（决定模型单位到像素的换算）。
    cubism::CanvasInfo canvas;

    /// 纹理 id 列表（顺序与 manifest.texturePaths 一致）。
    std::vector<GLuint> textures;

    /// 已解析的动作文件。
    std::vector<Motion> motions;
    /// 与 motions 一一对应的显示名（控制台 action list / action <name> do）。
    std::vector<std::string> motionNames;

    /// 动作组 -> 动作下标，用于随机播放。
    std::vector<std::vector<int>> motionGroups;

    /// 每个部件对应的 VAO / VBO / EBO。
    struct DrawableBuffers {
        GLuint vao = 0;
        GLuint vbo = 0;
        GLuint ebo = 0;
        int indexCount = 0;
    };
    std::vector<DrawableBuffers> buffers;

    /// 着色器程序与 uniform 位置。
    GLuint program = 0;
    GLint uniformMatrix = -1;
    GLint uniformTexture = -1;
    GLint uniformBaseColor = -1;
    GLint uniformMask = -1;
    GLint uniformUseMask = -1;
    GLint uniformInvertMask = -1;
    GLint uniformMaskPass = -1;

    /// 白色占位纹理。
    GLuint whiteTexture = 0;

    /// 遮罩纹理（离屏 FBO）。
    ///
    /// Cubism 的遮罩是「另一块部件的几何」：被它遮住的部件只显示落在
    /// 遮罩范围内的部分。官方渲染器把遮罩单独画进一张纹理，再用它裁剪。
    /// 这里照此实现：遮罩用与主画面**同一个 MVP 矩阵**画进 FBO，
    /// 于是「模型坐标 → 遮罩纹理 UV」与「模型坐标 → 裁剪空间」一一对应，
    /// 片元着色器按裁剪空间坐标采样即可。
    struct MaskTarget {
        GLuint framebuffer = 0;
        GLuint texture = 0;
        int width = 0;
        int height = 0;
    };
    MaskTarget mask;

    /// 加法混合（发光部件）的离屏目标。
    ///
    /// 【为什么需要单独一张离屏纹理】
    ///   官方 Cubism 5.3+ 的渲染器把发光部件画进一张离屏纹理，
    ///   再用 ColorBlend 着色器做 min(src + dst, 1.0) 的**钳制**相加。
    ///   直接在主帧缓冲上用 GL_ONE/GL_ONE 相加，RGB 会无界累加：
    ///   多个高光部件叠在一起就把整块画面推到纯白——
    ///   这正是「Live2D 渲染异常偏白」的成因
    ///   （实测：白像素占比从 0.61% 暴涨到 15.00%）。
    ///
    ///   这里用等效但更轻量的做法：加法部件画进离屏纹理后，
    ///   以「钳制相加」的方式合成回主画面，
    ///   数学上与官方的 min(src + dst, 1.0) 一致。
    struct BlendTarget {
        GLuint framebuffer = 0;
        GLuint texture = 0;
        int width = 0;
        int height = 0;
    };
    BlendTarget blend;

    /// 每个部件的遮罩列表（模型加载后是静态的，只解析一次）。
    std::vector<std::vector<cubism::MaskRef>> masks;

    /// 被别的部件当作遮罩使用的部件下标。
    /// 这些部件**不直接画进主画面**，只用于生成遮罩纹理。
    std::vector<char> isMaskDrawable;

    /// 上一帧的部件数量，用于检测模型结构变化。
    int drawableCount = 0;

    /// 遮罩关系是否已解析。
    bool masksResolved = false;

    /// 真实内容包围盒（模型单位）：由 Load() 遍历可见部件顶点算出。
    /// 画布 canvas.sizeInPixels 往往远大于角色本身（大量透明留白），
    /// 用画布做窗口宽高比会把模型缩得很小甚至横向截断；内容帧才是
    /// 「模型渲染的真实像素」对应的取景框。见 Live2DFrame.hpp。
    ppet::live2d::ContentFrame contentFrame;

    /// Live2D 物理（头发/裙摆摇曳）。
    Live2DPhysics physics;

    /// 物理涉及到的参数名 -> Cubism 参数索引（加载时解析一次）。
    std::map<std::string, int> physicsParamIndex;

    /// 物理输入/输出用到的参数缓存（每帧从 Cubism 读入、算完写回）。
    std::map<std::string, float> physicsParams;
};

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------

Live2DModel::Live2DModel() : impl_(std::make_unique<Impl>()) {}

Live2DModel::~Live2DModel() {
    ReleaseGraphics();
    if (impl_ && impl_->model) {
        cubism::DestroyModel(impl_->model);
        impl_->model = nullptr;
    }
}

bool Live2DModel::IsLoaded() const { return loaded_; }

std::string Live2DModel::DisplayName() const { return displayName_; }

// ---------------------------------------------------------------------------
// 加载
// ---------------------------------------------------------------------------

bool Live2DModel::Load(const std::string& path, std::string& error) {
    loaded_ = false;

    if (!cubism::Available()) {
        error = "Live2D 模型需要编译 Cubism SDK 后端（CMake 选项 PPET_WITH_CUBISM）";
        return false;
    }

    // --- 1) 定位并解析 .model3.json ---
    std::string model3Path;
    if (!LocateModel3Json(path, model3Path, error)) {
        return false;
    }
    Live2DManifest manifest;
    if (!ParseModel3Json(model3Path, manifest, error)) {
        return false;
    }

    // --- 2) 打开 moc ---
    cubism::Model* model = cubism::CreateModel();
    if (!model) {
        error = "内存不足，无法创建 Live2D 模型";
        return false;
    }
    std::string mocError;
    if (!cubism::OpenModel(model, manifest.mocPath, mocError)) {
        cubism::DestroyModel(model);
        error = mocError;
        return false;
    }

    // --- 3) 读取画布信息 ---
    cubism::CanvasInfo canvas;
    if (!cubism::ReadCanvasInfo(model, canvas)) {
        cubism::DestroyModel(model);
        error = "无法读取 Live2D 模型的画布信息";
        return false;
    }
    if (canvas.pixelsPerUnit <= 0.0f) canvas.pixelsPerUnit = 1.0f;

    // 提交到 Impl：先释放旧的，再接管新的。
    ReleaseGraphics();
    if (impl_->model) {
        cubism::DestroyModel(impl_->model);
        impl_->model = nullptr;
    }
    impl_->manifest = manifest;
    impl_->model = model;
    impl_->canvas = canvas;

    displayName_ = manifest.displayName;

    // --- 4) 纹理与动作 ---
    // 纹理需要 GL 上下文，如果当前没有上下文则推迟到首次 Render 时加载。
    LoadTextures();
    LoadMotions();

    // --- 5) 解析视线追踪参数 ---
    ResolveTrackingParameters();

    // --- 6) 载入物理（头发摇曳）---
    LoadPhysics();

    // --- 7) 探测表情参数（待机随机小表情）---
    ResolveFaceParameters();

    // --- 8) 计算真实内容帧 ---
    // 必须在 Load() 内完成：feature_verify --auto-size 会在创建 GL 窗口**之前**
    // 先 Load 一次并调 PreferredFrameAspect() 探测动态像素比，那时还没有
    // 任何一帧 Render()。内容帧只依赖 CPU 侧的 moc3 顶点，不依赖 GL 上下文。
    ComputeContentFrame();

    loaded_ = true;
    return true;
}

void Live2DModel::ComputeContentFrame() {
    impl_->contentFrame = ppet::live2d::ContentFrame{};
    if (!impl_ || !impl_->model) return;

    // 让 Core 以默认参数重算一次顶点（Load 阶段可能还没跑过 UpdateModel）。
    cubism::ResetParametersToDefault(impl_->model);
    cubism::UpdateModel(impl_->model);

    const int count = cubism::DrawableCount(impl_->model);
    // 用清单里的贴图数量判「有贴图」，不能依赖 impl_->textures：
    // 无 GL 上下文时 LoadTextures() 会推迟上传，textures 为空会误判。
    const int textureCount = std::max(
        1, static_cast<int>(impl_->manifest.texturePaths.size()));
    ppet::live2d::ContentFrameBuilder builder;
    for (int i = 0; i < count; ++i) {
        cubism::Drawable d;
        if (!cubism::GetDrawable(impl_->model, i, d)) continue;
        const bool hasTexture = d.textureIndex >= 0 && d.textureIndex < textureCount;
        // 只统计「可见 + 有贴图」的部件：纯几何遮罩（无贴图）与隐藏部件
        // 不该撑大取景框，否则又回到「画布过大 → 角色缩小」的老问题。
        const bool include = d.visible && d.opacity > 0.0f && hasTexture;
        builder.AddDrawable(d.positions, d.vertexCount, include);
    }
    impl_->contentFrame = builder.Frame();

    PPET_LOG_TAG_DEBUG(
        "LIVE2D",
        "真实内容帧：%s，宽高比 %.3f（画布 %.0fx%.0f，宽高比 %.3f），内容范围 "
        "x[%.2f,%.2f] y[%.2f,%.2f]",
        impl_->contentFrame.valid ? "有效" : "无效",
        static_cast<double>(impl_->contentFrame.Aspect()),
        static_cast<double>(impl_->canvas.sizeInPixels.x),
        static_cast<double>(impl_->canvas.sizeInPixels.y),
        static_cast<double>(impl_->canvas.sizeInPixels.y > 0.0f
                                ? impl_->canvas.sizeInPixels.x / impl_->canvas.sizeInPixels.y
                                : 0.0f),
        static_cast<double>(impl_->contentFrame.minX),
        static_cast<double>(impl_->contentFrame.maxX),
        static_cast<double>(impl_->contentFrame.minY),
        static_cast<double>(impl_->contentFrame.maxY));
}

void Live2DModel::LoadPhysics() {
    if (!impl_) return;
    impl_->physics.Reset();
    impl_->physicsParamIndex.clear();
    impl_->physicsParams.clear();

    const std::string& physicsPath = impl_->manifest.physicsPath;
    if (physicsPath.empty()) return;

    PhysicsFile file;
    std::string error;
    if (!LoadPhysicsFile(physicsPath, file, error)) {
        // 物理文件有问题不影响模型本体（只是没有头发摇曳）。
        std::fprintf(stderr, "[Live2D] %s\n", error.c_str());
        return;
    }
    if (!impl_->physics.Load(file)) return;

    // 收集物理用到的参数，建立「参数名 -> 索引」映射并登记参数范围。
    for (const PhysicsSetting& setting : file.settings) {
        for (const PhysicsInput& input : setting.inputs) {
            const int index = cubism::FindParameter(impl_->model, input.parameter);
            if (index < 0) continue;
            impl_->physicsParamIndex[input.parameter] = index;
            impl_->physicsParams[input.parameter] = 0.0f;
            float minValue = -1.0f;
            float maxValue = 1.0f;
            if (cubism::ParameterRange(impl_->model, index, minValue, maxValue)) {
                impl_->physics.SetParameterRange(input.parameter, minValue, maxValue);
            }
        }
        for (const PhysicsOutput& output : setting.outputs) {
            const int index = cubism::FindParameter(impl_->model, output.parameter);
            if (index < 0) continue;
            impl_->physicsParamIndex[output.parameter] = index;
            impl_->physicsParams[output.parameter] = 0.0f;
            float minValue = -1.0f;
            float maxValue = 1.0f;
            if (cubism::ParameterRange(impl_->model, index, minValue, maxValue)) {
                impl_->physics.SetParameterRange(output.parameter, minValue, maxValue);
            }
        }
    }

    // 安定化：让摆锤从「静止下垂」开始，避免第一帧猛然落下。
    impl_->physics.Stabilize(impl_->physicsParams);
}

void Live2DModel::ApplyPhysics(float deltaTime) {
    if (!impl_ || !impl_->model) return;
    if (impl_->physics.Empty()) return;
    if (impl_->physicsParams.empty()) return;

    // 1) 从 Cubism 读入当前参数值（动作 / 视线 / 呼吸已写入的值）。
    for (auto& kv : impl_->physicsParams) {
        const auto it = impl_->physicsParamIndex.find(kv.first);
        if (it == impl_->physicsParamIndex.end()) continue;
        kv.second = cubism::ParameterValue(impl_->model, it->second);
    }

    // 2) 推进物理（摆锤解算），结果写回 params。
    impl_->physics.Evaluate(deltaTime, impl_->physicsParams);

    // 3) 写回 Cubism。
    //
    // 这里**叠加**而不是覆盖：官方 Cubism 的物理是覆盖输出参数，
    // 但那会抹掉动作已经写入的同名字段（例如模型自带的头发动画）。
    // 叠加后「动作姿势」与「物理惯性」可以共存，头发摇曳更自然。
    for (const auto& kv : impl_->physicsParams) {
        const auto it = impl_->physicsParamIndex.find(kv.first);
        if (it == impl_->physicsParamIndex.end()) continue;
        const int index = it->second;
        float minValue = -1.0f;
        float maxValue = 1.0f;
        const bool hasRange = cubism::ParameterRange(impl_->model, index, minValue, maxValue);
        float value = cubism::ParameterValue(impl_->model, index) + kv.second;
        if (hasRange) {
            value = Clamp(value, minValue, maxValue);
        }
        cubism::SetParameterValue(impl_->model, index, value);
    }
}

void Live2DModel::LoadTextures() {
    if (!impl_) return;

    // 只有在有 GL 上下文时才能创建纹理。
    // GLFW 没有「当前上下文」查询的跨平台 API，这里用 try-catch 之外的方式：
    // 加载失败不致命（用白色占位），因此直接尝试即可。
    for (const std::string& texturePath : impl_->manifest.texturePaths) {
        GLuint texture = LoadTexture(texturePath);
        if (!texture) {
            // 加载失败：用白色占位，保证模型仍可见。
            if (!impl_->whiteTexture) impl_->whiteTexture = CreateWhiteTexture();
            texture = impl_->whiteTexture;
        }
        impl_->textures.push_back(texture);
    }

    if (impl_->textures.empty() && !impl_->whiteTexture) {
        impl_->whiteTexture = CreateWhiteTexture();
    }
}

void Live2DModel::LoadMotions() {
    if (!impl_) return;

    impl_->motions.clear();
    impl_->motionNames.clear();
    impl_->motionGroups.clear();

    std::vector<MotionGroup> directorGroups;

    // 动作组：manifest.motionGroups 里已经解析出了绝对路径与组名。
    // 旧实现把组名丢掉、把所有动作混进一个扁平数组随机播放，导致
    // 「互动动作在待机时自己冒出来、点击也没有反应」。现在按组名分类，
    // 交给 MotionDirector 做 Idle 待机循环 + 互动优先级调度。
    //
    // 【空名组的处理】很多模型把全部动作放在 model3.json 的空名组 "" 下，
    // 组名不含任何语义。此时**必须按文件名**区分待机/互动：
    //   "motions/idle.motion3.json"      -> Idle（待机随机播）
    //   "motions/touch_head.motion3.json"-> Tap + 命中区 head
    // 若不处理，这类模型既没有 Idle 组（待机时一个动作都不播，表现为「完全不动」）
    // 也没有互动组（点击无反应）。
    for (const Live2DMotionGroup& group : impl_->manifest.motionGroups) {
        // 1) 解析本组所有动作文件，收集「动作 -> (用途, 命中区)」。
        struct Entry {
            int motionIndex = -1;
            MotionGroupKind kind = MotionGroupKind::Other;
            std::string hitArea;
            std::string fileName;
        };
        std::vector<Entry> entries;
        entries.reserve(group.files.size());

        const bool groupHasSemantics =
            !group.name.empty() && ClassifyMotionGroup(group.name) != MotionGroupKind::Other;

        for (std::size_t fi = 0; fi < group.files.size(); ++fi) {
            Motion motion;
            std::string error;
            if (!ParseMotionFile(group.files[fi], motion, error)) {
                // 单个动作文件损坏不应导致整个模型加载失败。
                std::fprintf(stderr, "[Live2D] %s\n", error.c_str());
                continue;
            }
            // fade 优先用 motion3.json 自带值；缺失时回退到 model3.json
            // 组条目上的 FadeInTime / FadeOutTime（Cubism 标准写法）。
            if (motion.fadeInTime <= 0.0f && fi < group.fadeInTimes.size() &&
                group.fadeInTimes[fi] >= 0.0f) {
                motion.fadeInTime = group.fadeInTimes[fi];
            }
            if (motion.fadeOutTime <= 0.0f && fi < group.fadeOutTimes.size() &&
                group.fadeOutTimes[fi] >= 0.0f) {
                motion.fadeOutTime = group.fadeOutTimes[fi];
            }

            Entry entry;
            entry.motionIndex = static_cast<int>(impl_->motions.size());
            entry.fileName = fi < group.fileNames.size() ? group.fileNames[fi]
                                                          : std::string();
            if (groupHasSemantics) {
                entry.kind = ClassifyMotionGroup(group.name);
                entry.hitArea = HitAreaFromGroupName(group.name, entry.kind);
            } else {
                entry.kind = ClassifyMotionFile(entry.fileName);
                entry.hitArea = HitAreaFromMotionFile(entry.fileName);
            }
            entries.push_back(entry);
            impl_->motions.push_back(std::move(motion));
            impl_->motionNames.push_back(
                entry.fileName.empty() ? ("motion#" + std::to_string(entry.motionIndex))
                                       : entry.fileName);
        }
        if (entries.empty()) continue;

        // 2) 有语义的组名：整组作为同一个 MotionGroup（保持原行为）。
        if (groupHasSemantics) {
            MotionGroup dg;
            dg.name = group.name;
            dg.kind = ClassifyMotionGroup(group.name);
            dg.hitArea = HitAreaFromGroupName(group.name, dg.kind);
            for (const Entry& entry : entries) {
                dg.motionIndices.push_back(entry.motionIndex);
            }
            impl_->motionGroups.push_back(dg.motionIndices);
            directorGroups.push_back(std::move(dg));
            continue;
        }

        // 3) 空名组 / 无语义组名：按**推断出的用途**再分成若干子组。
        //    同一用途的动作合并成一个组（例如所有 touch_* 归为互动组），
        //    这样 MotionDirector 才能分别对待「待机循环」与「点击互动」。
        //    分组顺序按首次出现顺序，保证行为可复现。
        std::vector<std::string> bucketKeys;
        std::vector<MotionGroup> buckets;
        for (const Entry& entry : entries) {
            // 子组键：用途 + 命中区。命中区不同则分成不同组，
            // 这样点击头部能命中 touch_head，点击身体能命中 touch_body。
            std::string key = std::to_string(static_cast<int>(entry.kind));
            if (!entry.hitArea.empty()) key += "|" + entry.hitArea;

            std::size_t bucketIndex = bucketKeys.size();
            for (std::size_t i = 0; i < bucketKeys.size(); ++i) {
                if (bucketKeys[i] == key) { bucketIndex = i; break; }
            }
            if (bucketIndex == bucketKeys.size()) {
                bucketKeys.push_back(key);
                MotionGroup bucket;
                bucket.name = entry.fileName;
                bucket.kind = entry.kind;
                bucket.hitArea = entry.hitArea;
                buckets.push_back(std::move(bucket));
            }
            buckets[bucketIndex].motionIndices.push_back(entry.motionIndex);
        }
        for (MotionGroup& bucket : buckets) {
            if (bucket.motionIndices.empty()) continue;
            impl_->motionGroups.push_back(bucket.motionIndices);
            directorGroups.push_back(std::move(bucket));
        }
    }

    // 顶层 Animations 也作为独立动作组加入（名字含 exp 的表情组会被识别为点缀）。
    for (const Live2DAnimation& animation : impl_->manifest.animations) {
        Motion motion;
        std::string error;
        if (!ParseMotionFile(animation.file, motion, error)) {
            std::fprintf(stderr, "[Live2D] %s\n", error.c_str());
            continue;
        }
        MotionGroup dg;
        dg.name = animation.name;
        dg.kind = ClassifyMotionGroup(animation.name);
        dg.hitArea = HitAreaFromGroupName(animation.name, dg.kind);
        dg.motionIndices.push_back(static_cast<int>(impl_->motions.size()));
        impl_->motions.push_back(std::move(motion));
        impl_->motionNames.push_back(animation.name.empty()
                                         ? ("motion#" + std::to_string(impl_->motions.size() - 1))
                                         : animation.name);
        impl_->motionGroups.push_back(dg.motionIndices);
        directorGroups.push_back(std::move(dg));
    }

    // 命中区名字（model3.json 的 HitAreas[].Name），用于把点击分区映射到互动组。
    std::vector<std::string> hitAreaNames;
    for (const Live2DHitArea& area : impl_->manifest.hitAreas) {
        if (!area.name.empty()) hitAreaNames.push_back(area.name);
    }

    // 动作元数据（时长 / 淡入淡出）与全局 motions 下标一一对应。
    std::vector<float> durations, fadeIns, fadeOuts;
    std::vector<bool> loops;
    durations.reserve(impl_->motions.size());
    fadeIns.reserve(impl_->motions.size());
    fadeOuts.reserve(impl_->motions.size());
    loops.reserve(impl_->motions.size());
    for (const Motion& m : impl_->motions) {
        durations.push_back(m.duration);
        fadeIns.push_back(m.fadeInTime);
        fadeOuts.push_back(m.fadeOutTime);
        loops.push_back(m.loop);
    }

    motionDirector_.Configure(std::move(directorGroups), std::move(hitAreaNames),
                              impl_->motions.size());
    motionDirector_.SetMotionMeta(durations, fadeIns, fadeOuts, loops);

    // 分组摘要：便于在日志里确认「待机组是否识别成功」。
    {
        std::string summary;
        int idleGroups = 0;
        int interactionGroups = 0;
        int expressionGroups = 0;
        for (const MotionGroup& g : motionDirector_.Groups()) {
            if (!summary.empty()) summary += ", ";
            summary += g.name + "[" + MotionGroupKindName(g.kind) + "]×" +
                       std::to_string(g.motionIndices.size());
            if (g.kind == MotionGroupKind::Idle) ++idleGroups;
            if (g.IsInteraction()) ++interactionGroups;
            if (g.kind == MotionGroupKind::Expression) ++expressionGroups;
        }
        PPET_LOG_TAG("LIVE2D", "动作分组：%s（待机组 %d / 互动组 %d / 表情组 %d）",
                     summary.c_str(), idleGroups, interactionGroups, expressionGroups);
    }
}

void Live2DModel::ResolveFaceParameters() {
    faceParamIndex_.clear();
    // 每次（重新）加载模型都把表情状态机拉回中性。否则上一次加载里手动
    // 触发、还停在 Hold 的表情会渗进新状态，导致「口型/动作」对比图里
    // 混着上一个表情，验证结论失真（实测由 ppet_feature_verify 捕获）。
    idleFaces_.Reset();

    // Live2D 表情参数的事实标准命名（Cubism 官方模板 / 绝大多数模型都遵循）。
    // 这里列出「参数名 -> 表情预设里的目标值」，只保留模型真实拥有的那些。
    struct FaceParamDef {
        const char* name;
        float value;
    };
    // 眉毛：上挑 = 开心/惊讶；Form = 皱眉（生气/困惑）。
    static const FaceParamDef kBrowDefs[] = {
        {"ParamBrowLY", 1.0f}, {"ParamBrowRY", 1.0f},
        {"ParamBrowLForm", -1.0f}, {"ParamBrowRForm", -1.0f},
        {"ParamBrowLAngle", 0.6f}, {"ParamBrowRAngle", 0.6f},
    };
    // 脸颊：泛红（害羞/开心）。
    static const FaceParamDef kCheekDefs[] = {
        {"ParamCheek", 1.0f}, {"ParamCheekL", 1.0f}, {"ParamCheekR", 1.0f},
    };
    // 笑眼：眯眼笑。
    static const FaceParamDef kSmileEyeDefs[] = {
        {"ParamEyeLSmile", 1.0f}, {"ParamEyeRSmile", 1.0f},
    };
    // 嘴形：嘴角上扬。
    static const FaceParamDef kMouthDefs[] = {
        {"ParamMouthForm", 1.0f}, {"ParamMouthFormEnd", 0.5f},
    };

    auto registerParam = [&](const FaceParamDef& def) {
        const int index = cubism::FindParameter(impl_->model, def.name);
        if (index < 0) return false;
        faceParamIndex_[def.name] = index;
        return true;
    };

    // 记录每个参数的目标值，供预设生成使用。
    std::map<std::string, float> paramTargets;
    for (const FaceParamDef& def : kBrowDefs) {
        if (registerParam(def)) paramTargets[def.name] = def.value;
    }
    for (const FaceParamDef& def : kCheekDefs) {
        if (registerParam(def)) paramTargets[def.name] = def.value;
    }
    for (const FaceParamDef& def : kSmileEyeDefs) {
        if (registerParam(def)) paramTargets[def.name] = def.value;
    }
    for (const FaceParamDef& def : kMouthDefs) {
        if (registerParam(def)) paramTargets[def.name] = def.value;
    }

    if (paramTargets.empty()) {
        PPET_LOG_TAG("LIVE2D", "模型没有表情参数，待机小表情不可用");
        idleFaces_.SetPresets({});
        return;
    }

    // --- 按「模型实际拥有的参数」组装表情预设 ---
    //
    // 预设是「参数子集」的组合：模型缺哪个参数，那个预设就少一项，
    // 不会出现「设了一个不存在的参数所以整张脸没变化」的情况。
    auto pick = [&](std::initializer_list<const char*> names) {
        std::vector<std::pair<std::string, float>> out;
        for (const char* n : names) {
            const auto it = paramTargets.find(n);
            if (it != paramTargets.end()) out.emplace_back(it->first, it->second);
        }
        return out;
    };

    std::vector<FacePreset> presets;
    {
        FacePreset p;
        p.id = "happy";
        p.name = "开心";
        p.params = pick({"ParamBrowLY", "ParamBrowRY", "ParamEyeLSmile", "ParamEyeRSmile",
                          "ParamMouthForm"});
        if (!p.params.empty()) presets.push_back(std::move(p));
    }
    {
        FacePreset p;
        p.id = "shy";
        p.name = "害羞";
        p.params = pick({"ParamCheek", "ParamCheekL", "ParamCheekR", "ParamBrowLY",
                          "ParamBrowRY"});
        if (!p.params.empty()) presets.push_back(std::move(p));
    }
    {
        FacePreset p;
        p.id = "angry";
        p.name = "生气";
        p.params = pick({"ParamBrowLForm", "ParamBrowRForm", "ParamBrowLAngle",
                          "ParamBrowRAngle"});
        if (!p.params.empty()) presets.push_back(std::move(p));
    }
    {
        FacePreset p;
        p.id = "surprised";
        p.name = "惊讶";
        p.params = pick({"ParamBrowLY", "ParamBrowRY"});
        if (!p.params.empty()) presets.push_back(std::move(p));
    }

    idleFaces_.SetPresets(std::move(presets));
    idleFaces_.SetEnabled(randomExpression_);
    PPET_LOG_TAG("LIVE2D", "已探测到 %zu 个表情参数，生成 %zu 个待机小表情预设",
                 paramTargets.size(), idleFaces_.Presets().size());
}

void Live2DModel::ApplyIdleFaces(float deltaTime) {
    if (!impl_ || !impl_->model) return;
    if (faceParamIndex_.empty()) return;

    const std::vector<IdleFaces::Face> faces = idleFaces_.Tick(deltaTime);
    for (const IdleFaces::Face& face : faces) {
        const auto it = faceParamIndex_.find(face.name);
        if (it == faceParamIndex_.end() || it->second < 0) continue;
        const int index = it->second;
        // 叠加而不是覆盖：模型自己的待机动作（如眉毛动画）必须保留。
        float value = cubism::ParameterValue(impl_->model, index) + face.weight;
        float minValue = -1.0f;
        float maxValue = 1.0f;
        if (cubism::ParameterRange(impl_->model, index, minValue, maxValue)) {
            value = Clamp(value, minValue, maxValue);
        } else {
            value = Clamp(value, -100.0f, 100.0f);
        }
        cubism::SetParameterValue(impl_->model, index, value);
    }
}

void Live2DModel::ResolveTrackingParameters() {
    paramAngleX_ = cubism::FindParameter(impl_->model, kParamAngleX);
    paramAngleY_ = cubism::FindParameter(impl_->model, kParamAngleY);
    paramEyeBallX_ = cubism::FindParameter(impl_->model, kParamEyeBallX);
    paramEyeBallY_ = cubism::FindParameter(impl_->model, kParamEyeBallY);
    paramBodyAngleX_ = cubism::FindParameter(impl_->model, kParamBodyAngleX);
    paramBodyAngleZ_ = cubism::FindParameter(impl_->model, kParamBodyAngleZ);
    paramEyeLOpen_ = cubism::FindParameter(impl_->model, kParamEyeLOpen);
    paramEyeROpen_ = cubism::FindParameter(impl_->model, kParamEyeROpen);
    paramBreath_ = cubism::FindParameter(impl_->model, kParamBreath);
    paramMouthOpenY_ = cubism::FindParameter(impl_->model, kParamMouthOpenY);
    paramMouthForm_ = cubism::FindParameter(impl_->model, kParamMouthForm);
}

// ---------------------------------------------------------------------------
// 更新
// ---------------------------------------------------------------------------

void Live2DModel::Update(const RenderContext& ctx) {
    if (!loaded_ || !impl_ || !impl_->model) return;

    const float dt = std::max(0.0f, ctx.deltaTime);

    // 每帧先复位到默认姿态，再依次叠加各层。否则动作结束后它写过的参数
    // 会一直停留在最后一帧（表情/手臂「卡住」），也不会随淡出平滑回落。
    cubism::ResetParametersToDefault(impl_->model);

    // 【顺序很重要】
    //   1) 眨眼 / 呼吸：模型自带的基础动作
    //   2) 动作（motion3.json）：模型的待机/互动动画
    //   3) 视线追踪：**必须放在动作之后**
    //   4) 物理（physics3.json）：头发/裙摆的惯性摆动
    //   5) 口型
    //
    // 视线追踪放在动作之前会被动作覆盖：几乎所有 Live2D 模型的待机动作
    // 都会动画 ParamAngleX / ParamAngleY（模型自己在摇头晃脑），
    // 于是「模型一直看鼠标」完全看不到。放在动作之后并采用**叠加**方式，
    // 就能做到「模型照常做自己的动作，同时眼睛跟着鼠标看」。
    //
    // 物理放在视线之后：物理的输入通常是头部/身体角度参数
    // （ParamAngleX / ParamBodyAngleX），必须等视线追踪把角度写进去，
    // 物理才能算出「头转到哪，头发就甩到哪」。
    ApplyEyeBlink(dt);
    ApplyBreathing(dt);
    UpdateMotion(dt);
    ApplyTracking(ctx, dt);
    ApplyIdleFaces(dt);
    ApplyPhysics(dt);
    ApplyMouth(dt);

    // 依据参数重算顶点。
    cubism::UpdateModel(impl_->model);
}

void Live2DModel::ApplyTracking(const RenderContext& ctx, float deltaTime) {
    if (!impl_ || !impl_->model) return;

    // 三通道视线：眼/头/身体各自以不同速度指数阻尼追向鼠标方向，
    // 且「回中」比「偏离」更快（参考 FULASKA 的 GazeChannels）。
    float targetX = 0.0f;
    float targetY = 0.0f;
    // 全局光标优先；不可用时回退到窗口内光标。
    // 没有这一步，在「未开启全屏追踪」或「平台不支持全局钩子」的环境里
    // 目标恒为 0，模型就完全不会看向鼠标——用户反馈的「眼睛追踪没有」。
    if (ctx.HasCursorTarget()) {
        targetX = Clamp(ctx.localCursor.x, -1.0f, 1.0f);
        targetY = Clamp(ctx.localCursor.y, -1.0f, 1.0f);
    }
    const GazeChannels::Channels g = gaze_.Tick(deltaTime, targetX, targetY);

    // 待机微摆：常驻小幅晃动，叠加在视线基线之上（边说话边晃会被压低）。
    const IdleDirector::Sway sway = idleStage_.Tick(deltaTime, talkOpen_);

    // 追踪强度倍率（设置项）。0 = 完全不跟，1 = 默认，2 = 双倍。
    const float angle = maxTrackingAngle_ * trackingStrength_;

    // 叠加写入：在当前参数值（可能已被动作写过）之上加追踪量。
    auto add = [&](int index, float offset) {
        if (index < 0) return;
        float minValue = -1.0f;
        float maxValue = 1.0f;
        const bool hasRange = cubism::ParameterRange(impl_->model, index, minValue, maxValue);
        float value = cubism::ParameterValue(impl_->model, index) + offset;
        if (hasRange) {
            value = Clamp(value, minValue, maxValue);
        } else {
            value = Clamp(value, -100.0f, 100.0f);
        }
        cubism::SetParameterValue(impl_->model, index, value);
    };

    // 头部：追踪量与待机微摆叠加（模型自己的摇头动作保留）。
    add(paramAngleX_, g.headX * angle + sway.headYaw);
    add(paramAngleY_, g.headY * angle + sway.headPitch);
    // 眼球：始终跟随鼠标，这是「眼睛追踪」最直观的体现。
    //
    // 眼球参数与头部角度参数的**量纲不同**：
    //   · ParamAngleX 是「度」（典型范围 -30..30）；
    //   · ParamEyeBallX 是「归一化方向」（典型范围 -1..1）。
    // 早期实现对两者用同一个 angle，导致眼球被写到 ±30 再被钳到 ±1，
    // 眼睛看起来是「一直斜着看一个角落」而不是跟着鼠标转。
    // 这里按参数自身的范围换算，保证眼球扫过完整的可视角度。
    auto addNormalized = [&](int index, float normalized) {
        if (index < 0) return;
        float minValue = -1.0f;
        float maxValue = 1.0f;
        const bool hasRange = cubism::ParameterRange(impl_->model, index, minValue, maxValue);
        if (!hasRange) {
            add(index, normalized);
            return;
        }
        // 把 [-1,1] 的方向映射到参数自身的 [min,max]。
        const float span = (maxValue - minValue) * 0.5f;
        const float center = (maxValue + minValue) * 0.5f;
        float value = cubism::ParameterValue(impl_->model, index) + center + normalized * span;
        value = Clamp(value, minValue, maxValue);
        cubism::SetParameterValue(impl_->model, index, value);
    };
    addNormalized(paramEyeBallX_, g.eyeX);
    addNormalized(paramEyeBallY_, g.eyeY);
    // 身体：幅度最小，只做轻微跟随。
    add(paramBodyAngleX_, g.bodyX * angle);
    add(paramBodyAngleZ_, g.bodyX * angle);
}

void Live2DModel::ApplyEyeBlink(float deltaTime) {
    if (!impl_ || !impl_->model) return;
    if (paramEyeLOpen_ < 0 && paramEyeROpen_ < 0) return;

    blinkTimer_ -= deltaTime;
    if (blinkTimer_ <= 0.0f) {
        // 开始一次眨眼。
        blinking_ = true;
        blinkPhase_ = 0.0f;
        // 下次眨眼的等待时间：2.5 ~ 6.5 秒随机。
        std::uniform_real_distribution<float> dist(2.5f, 6.5f);
        blinkTimer_ = dist(rng_);
    }

    float openValue = 1.0f;
    if (blinking_) {
        // 一次眨眼约 0.2 秒：0.08 秒闭合，0.06 秒保持，0.06 秒睁开。
        blinkPhase_ += deltaTime;
        if (blinkPhase_ < 0.08f) {
            openValue = 1.0f - blinkPhase_ / 0.08f;
        } else if (blinkPhase_ < 0.14f) {
            openValue = 0.0f;
        } else if (blinkPhase_ < 0.20f) {
            openValue = (blinkPhase_ - 0.14f) / 0.06f;
        } else {
            openValue = 1.0f;
            blinking_ = false;
        }
        openValue = Clamp(openValue, 0.0f, 1.0f);
    }

    if (paramEyeLOpen_ >= 0) {
        cubism::SetParameterValue(impl_->model, paramEyeLOpen_, openValue);
    }
    if (paramEyeROpen_ >= 0) {
        cubism::SetParameterValue(impl_->model, paramEyeROpen_, openValue);
    }
}

void Live2DModel::ApplyBreathing(float deltaTime) {
    if (!impl_ || !impl_->model) return;

    // 呼吸：用一个缓慢的正弦驱动模型起伏。
    // 优先使用模型自带的 ParamBreath 参数；没有的话就给 ParamAngleY 加一个基线偏移。
    breathPhase_ += deltaTime * 1.6f;
    if (breathPhase_ > 2.0f * kPi) breathPhase_ -= 2.0f * kPi;

    const float breath = std::sin(breathPhase_);

    if (paramBreath_ >= 0) {
        float minValue = -1.0f;
        float maxValue = 1.0f;
        float value = breath;
        if (cubism::ParameterRange(impl_->model, paramBreath_, minValue, maxValue)) {
            value = Clamp(value, minValue, maxValue);
        }
        cubism::SetParameterValue(impl_->model, paramBreath_, value);
        return;
    }

    // 没有 ParamBreath 时，把呼吸叠到 ParamAngleY 上。
    // 注意：这里叠加的是「基线」，视线追踪每帧会重写 ParamAngleY，
    // 因此需要在视线追踪之后、呼吸之前写入，
    // 并且写入的是「视线角度 + 呼吸偏移」。
    if (paramAngleY_ >= 0) {
        const float current = cubism::ParameterValue(impl_->model, paramAngleY_);
        float minValue = -1.0f;
        float maxValue = 1.0f;
        float value = current + breath * 1.5f;
        if (cubism::ParameterRange(impl_->model, paramAngleY_, minValue, maxValue)) {
            value = Clamp(value, minValue, maxValue);
        }
        cubism::SetParameterValue(impl_->model, paramAngleY_, value);
    }
}

void Live2DModel::UpdateMotion(float deltaTime) {
    if (!impl_ || !impl_->model) return;

    // MotionDirector 负责「播放哪个动作、优先级、淡入淡出权重」：
    //   - 空闲时按冷却随机播放 Idle 组动作（待机循环）；
    //   - 用户点击时按命中区插入互动动作（可打断待机）；
    //   - 动作结束回落到待机。
    motionDirector_.Tick(deltaTime);

    // --- 待机随机表情 ---
    //
    // Live2D 的「表情」是表情类动作组，所以随机表情 = 随机播一个表情动作。
    // 播完停留几秒后由 MotionDirector 自己淡回待机循环。
    if (randomExpression_) {
        if (randomExpressionHold_ >= 0.0f) {
            randomExpressionHold_ -= deltaTime;
            if (randomExpressionHold_ <= 0.0f) randomExpressionHold_ = -1.0f;
        } else {
            randomExpressionTimer_ -= deltaTime;
            if (randomExpressionTimer_ <= 0.0f) {
                TriggerRandomExpression();
            }
        }
    }

    // 【官方 Framework 风格】队列返回本帧所有生效动作（含正在淡出的旧动作），
    // 按入队顺序依次采样曲线并叠加；后来的动作按 easing 权重覆盖前面的参数，
    // 因此动作切换是交叉淡入淡出，不会瞬间跳变。
    //
    // 权威依据：Framework/src/Motion/CubismMotionQueueManager.cpp::DoUpdateMotion
    //           与 CubismMotion.cpp::DoUpdateParameters 的
    //               v = sourceValue + (value - sourceValue) * paramWeight
    const std::vector<QueueBlend> blends = motionDirector_.Blends();
    for (const QueueBlend& blend : blends) {
        if (blend.motionIndex < 0 || blend.weight <= 0.0f) continue;
        if (blend.motionIndex >= static_cast<int>(impl_->motions.size())) continue;

        const Motion& motion = impl_->motions[static_cast<std::size_t>(blend.motionIndex)];
        std::vector<float> values;
        motion.Sample(blend.time, values);

        for (std::size_t i = 0; i < motion.curves.size() && i < values.size(); ++i) {
            const MotionCurve& curve = motion.curves[i];
            // motion3.json 的曲线还可作用于部件不透明度（Target="PartOpacity"），
            // 本渲染层未建立部件索引映射，暂时只处理 Parameter 曲线。
            if (curve.target != "Parameter") continue;
            const int index = cubism::FindParameter(impl_->model, curve.id);
            if (index < 0) continue;

            // 基线是**上一个动作写完之后**的当前值，因此多个动作在队列里自然叠加。
            const float base = cubism::ParameterValue(impl_->model, index);
            float value = base + (values[i] - base) * blend.weight;
            float minValue = -1.0f;
            float maxValue = 1.0f;
            if (cubism::ParameterRange(impl_->model, index, minValue, maxValue)) {
                value = Clamp(value, minValue, maxValue);
            }
            cubism::SetParameterValue(impl_->model, index, value);
        }
    }
}

bool Live2DModel::OnInteract(float nx, float ny) {
    if (!loaded_) return false;
    return motionDirector_.Interact(nx, ny);
}

std::vector<std::string> Live2DModel::MotionNames() const {
    std::vector<std::string> out;
    if (!impl_) return out;
    out.reserve(impl_->motionNames.size());
    // 同名动作加 #序号 后缀，保证每个名字都能被 action do 精确命中。
    std::map<std::string, int> seen;
    for (std::size_t i = 0; i < impl_->motionNames.size(); ++i) {
        std::string name = impl_->motionNames[i];
        if (name.empty()) name = "motion#" + std::to_string(i);
        const int nth = seen[name]++;
        if (nth > 0) name += "#" + std::to_string(nth);
        out.push_back(std::move(name));
    }
    return out;
}

bool Live2DModel::PlayMotion(const std::string& id) {
    if (!loaded_ || !impl_) return false;
    // 1) 精确匹配动作显示名（action list 里列出的名字）。
    const std::vector<std::string> names = MotionNames();
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (names[i] == id) {
            return motionDirector_.PlayMotionIndex(static_cast<int>(i), MotionPriority::Force,
                                                   names[i]);
        }
    }
    // 2) 按动作组名触发（组里多个动作时随机挑一个）。
    if (motionDirector_.PlayGroup(id, MotionPriority::Force)) return true;
    // 3) 宽松匹配：名字包含输入串即可（方便敲一半）。
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (names[i].find(id) != std::string::npos) {
            return motionDirector_.PlayMotionIndex(static_cast<int>(i), MotionPriority::Force,
                                                   names[i]);
        }
    }
    return false;
}

bool Live2DModel::StopMotion() {
    if (!loaded_) return false;
    motionDirector_.StopAll();
    return true;
}

bool Live2DModel::PlayRandomIdle() {
    // 需求 2：随机起一个待机动作。MotionDirector 内部按 idle 组随机挑、带冷却，
    // 因此「动一下停一下」的节奏由它把关，这里只负责触发。
    if (!loaded_) return false;
    return motionDirector_.PlayRandomIdle();
}

bool Live2DModel::TriggerTouch(const std::string& region) {
    // 需求 2：分区触摸。把区域换算成归一化坐标后交给 MotionDirector::Interact，
    // 由其按「命中区 -> 互动组」匹配（head/body/leg/left/right 与模型 HitArea 双向包含）。
    if (!loaded_) return false;
    const Vec2 p = NormalizedFromRegion(region);
    return motionDirector_.Interact(p.x, p.y);
}

void Live2DModel::ReloadRenderer() {
    if (!loaded_ || !impl_) return;
    // 释放全部 GPU 资源后重建纹理；program / VAO / 遮罩关系由下一帧
    // Render() 里的惰性重建路径自动恢复（官方 Framework 也是这个思路：
    // 渲染器与模型数据分离，重建渲染器不需要重新解析 moc3）。
    ReleaseGraphics();
    LoadTextures();
    impl_->masksResolved = false;
}

float Live2DModel::PreferredFrameAspect() const {
    if (!loaded_ || !impl_) return 0.0f;
    // 【需求 3】优先用「真实内容包围盒」的宽高比，而不是画布尺寸。
    // 画布 sizeInPixels 是导出画板，常留大量透明边；用户说的「真实像素
    // 2000x600 / 框 600x600 / 横向被截断」正是拿画布当取景框造成的。
    // 内容帧由 Load() 遍历可见部件顶点算出，反映实际出图范围。
    const float contentAspect = impl_->contentFrame.Aspect();
    if (contentAspect > 0.05f && contentAspect < 20.0f) return contentAspect;
    // 回退：模型异常（没有可见顶点）时仍用画布比例，保证不返回 0。
    const float w = impl_->canvas.sizeInPixels.x;
    const float h = impl_->canvas.sizeInPixels.y;
    if (w <= 1.0f || h <= 1.0f) return 0.0f;
    const float aspect = w / h;
    return (aspect > 0.05f && aspect < 20.0f) ? aspect : 0.0f;
}

std::vector<std::pair<std::string, std::string>> Live2DModel::RenderResources() const {
    std::vector<std::pair<std::string, std::string>> out;
    if (!impl_) return out;
    if (!impl_->manifest.mocPath.empty()) {
        out.emplace_back("moc3", impl_->manifest.mocPath);
    }
    for (const std::string& path : impl_->manifest.texturePaths) {
        out.emplace_back("texture", path);
    }
    if (!impl_->manifest.physicsPath.empty()) {
        out.emplace_back("physics3", impl_->manifest.physicsPath);
    }
    out.emplace_back("motions", std::to_string(impl_->motions.size()) + " 条动作");
    return out;
}

void Live2DModel::ApplyMouth(float deltaTime) {
    if (!impl_ || !impl_->model) return;
    if (paramMouthOpenY_ < 0 && paramMouthForm_ < 0) return;

    // 平滑追向「说话开口度」，并叠加呼吸微动（参考 FULASKA 的 MouthDirector）。
    const std::pair<float, float> out = mouthStage_.Tick(deltaTime, talkOpen_);
    auto set = [&](int index, float value) {
        if (index < 0) return;
        float minValue = -1.0f;
        float maxValue = 1.0f;
        if (cubism::ParameterRange(impl_->model, index, minValue, maxValue)) {
            value = Clamp(value, minValue, maxValue);
        }
        cubism::SetParameterValue(impl_->model, index, value);
    };
    set(paramMouthOpenY_, out.first);
    set(paramMouthForm_, out.second);
}

bool Live2DModel::SetLipSync(float open, float form) {
    talkOpen_ = std::clamp(open, 0.0f, 1.0f);
    mouthStage_.SetForm(form);
    return true;
}

bool Live2DModel::SetExpressionPreset(const std::string& id) {
    // 通道 1：模型自带的表情类动作组（如 ExpHappy），效果最好，优先播放。
    if (motionDirector_.PlayGroup(id, MotionPriority::Expression)) return true;

    // 通道 2：参数级表情预设回退。
    // 大量模型（含实测的 nengdai_2）只有 ParamBrow* / ParamEye*Smile 等表情参数、
    // 完全没有表情动作组。此时必须走 IdleFaces 的参数预设，否则手动切换表情与
    // 控制台 expression <id> 对这些模型一律返回 false、画面毫无变化。
    // IdleFaces 允许「手动触发」独立于「待机随机表情」开关播放，因此这里
    // 即使随机表情被用户关掉，手动切换依然能生效。
    return idleFaces_.TriggerId(id);
}

void Live2DModel::SetRandomIdleExpression(bool enabled) {
    randomExpression_ = enabled;
    randomExpressionTimer_ = RandomExpressionWait();
    randomExpressionHold_ = -1.0f;
    // 参数级小表情同步开关。
    idleFaces_.SetEnabled(enabled);
}

void Live2DModel::SetTrackingStrength(float strength) {
    trackingStrength_ = Clamp(strength, 0.0f, 3.0f);
}

void Live2DModel::SetMaxTrackingAngle(float degrees) {
    // 与 AppConfig 的合法范围一致（0..90）。越界值钳制，避免异常配置把模型拧到
    // 不自然的姿势。需求 7 修复：此前没有这个方法，配置里的最大偏转角度永远是空谈。
    maxTrackingAngle_ = Clamp(degrees, 0.0f, 90.0f);
}

bool Live2DModel::RandomIdleExpression() const {
    return randomExpression_;
}

bool Live2DModel::TriggerRandomExpression() {
    // 优先用「表情类动作组」——模型的原生表情，效果最好。
    std::vector<std::string> ids = ExpressionPresetIds();
    if (!ids.empty()) {
        std::uniform_int_distribution<std::size_t> dist(0, ids.size() - 1);
        const std::string& id = ids[dist(expressionRng_)];
        if (motionDirector_.PlayGroup(id, MotionPriority::Expression)) {
            std::uniform_real_distribution<float> hold(2.0f, 5.0f);
            randomExpressionHold_ = hold(expressionRng_);
            randomExpressionTimer_ = RandomExpressionWait();
            return true;
        }
    }
    // 回退：模型没有表情动作组时，用表情参数做小表情。
    // 很多模型（包括本项目实测的国内模型包）只有表情参数、没有 exp 动作组，
    // 不做这个回退，随机表情对这些模型就是完全无效的。
    if (idleFaces_.Trigger()) {
        randomExpressionHold_ = -1.0f;
        randomExpressionTimer_ = RandomExpressionWait();
        return true;
    }
    return false;
}

float Live2DModel::RandomExpressionWait() {
    std::uniform_real_distribution<float> dist(8.0f, 20.0f);
    return dist(expressionRng_);
}

std::vector<std::string> Live2DModel::ExpressionPresetIds() const {
    // Live2D 表情有两条通道，都要列出来：
    //   1) 表情类动作组（MotionGroupKind::Expression）；
    //   2) 参数级小表情预设（IdleFaces，按模型实际拥有的表情参数生成）。
    // 只列通道 1 会让「只有表情参数、没有 exp 动作组」的模型在控制台
    // expression list 里显示为空，用户根本无从操作表情。
    std::vector<std::string> ids;
    for (const MotionGroup& group : motionDirector_.Groups()) {
        if (group.kind == MotionGroupKind::Expression && !group.name.empty()) {
            ids.push_back(group.name);
        }
    }
    for (const std::string& id : idleFaces_.PresetIds()) {
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
    }
    return ids;
}

// ---------------------------------------------------------------------------
// 绘制
// ---------------------------------------------------------------------------

void Live2DModel::OnResize(int width, int height) {
    // Live2D 按画布比例自适应，不需要重建任何资源。
    (void)width;
    (void)height;
}

Mat4 Live2DModel::BuildTransform(const RenderContext& ctx) const {
    const float vw = ctx.viewportWidth > 0 ? static_cast<float>(ctx.viewportWidth) : 1.0f;
    const float vh = ctx.viewportHeight > 0 ? static_cast<float>(ctx.viewportHeight) : 1.0f;

    // 【需求 3】用真实内容帧做 contain fit + 居中。相较旧的「画布 fit」：
    //   - 画布常含大量透明留白，按画布 fit 会把角色缩得很小；
    //   - 旧实现假设顶点以原点为中心（平移=0），偏心导出的模型会偏出画面；
    //   - contain fit 取较小边铺满，任何窗口比例下内容都完整不截断。
    float fitMatrix[16];
    if (ppet::live2d::ComputeFitMatrix(impl_->contentFrame, vw, vh,
                                       ctx.renderScale, 0.95f, fitMatrix)) {
        Mat4 m;
        for (int i = 0; i < 16; ++i) m.m[i] = fitMatrix[i];
        return m;
    }

    // 回退：内容帧不可用时（异常模型没有可见顶点）用画布 fit。
    // 单位换算必须除以 pixelsPerUnit，否则模型会被缩小上百倍。
    const cubism::CanvasInfo& canvas = impl_->canvas;
    const float ppu = canvas.pixelsPerUnit > 0.0f ? canvas.pixelsPerUnit : 1.0f;
    const float W = canvas.sizeInPixels.x / ppu;
    const float H = canvas.sizeInPixels.y / ppu;
    const float k = std::min(vw / (W > 0.0f ? W : 1.0f),
                              vh / (H > 0.0f ? H : 1.0f)) * 0.95f * ctx.renderScale;
    Mat4 m = Mat4::Identity();
    m.At(0, 0) = k / (vw * 0.5f);
    m.At(1, 1) = k / (vh * 0.5f);
    return m;
}

void Live2DModel::Render(const RenderContext& ctx) {
    if (!loaded_ || !impl_ || !impl_->model) return;

    // --- 准备着色器 ---
    if (!impl_->program) {
        impl_->program = CompileProgram(kVertexShader, kFragmentShader);
        if (!impl_->program) return;
        impl_->uniformMatrix = glGetUniformLocation(impl_->program, "u_matrix");
        impl_->uniformTexture = glGetUniformLocation(impl_->program, "s_texture0");
        impl_->uniformBaseColor = glGetUniformLocation(impl_->program, "u_baseColor");
        impl_->uniformMask = glGetUniformLocation(impl_->program, "s_mask");
        impl_->uniformUseMask = glGetUniformLocation(impl_->program, "u_useMask");
        impl_->uniformInvertMask = glGetUniformLocation(impl_->program, "u_invertMask");
        impl_->uniformMaskPass = glGetUniformLocation(impl_->program, "u_maskPass");
    }

    // --- 准备部件缓冲 ---
    const int drawableCount = cubism::DrawableCount(impl_->model);
    if (drawableCount != impl_->drawableCount ||
        impl_->buffers.size() != static_cast<std::size_t>(drawableCount)) {
        // 部件数量变了：重建缓冲。
        for (auto& buffer : impl_->buffers) {
            if (buffer.vao) glDeleteVertexArrays(1, &buffer.vao);
            if (buffer.vbo) glDeleteBuffers(1, &buffer.vbo);
            if (buffer.ebo) glDeleteBuffers(1, &buffer.ebo);
        }
        impl_->buffers.clear();
        impl_->buffers.resize(static_cast<std::size_t>(std::max(0, drawableCount)));
        impl_->drawableCount = drawableCount;
        impl_->masksResolved = false;
    }

    // --- 解析遮罩关系（模型结构不变时只解析一次）---
    if (!impl_->masksResolved) {
        ResolveMaskRelations(drawableCount);
    }

    const Mat4 transform = BuildTransform(ctx);

    // --- GL 状态 ---
    glUseProgram(impl_->program);
    glUniformMatrix4fv(impl_->uniformMatrix, 1, GL_FALSE, transform.m);
    glUniform1i(impl_->uniformTexture, 0);
    glUniform1i(impl_->uniformMask, 1);
    glUniform1f(impl_->uniformMaskPass, 0.0f);
    glActiveTexture(GL_TEXTURE0);

    // 桌宠窗口是透明叠加窗口：premultiplied alpha 混合。
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    // --- 官方 Framework 风格的渲染决策（纯逻辑，可单测）---
    //
    // 决策与执行分离：CubismRenderPlan 只产出「画哪些部件、按什么顺序、
    // 用什么混合/premultiplied 颜色」，本循环只负责把指令翻译成 GL 调用。
    // 这样「渲染顺序错」「RGB 忘了乘 opacity」「加法混合 Alpha 累加」这类
    // 问题都能在纯逻辑单测里被抓住，而不必依赖截图肉眼比对。
    const std::vector<DrawableDesc> descs = CollectDrawableDescs(
        impl_->model, drawableCount, impl_->masks, impl_->isMaskDrawable,
        static_cast<int>(impl_->textures.size()));
    const std::vector<RenderOp> plan = BuildRenderPlan(descs, 1.0f);

    const std::vector<cubism::MaskRef> empty;
    for (const RenderOp& op : plan) {
        const int index = op.drawableIndex;
        const std::vector<cubism::MaskRef>& masks =
            (index >= 0 && static_cast<std::size_t>(index) < impl_->masks.size())
                ? impl_->masks[static_cast<std::size_t>(index)]
                : empty;

        if (op.useMask && !masks.empty() && RenderMaskTexture(masks, transform, ctx)) {
            glUniform1f(impl_->uniformMaskPass, 0.0f);
            glUniform1f(impl_->uniformUseMask, 1.0f);
            glUniform1f(impl_->uniformInvertMask, op.invertMask ? 1.0f : 0.0f);
        } else {
            // 遮罩纹理不可用（GL 资源创建失败）：退化为不裁剪，
            // 至少让模型可见，而不是整块消失。
            glUniform1f(impl_->uniformUseMask, 0.0f);
            glUniform1f(impl_->uniformInvertMask, 0.0f);
        }

        // 混合模式：官方三套 glBlendFuncSeparate（Add/Mult 的 Alpha 用 ZERO/ONE）。
        const BlendFunc& bf = op.blendFunc;
        glBlendFuncSeparate(ToGlBlendFactor(bf.srcRgb), ToGlBlendFactor(bf.dstRgb),
                            ToGlBlendFactor(bf.srcA), ToGlBlendFactor(bf.dstA));
        // 官方 GetModelColorWithOpacity：premultiplied 下 RGB 与 A 一起乘 opacity。
        glUniform4f(impl_->uniformBaseColor, op.baseColor.r, op.baseColor.g,
                    op.baseColor.b, op.baseColor.a);

        DrawDrawable(index, transform, false);
    }

    glUseProgram(0);
}

void Live2DModel::ResolveMaskRelations(int drawableCount) {
    impl_->masks.clear();
    impl_->isMaskDrawable.assign(static_cast<std::size_t>(std::max(0, drawableCount)), 0);
    impl_->masks.resize(static_cast<std::size_t>(std::max(0, drawableCount)));

    int maskCount = 0;
    for (int i = 0; i < drawableCount; ++i) {
        std::vector<cubism::MaskRef> refs;
        if (cubism::GetDrawableMasks(impl_->model, i, refs) && !refs.empty()) {
            impl_->masks[static_cast<std::size_t>(i)] = refs;
            ++maskCount;
            for (const cubism::MaskRef& ref : refs) {
                if (ref.drawableIndex >= 0 &&
                    static_cast<std::size_t>(ref.drawableIndex) < impl_->isMaskDrawable.size()) {
                    impl_->isMaskDrawable[static_cast<std::size_t>(ref.drawableIndex)] = 1;
                }
            }
        }
    }
    impl_->masksResolved = true;

    // --- 遮罩拓扑诊断（DEBUG 级）---
    // 曾经把「兼作遮罩源的可见部件」整块从主画面剔除，导致脸/腿变透明块。
    // 这里统计各类部件数量，便于用真实模型核对渲染计划是否漏画。
    {
        int multiMask = 0, inverted = 0, maskSourceVisible = 0, maskSourceHidden = 0;
        int noTexture = 0, invisible = 0;
        for (int i = 0; i < drawableCount; ++i) {
            const auto& refs = impl_->masks[static_cast<std::size_t>(i)];
            if (refs.size() > 1) ++multiMask;
            for (const auto& r : refs) if (r.inverted) ++inverted;
            cubism::Drawable d;
            if (cubism::GetDrawable(impl_->model, i, d)) {
                if (!d.visible || d.opacity <= 0.0f) ++invisible;
                if (d.textureIndex < 0 ||
                    d.textureIndex >= static_cast<int>(impl_->textures.size())) ++noTexture;
            }
            if (static_cast<std::size_t>(i) < impl_->isMaskDrawable.size() &&
                impl_->isMaskDrawable[static_cast<std::size_t>(i)] != 0) {
                if (cubism::GetDrawable(impl_->model, i, d) && d.visible && d.opacity > 0.0f &&
                    d.textureIndex >= 0 &&
                    d.textureIndex < static_cast<int>(impl_->textures.size())) {
                    ++maskSourceVisible;
                } else {
                    ++maskSourceHidden;
                }
            }
        }
        PPET_LOG_TAG_DEBUG(
            "LIVE2D",
            "遮罩拓扑：部件 %d / 被遮罩 %d / 多遮罩 %d / 反转 %d / 遮罩源可见(有贴图) %d / "
            "遮罩源隐藏 %d / 无贴图 %d / 不可见 %d",
            drawableCount, maskCount, multiMask, inverted, maskSourceVisible,
            maskSourceHidden, noTexture, invisible);
    }

    if (maskCount > 0) {
        PPET_LOG_TAG("LIVE2D", "已解析遮罩关系：%d 个部件带遮罩", maskCount);
    }
}

bool Live2DModel::RenderMaskTexture(const std::vector<cubism::MaskRef>& masks,
                                    const Mat4& transform, const RenderContext& ctx) {
    if (masks.empty()) return false;

    // 遮罩纹理与主画面同尺寸：遮罩用同一个 MVP 绘制，
    // 于是「模型坐标 → 遮罩 UV」与「模型坐标 → 裁剪空间」严格对应。
    int width = ctx.viewportWidth;
    int height = ctx.viewportHeight;
    if (width <= 0 || height <= 0) return false;
    // 上限保护：超大窗口时降采样，避免遮罩纹理吃掉过多显存。
    constexpr int kMaxMaskSize = 2048;
    if (width > kMaxMaskSize || height > kMaxMaskSize) {
        const float scale = static_cast<float>(kMaxMaskSize) /
                            static_cast<float>(std::max(width, height));
        width = std::max(1, static_cast<int>(width * scale));
        height = std::max(1, static_cast<int>(height * scale));
    }

    Impl::MaskTarget& target = impl_->mask;
    if (!target.framebuffer || target.width != width || target.height != height) {
        if (target.framebuffer) glDeleteFramebuffers(1, &target.framebuffer);
        if (target.texture) glDeleteTextures(1, &target.texture);
        target.framebuffer = 0;
        target.texture = 0;
        target.width = 0;
        target.height = 0;

        glGenTextures(1, &target.texture);
        glBindTexture(GL_TEXTURE_2D, target.texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glGenFramebuffers(1, &target.framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               target.texture, 0);
        const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
        if (!ok) {
            PPET_LOG_WARN("LIVE2D: 遮罩纹理创建失败（%dx%d），本帧不做裁剪", width, height);
            glDeleteFramebuffers(1, &target.framebuffer);
            glDeleteTextures(1, &target.texture);
            target.framebuffer = 0;
            target.texture = 0;
            return false;
        }
        target.width = width;
        target.height = height;
    }

    // --- 画进遮罩纹理 ---
    glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
    glViewport(0, 0, target.width, target.height);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    glUseProgram(impl_->program);
    glUniformMatrix4fv(impl_->uniformMatrix, 1, GL_FALSE, transform.m);
    glUniform1i(impl_->uniformTexture, 0);
    glUniform1i(impl_->uniformMask, 1);
    glUniform1f(impl_->uniformUseMask, 0.0f);
    glUniform1f(impl_->uniformInvertMask, 0.0f);
    // 遮罩通道：片元只输出几何覆盖度，不采样部件贴图。
    glUniform1f(impl_->uniformMaskPass, 1.0f);

    // 遮罩纹理作为 s_mask 绑定，但遮罩本身不使用遮罩。
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, target.texture);
    glActiveTexture(GL_TEXTURE0);

    // 【关键修复】遍历该部件挂载的**所有**遮罩，把几何覆盖度叠加进同一张
    // 遮罩纹理（GL_ONE/ONE_MINUS_SRC_ALPHA 使 alpha 累积 => 并集语义）。
    // 早期版本只画 masks[0]，多遮罩模型会缺裁剪区，被裁部件出现透明破洞。
    // 官方 Framework 同样是把 masks 数组里的每个 drawable 都画进裁剪缓冲。
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    for (const cubism::MaskRef& ref : masks) {
        if (ref.drawableIndex < 0) continue;
        cubism::Drawable maskDrawable;
        float maskOpacity = 1.0f;
        if (cubism::GetDrawable(impl_->model, ref.drawableIndex, maskDrawable)) {
            maskOpacity = maskDrawable.opacity;
        }
        const ModelColor color = ModelColorWithOpacity(maskOpacity);
        glUniform4f(impl_->uniformBaseColor, color.r, color.g, color.b, color.a);
        DrawDrawable(ref.drawableIndex, transform, true);
    }

    glUniform1f(impl_->uniformMaskPass, 0.0f);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    // 遮罩纹理已经在 GL_TEXTURE1 上绑定好了；这里只恢复主画面的 viewport，
    // 不能把遮罩纹理绑到 TEXTURE0（会覆盖部件贴图）。
    glViewport(0, 0, ctx.viewportWidth, ctx.viewportHeight);
    glActiveTexture(GL_TEXTURE0);
    return true;
}

void Live2DModel::DrawDrawable(int index, const Mat4& transform, bool maskPass) {
    cubism::Drawable drawable;
    if (!cubism::GetDrawable(impl_->model, index, drawable)) return;
    // 遮罩通道画的是「形状」：Cubism 里纯裁剪部件常 csmIsVisible=false 或
    // opacity=0，但它的几何仍要进遮罩纹理。早期版本在这里一刀切 return，
    // 会让这类遮罩纹理全空 → 被裁剪部件整块消失 / 出现透明块。
    if (!maskPass) {
        if (!drawable.visible) return;
        if (drawable.opacity <= 0.0f) return;
    }
    if (!drawable.positions || !drawable.uvs || !drawable.indices) return;

    // transform（模型视图矩阵）在 BuildTransform 中给出，用于把模型放到屏幕位置；
    // 当前顶点已按画布坐标系上传，绘制暂未显式乘入该矩阵，保留参数以便后续接入。
    (void)transform;

    // --- 纹理选择 ---
    //
    // 遮罩部件在 Cubism 里没有自己的贴图（纹理索引为 -1）：
    //   · 画进遮罩纹理时用白色实心填充——遮罩的「形状」由几何决定；
    //   · 画进主画面时必须跳过，否则会变成一块盖住角色的白色矩形。
    const int textureIndex = drawable.textureIndex;
    const bool hasOwnTexture =
        textureIndex >= 0 && textureIndex < static_cast<int>(impl_->textures.size());
    if (!hasOwnTexture && !maskPass) return;

    Impl::DrawableBuffers& buffer = impl_->buffers[static_cast<std::size_t>(index)];

    // --- 首次使用时创建 VAO / VBO / EBO ---
    if (!buffer.vao) {
        glGenVertexArrays(1, &buffer.vao);
        glGenBuffers(1, &buffer.vbo);
        glGenBuffers(1, &buffer.ebo);
        if (!buffer.vao || !buffer.vbo || !buffer.ebo) return;

        glBindVertexArray(buffer.vao);
        glBindBuffer(GL_ARRAY_BUFFER, buffer.vbo);
        // 位置与 UV 交错存放：x, y, u, v。
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                              reinterpret_cast<const void*>(2 * sizeof(float)));
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffer.ebo);
        glBindVertexArray(0);
    }

    // --- 上传顶点数据 ---
    //
    // 顶点与 UV 是分开的两个数组，需要交错打包。
    // Core 的顶点数据每帧都会变（参数变化导致顶点重算），因此每次都上传。
    const int vertexCount = drawable.vertexCount;
    std::vector<float> interleaved(static_cast<std::size_t>(vertexCount) * 4);
    for (int v = 0; v < vertexCount; ++v) {
        interleaved[static_cast<std::size_t>(v) * 4 + 0] = drawable.positions[v * 2 + 0];
        interleaved[static_cast<std::size_t>(v) * 4 + 1] = drawable.positions[v * 2 + 1];
        interleaved[static_cast<std::size_t>(v) * 4 + 2] = drawable.uvs[v * 2 + 0];
        interleaved[static_cast<std::size_t>(v) * 4 + 3] = drawable.uvs[v * 2 + 1];
    }

    glBindVertexArray(buffer.vao);
    glBindBuffer(GL_ARRAY_BUFFER, buffer.vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(interleaved.size() * sizeof(float)),
                 interleaved.data(), GL_DYNAMIC_DRAW);

    // 索引：每个部件的索引空间独立，因此每个部件用独立的 EBO。
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffer.ebo);
    if (buffer.indexCount != drawable.indexCount) {
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(drawable.indexCount * sizeof(unsigned short)),
                     drawable.indices, GL_STATIC_DRAW);
        buffer.indexCount = drawable.indexCount;
    }

    // --- 混合模式 / baseColor 由渲染计划统一设置 ---
    //
    // 决策层在 CubismRenderPlan.hpp（官方 Framework 的三套 glBlendFuncSeparate
    // 与 premultiplied GetModelColorWithOpacity），执行层在 Live2DModel::Render()。
    // 本函数只负责几何、纹理与 glDrawElements，不再自己决定 GL 状态。
    // 遮罩通道（maskPass）由 RenderMaskTexture() 设置普通混合与白色 baseColor。

    // --- 纹理 ---
    GLuint texture = impl_->whiteTexture;
    if (hasOwnTexture && !maskPass) {
        texture = impl_->textures[static_cast<std::size_t>(textureIndex)];
    }
    if (!texture) return;
    glBindTexture(GL_TEXTURE_2D, texture);

    glDrawElements(GL_TRIANGLES, drawable.indexCount, GL_UNSIGNED_SHORT, nullptr);
    glBindVertexArray(0);
}

void Live2DModel::ReleaseGraphics() {
    if (!impl_) return;

    if (impl_->mask.framebuffer) {
        glDeleteFramebuffers(1, &impl_->mask.framebuffer);
        impl_->mask.framebuffer = 0;
    }
    if (impl_->mask.texture) {
        glDeleteTextures(1, &impl_->mask.texture);
        impl_->mask.texture = 0;
    }
    impl_->mask.width = 0;
    impl_->mask.height = 0;

    for (auto& buffer : impl_->buffers) {
        if (buffer.vao) glDeleteVertexArrays(1, &buffer.vao);
        if (buffer.vbo) glDeleteBuffers(1, &buffer.vbo);
        if (buffer.ebo) glDeleteBuffers(1, &buffer.ebo);
    }
    impl_->buffers.clear();
    impl_->drawableCount = 0;

    for (GLuint texture : impl_->textures) {
        if (texture && texture != impl_->whiteTexture) glDeleteTextures(1, &texture);
    }
    impl_->textures.clear();

    if (impl_->whiteTexture) {
        glDeleteTextures(1, &impl_->whiteTexture);
        impl_->whiteTexture = 0;
    }

    if (impl_->program) {
        glDeleteProgram(impl_->program);
        impl_->program = 0;
    }
}

std::vector<std::pair<std::string, float>> Live2DModel::ParameterSnapshot() const {
    std::vector<std::pair<std::string, float>> out;
    if (!impl_ || !impl_->model) return out;
    const int count = cubism::ParameterCount(impl_->model);
    out.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const std::string id = cubism::ParameterId(impl_->model, i);
        if (id.empty()) continue;
        out.emplace_back(id, cubism::ParameterValue(impl_->model, i));
    }
    return out;
}

ModelDebugInfo Live2DModel::DebugInfo() const {
    ModelDebugInfo info;
    info.displayName = DisplayName();
    info.type = ModelType::Live2D;
    info.loaded = loaded_;
    info.formatVersion = "Cubism 5（.moc3 + .model3.json）";
    info.textEncoding = "内嵌（model3.json / UTF-8）";
    info.isSkinned = false;
    info.doubleSided = true;

    if (impl_) {
        const Live2DManifest& m = impl_->manifest;
        info.path = m.model3Path;

        info.textureReferenced = m.texturePaths.size();
        info.textures.reserve(m.texturePaths.size());
        std::size_t loadedTextures = 0;
        for (std::size_t i = 0; i < m.texturePaths.size(); ++i) {
            ModelDebugInfo::TextureInfo t;
            t.name = i < m.textureRelative.size() ? m.textureRelative[i]
                                                  : ("纹理 #" + std::to_string(i + 1));
            t.path = m.texturePaths[i];
            t.loaded = FileExists(t.path);
            if (t.loaded) ++loadedTextures;
            t.note = t.loaded ? "磁盘纹理（可预览）" : "纹理文件不存在";
            if (!t.loaded) info.missingResources.push_back(t.path);
            info.textures.push_back(t);
        }
        info.textureLoaded = loadedTextures;

        info.subMeshCount = impl_->buffers.size();
        info.drawCalls = impl_->buffers.size();
        info.gpuReady = impl_->program != 0 && !impl_->buffers.empty();

        info.properties.push_back("moc3=" + m.mocPath);
        info.properties.push_back("部件(Drawable)数=" +
                                  std::to_string(impl_->buffers.size()));
        info.properties.push_back("画布像素/单位=" +
                                  std::to_string(impl_->canvas.pixelsPerUnit));
        std::size_t motionFiles = 0;
        for (const Live2DMotionGroup& g : m.motionGroups) motionFiles += g.files.size();
        info.properties.push_back("动作组=" + std::to_string(m.motionGroups.size()) +
                                  " / 动作文件=" + std::to_string(motionFiles));
        // --- 动作系统（待机/互动）诊断 ---
        std::string groupSummary;
        for (const MotionGroup& g : motionDirector_.Groups()) {
            if (!groupSummary.empty()) groupSummary += " / ";
            groupSummary += g.name + "×" + std::to_string(g.motionIndices.size());
        }
        info.motionSummary = "动作组 " + std::to_string(motionDirector_.GroupCount()) +
                             "（" + (groupSummary.empty() ? "无" : groupSummary) + "）" +
                             " / 命中区 " + std::to_string(motionDirector_.HitAreaCount()) +
                             " / 互动组 " +
                             (motionDirector_.HasInteractionGroups() ? "有" : "无");
        info.motionStatus = motionDirector_.StatusText();

        // 需求 2：智能识别动作组语义（idle/touch/expression + 区域映射），
        // 把识别结论也写进诊断面板，便于确认「随机待机 / 分区触摸」的素材是否齐全。
        const MotionSetAnalysis recognize =
            AnalyzeMotionGroups(motionDirector_.Groups());
        info.motionSummary += " | 识别：" + recognize.Summary();

        // --- 表情系统状态 ---
        // Live2D 的表情有两条通道：表情动作组（Exp*）与表情参数（ParamBrowLY 等）。
        // 两者都要报出来，否则「随机表情为什么没反应」无从排查。
        {
            const std::vector<std::string> expIds = ExpressionPresetIds();
            std::string status = "待机随机表情：";
            status += randomExpression_ ? "开" : "关";
            if (randomExpression_) {
                // 距离下一次随机还有多久。
                status += "（下次 " +
                          std::to_string(static_cast<int>(randomExpressionTimer_ + 0.5f)) + "s）";
            }
            const std::string current = idleFaces_.CurrentId();
            if (!current.empty()) {
                status += " / 当前小表情：" + current;
            }
            info.expressionStatus = status;

            // expIds 是「表情动作组 + 参数预设」的合并列表，可切换表情总数用它；
            // 动作组数量单独统计，否则会把参数预设误报成「表情动作组」。
            int motionExpGroups = 0;
            for (const MotionGroup& group : motionDirector_.Groups()) {
                if (group.kind == MotionGroupKind::Expression && !group.name.empty()) {
                    ++motionExpGroups;
                }
            }
            std::string summary = "表情动作组 " + std::to_string(motionExpGroups) +
                                  " / 表情参数预设 " +
                                  std::to_string(idleFaces_.Presets().size()) +
                                  " / 可切换表情 " + std::to_string(expIds.size());
            info.expressionSummary = summary;
        }
        info.properties.push_back("动作系统=Idle 随机待机 + HitArea 互动（优先级打断 / 淡入淡出）");
        info.properties.push_back("命中区(HitAreas)=" +
                                  std::to_string(motionDirector_.HitAreaCount()));
        info.properties.push_back("当前动作=" + info.motionStatus);
        info.properties.push_back("渲染管线=Live2D 二维部件叠加（与 PMX/VRM 的蒙皮网格不同）");
        info.properties.push_back("坐标系=Cubism 单位坐标 → 裁剪空间（Y 向上）");
    }

    info.renderingOk = loaded_ && info.gpuReady && info.glError == 0;
    if (!loaded_) {
        info.renderStatus = "未加载模型";
    } else if (!info.gpuReady) {
        info.renderStatus = "等待首次渲染建立 GPU 资源";
    } else if (!info.missingResources.empty()) {
        info.renderStatus = "可出图，但缺 " +
                            std::to_string(info.missingResources.size()) +
                            " 张纹理（部件会显示为白色）";
    } else {
        info.renderStatus = "正常渲染";
    }
    return info;
}

}  // namespace ppet::live2d
