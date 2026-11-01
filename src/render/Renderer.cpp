// PPet - C++ 移植版
// 渲染层实现：着色器、纹理、toon 蒙皮网格绘制器。
//
// 坐标与数据约定（与 PMX / VRM 两个后端共用，务必保持一致）：
//   1. 右手系，Y 轴向上，相机位于模型 +Z 一侧看向 -Z；
//      PMX 载入时已把 z 取反并反转三角形绕序，VRM 0.x 已整体旋转 180°，
//      因此两者载入后正面都朝 +Z，渲染层不需要再做朝向修正。
//   2. 矩阵一律列主序（core/Math.hpp 的 Mat4），与 glUniformMatrix4fv 的
//      transpose=GL_FALSE 搭配；VP 矩阵按 proj * view 的顺序相乘。
//   3. 模型 UV 原点在左上（glTF / PMX 约定），而 OpenGL 纹理坐标原点在左下。
//      纹理上传时统一翻转成「首行在最下面」，着色器采样时用 1 - v 还原，
//      两个后端因此画面方向一致。
#include "render/Renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <GL/glew.h>

#include "render/RenderUtils.hpp"

// ---------------------------------------------------------------------------
// stb_image：本文件是渲染层唯一一处 STB_IMAGE_IMPLEMENTATION。
//
// stb_image 的公开符号是 C 链接的全局符号，其它 TU 也可能包含它：
//   - src/model/vrm/VrmImage.cpp 已把符号改名为 vrm_stbi_*；
//   - src/platform/Tray.cpp 用的是原始名字（stbi_load_from_memory 等）。
// 如果这里再用原始名字，链接同一个可执行文件时会出现重复符号错误，
// 因此把公开符号统一改名为 ppet_stbi_*，三份实现互不冲突。
// ---------------------------------------------------------------------------
#define stbi_load                         ppet_stbi_load
#define stbi_load_from_memory             ppet_stbi_load_from_memory
#define stbi_load_from_callbacks          ppet_stbi_load_from_callbacks
#define stbi_load_from_file               ppet_stbi_load_from_file
#define stbi_load_from_file_16            ppet_stbi_load_from_file_16
#define stbi_load_16                      ppet_stbi_load_16
#define stbi_load_16_from_memory          ppet_stbi_load_16_from_memory
#define stbi_load_16_from_callbacks       ppet_stbi_load_16_from_callbacks
#define stbi_loadf                        ppet_stbi_loadf
#define stbi_loadf_from_memory            ppet_stbi_loadf_from_memory
#define stbi_loadf_from_callbacks         ppet_stbi_loadf_from_callbacks
#define stbi_loadf_from_file              ppet_stbi_loadf_from_file
#define stbi_load_gif_from_memory         ppet_stbi_load_gif_from_memory
#define stbi_info                         ppet_stbi_info
#define stbi_info_from_memory             ppet_stbi_info_from_memory
#define stbi_info_from_callbacks          ppet_stbi_info_from_callbacks
#define stbi_info_from_file               ppet_stbi_info_from_file
#define stbi_is_16_bit                    ppet_stbi_is_16_bit
#define stbi_is_16_bit_from_memory        ppet_stbi_is_16_bit_from_memory
#define stbi_is_16_bit_from_callbacks     ppet_stbi_is_16_bit_from_callbacks
#define stbi_is_16_bit_from_file          ppet_stbi_is_16_bit_from_file
#define stbi_is_hdr                       ppet_stbi_is_hdr
#define stbi_is_hdr_from_memory           ppet_stbi_is_hdr_from_memory
#define stbi_is_hdr_from_callbacks        ppet_stbi_is_hdr_from_callbacks
#define stbi_is_hdr_from_file             ppet_stbi_is_hdr_from_file
#define stbi_failure_reason               ppet_stbi_failure_reason
#define stbi_image_free                   ppet_stbi_image_free
#define stbi_set_flip_vertically_on_load  ppet_stbi_set_flip_vertically_on_load
#define stbi_set_flip_vertically_on_load_thread \
    ppet_stbi_set_flip_vertically_on_load_thread
#define stbi_set_unpremultiply_on_load ppet_stbi_set_unpremultiply_on_load
#define stbi_set_unpremultiply_on_load_thread ppet_stbi_set_unpremultiply_on_load_thread
#define stbi_convert_iphone_png_to_rgb ppet_stbi_convert_iphone_png_to_rgb
#define stbi_convert_iphone_png_to_rgb_thread ppet_stbi_convert_iphone_png_to_rgb_thread
#define stbi_convert_wchar_to_utf8      ppet_stbi_convert_wchar_to_utf8
#define stbi_hdr_to_ldr_gamma           ppet_stbi_hdr_to_ldr_gamma
#define stbi_hdr_to_ldr_scale           ppet_stbi_hdr_to_ldr_scale
#define stbi_ldr_to_hdr_gamma           ppet_stbi_ldr_to_hdr_gamma
#define stbi_ldr_to_hdr_scale           ppet_stbi_ldr_to_hdr_scale
#define stbi_zlib_decode_buffer         ppet_stbi_zlib_decode_buffer
#define stbi_zlib_decode_malloc         ppet_stbi_zlib_decode_malloc
#define stbi_zlib_decode_malloc_guesssize ppet_stbi_zlib_decode_malloc_guesssize
#define stbi_zlib_decode_malloc_guesssize_headerflag \
    ppet_stbi_zlib_decode_malloc_guesssize_headerflag
#define stbi_zlib_decode_noheader_buffer ppet_stbi_zlib_decode_noheader_buffer
#define stbi_zlib_decode_noheader_malloc ppet_stbi_zlib_decode_noheader_malloc

#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

namespace ppet {
namespace {

// 骨骼矩阵 UBO 的绑定点。UBO（而不是 SSBO）是因为 GLSL 330 core 里
// SSBO 需要 GL 4.3，而 UBO 从 GL 3.1 起就有。
constexpr unsigned int kBoneBindingPoint = 0;

// 骨骼矩阵数组的最大长度。
//
// GLSL 330 core 的 uniform block 里不允许无长度数组（unsized array），
// 数组长度必须是编译期常量，所以这里按驱动的 GL_MAX_UNIFORM_BLOCK_SIZE
// 动态生成着色器源码，把上限贴着硬件能力走。
//
// 一个 mat4 在 std140 下占 64 字节，16KB 的最小保证值意味着至少 256 根骨骼；
// 实际桌面驱动通常是 64KB，即 1024 根。
constexpr int kMaxBoneCount = 1024;

/// 查询本机一次最多能放进一个 uniform block 多少根骨骼。
int QueryMaxBoneCount() {
    GLint blockSize = 0;
    glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &blockSize);
    if (blockSize <= 0) return 256;
    // std140 下 mat4 占 64 字节（4 列 × vec4 × 4 字节）。
    const int count = blockSize / static_cast<int>(sizeof(Mat4));
    if (count < 16) return 16;
    if (count > kMaxBoneCount) return kMaxBoneCount;
    return count;
}

// ---------------------------------------------------------------------------
// 着色器源码（GLSL 330 core）
// ---------------------------------------------------------------------------

/// 主渲染（toon）顶点着色器。
/// maxBones 会被拼进源码里：uniform block 的数组长度必须是编译期常量。
std::string BuildToonVertexShader(int maxBones) {
    return std::string(R"(#version 330 core

#define MAX_BONES )") +
           std::to_string(maxBones) + R"(

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
layout(location = 3) in vec4 aColor;
// 骨骼索引用整型属性：不能用归一化的 UNSIGNED_BYTE，
// 否则 255 会被解释成 1.0，骨骼索引就错了。
layout(location = 4) in uvec4 aJoints;
layout(location = 5) in vec4 aWeights;

// 骨骼蒙皮矩阵。std140 下 mat4 数组每项 64 字节，
// 与 core/Math.hpp 的 Mat4（16 个 float，列主序）内存布局完全一致。
layout(std140) uniform BoneBlock {
    mat4 uBones[MAX_BONES];
};

uniform mat4 uViewProjection;
uniform vec3 uCameraPos;      // 透视相机的世界坐标
uniform vec3 uViewDirConst;   // 正交相机的视线方向（常量）
uniform int uPerspective;     // 1 = 透视，0 = 正交

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUv;
out vec4 vColor;
out vec3 vViewDir;

// 骨骼索引越界时夹回合法范围，避免读到未初始化的矩阵。
uint SafeBone(uint index) {
    return min(index, uint(MAX_BONES - 1));
}

void main() {
    // 蒙皮：4 个骨骼按权重做线性混合。
    // 蒙皮矩阵的约定是 world * inverseBind（见 model/SkinnedMesh.cpp）。
    mat4 skin = uBones[SafeBone(aJoints.x)] * aWeights.x
              + uBones[SafeBone(aJoints.y)] * aWeights.y
              + uBones[SafeBone(aJoints.z)] * aWeights.z
              + uBones[SafeBone(aJoints.w)] * aWeights.w;

    vec4 world = skin * vec4(aPosition, 1.0);
    vWorldPos = world.xyz;
    // 法线只受旋转影响，不受骨骼平移影响，所以取 skin 的左上 3x3。
    vNormal = mat3(skin) * aNormal;
    vUv = aUv;
    vColor = aColor;

    // 视线方向（从表面指向相机）。
    // 透视相机：每个顶点各自算，靠近相机的边缘会更「掠射」，边缘光才有体积感。
    // 正交相机：所有视线互相平行，方向是常量，直接用 CPU 算好的方向。
    vec3 toCamera = (uPerspective == 1) ? (uCameraPos - world.xyz) : uViewDirConst;
    vViewDir = normalize(toCamera);

    gl_Position = uViewProjection * world;
}
)";
}

/// 主渲染（toon）片元着色器。
const char* const kToonFragmentShader = R"(#version 330 core

in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUv;
in vec4 vColor;
in vec3 vViewDir;      // 从表面指向相机的方向（世界空间）

uniform vec3 uLightDir;      // 世界空间，指向光源
uniform vec4 uDiffuse;       // 材质基础色（线性空间）
uniform vec3 uAmbient;
uniform vec3 uEmissive;
uniform vec3 uShadeColor;    // 阴影色
uniform vec3 uRimColor;      // 边缘光颜色
uniform vec3 uSpecular;
uniform float uShininess;
uniform float uShadeShift;
uniform float uShadeToony;
uniform float uRimLightingMix;
uniform float uRimFresnelPower;
uniform float uLightIntensity;
uniform int uUseToon;
uniform int uUnlit;
uniform int uHasBaseTex;
uniform int uHasToonRamp;
uniform int uHasSphereTex;

uniform sampler2D uBaseColorTex;   // 单元 0
uniform sampler2D uToonRampTex;    // 单元 1
uniform sampler2D uSphereTex;      // 单元 2

out vec4 fragColor;

const float kPi = 3.14159265358979;

vec3 LinearToSrgb(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    vec3 lo = c * 12.92;
    vec3 hi = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
    return mix(lo, hi, step(vec3(0.0031308), c));
}

void main() {
    vec3 N = normalize(vNormal);
    vec3 L = normalize(uLightDir);
    // V 来自顶点着色器：透视相机逐顶点计算，正交相机是常量方向。
    vec3 V = normalize(vViewDir);

    // 基础色 = 材质色 * 顶点色 * 基础色贴图
    vec4 base = uDiffuse * vColor;
    if (uHasBaseTex == 1) {
        // 纹理是 sRGB 内部格式，硬件采样时已经解码到线性空间。
        // 模型 UV 原点在左上，纹理原点在左下，因此 V 取反。
        base *= texture(uBaseColorTex, vec2(vUv.x, 1.0 - vUv.y));
    }

    vec3 color;
    if (uUnlit == 1) {
        // 自发光材质：不受光照影响。
        color = base.rgb;
    } else {
        float ndl = dot(N, L);

        // toon 阴影：把 N·L 平滑地切成「阴影 / 受光」两段，
        // 边界由 shadeShift 偏移、shadeToony 控制软硬。
        float shade = smoothstep(uShadeShift - 0.1, uShadeShift + uShadeToony * 0.5, ndl);

        if (uHasToonRamp == 1) {
            // toon 渐变贴图是一张「以受光程度为横轴」的一维渐变图：
            //   PMX 的共享 toon 贴图、MToon 的 shadeMultiply / shadeColor 都如此，
            //   左端是全阴影、右端是全受光。
            // 所以横轴必须用 N·L（而不是模型 UV 的 x），
            // 否则渐变会随模型表面的贴图坐标走，完全失去光照语义。
            // 竖直方向取中间一行（v = 0.5），这类贴图通常只有一行有效内容。
            float rampU = clamp(ndl * 0.5 + 0.5, 0.0, 1.0);
            vec3 ramp = texture(uToonRampTex, vec2(rampU, 0.5)).rgb;
            if (uUseToon == 1) {
                // ramp 是「受光系数」，乘到基础色上得到该受光程度下的实际颜色；
                // shade 提供 toon 的硬边界，让渐变被切成两段。
                color = mix(ramp * base.rgb, base.rgb, shade);
            } else {
                color = base.rgb * clamp(ndl * 0.5 + 0.5, 0.0, 1.0);
            }
        } else if (uUseToon == 1) {
            color = mix(uShadeColor * base.rgb, base.rgb, shade);
        } else {
            // 非 toon 材质退化成柔和的半兰伯特。
            color = base.rgb * clamp(ndl * 0.5 + 0.5, 0.0, 1.0);
        }

        // 球面贴图（PMX sphere）：按视线方向做经纬映射。
        // 注意：契约里 sphereTex 定义为「PMX 球面映射贴图」，这里实现的就是这个语义。
        // VRM 后端目前把 MToon 的 rim 贴图也放进了这个槽位，
        // 而 rim 贴图实际上是一维渐变（沿 X 方向按边缘光系数查表），
        // 语义与球面映射不同 —— 已在交付报告中列为「与契约不符」项。
        if (uHasSphereTex == 1) {
            vec3 dir = normalize(-V);
            float su = atan(dir.z, dir.x) / (2.0 * kPi) + 0.5;
            float sv = acos(clamp(dir.y, -1.0, 1.0)) / kPi;
            color *= texture(uSphereTex, vec2(su, sv)).rgb;
        }

        // 边缘光：正面接近 1，掠射角接近 0，用菲涅耳项做衰减。
        float rim = pow(1.0 - max(dot(N, V), 0.0), max(uRimFresnelPower, 0.001));
        color += uRimColor * rim * uRimLightingMix;

        // toon 高光：把连续高光切成阶梯，保留卡通的硬边。
        vec3 H = normalize(L + V);
        float spec = pow(max(dot(N, H), 0.0), max(uShininess, 1.0));
        color += uSpecular * smoothstep(0.35, 0.65, spec);

        color = color * uLightIntensity + uAmbient * base.rgb + uEmissive;
    }

    // 默认帧缓冲不是 sRGB 的，输出前需要手工编码回 sRGB。
    fragColor = vec4(LinearToSrgb(color), base.a);
}
)";

/// 描边顶点着色器：沿法线外扩顶点位置。
/// maxBones 会被拼进源码里：uniform block 的数组长度必须是编译期常量。
std::string BuildOutlineVertexShader(int maxBones) {
    return std::string(R"(#version 330 core

#define MAX_BONES )") +
           std::to_string(maxBones) + R"(

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 4) in uvec4 aJoints;
layout(location = 5) in vec4 aWeights;

layout(std140) uniform BoneBlock {
    mat4 uBones[MAX_BONES];
};

uniform mat4 uViewProjection;
uniform float uOutlineWidth;   // 世界单位的外扩量
uniform float uOutlineScale;   // 额外缩放（材质宽度之外的调节）

// 骨骼索引越界时夹回合法范围，避免读到未初始化的矩阵。
uint SafeBone(uint index) {
    return min(index, uint(MAX_BONES - 1));
}

void main() {
    mat4 skin = uBones[SafeBone(aJoints.x)] * aWeights.x
              + uBones[SafeBone(aJoints.y)] * aWeights.y
              + uBones[SafeBone(aJoints.z)] * aWeights.z
              + uBones[SafeBone(aJoints.w)] * aWeights.w;

    vec4 world = skin * vec4(aPosition, 1.0);
    vec3 n = normalize(mat3(skin) * aNormal);

    vec4 clip = uViewProjection * world;
    // gl_Position.w 是裁剪空间的深度：透视投影下等于视空间距离。
    // 用它放大外扩量，可以让描边在屏幕上的粗细基本恒定；
    // 正交投影下 w 恒为 1，外扩量就是固定的世界长度。
    float depth = max(clip.w, 1e-4);
    vec3 expanded = world.xyz + n * (uOutlineWidth * uOutlineScale * depth);

    gl_Position = uViewProjection * vec4(expanded, 1.0);
}
)";
}

/// 描边片元着色器：纯色。
const char* const kOutlineFragmentShader = R"(#version 330 core

uniform vec3 uOutlineColor;

out vec4 fragColor;

const float kPi = 3.14159265358979;

vec3 LinearToSrgb(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    vec3 lo = c * 12.92;
    vec3 hi = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
    return mix(lo, hi, step(vec3(0.0031308), c));
}

void main() {
    fragColor = vec4(LinearToSrgb(uOutlineColor), 1.0);
}
)";

/// GPU 顶点格式。
/// 与 model/SkinnedMesh.hpp 的 Vertex 语义一一对应，但用显式字段布局，
/// 不依赖编译器对 C++ 结构体的填充规则。
struct GpuVertex {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
    float r, g, b, a;
    std::uint32_t j0, j1, j2, j3;
    float w0, w1, w2, w3;
};

static_assert(sizeof(GpuVertex) == 80, "GPU 顶点结构大小与着色器属性布局不一致");

/// 从视图投影矩阵（proj * view）反解出相机在世界空间的信息。
///
/// 渲染层只拿到 viewProjection，拿不到相机位置，但边缘光需要
/// 「从表面指向相机」的方向 V，所以必须从矩阵里反解。
///
/// 反解依据（列主序，At(row, col)）：
///   VP = P * V。V 的三行分别是 right / up / backward（backward 由目标点指向相机）。
///   - 正交投影 P 的对角元为 (sx, sy, sz)，所以 VP 的第 0、1 行分别平行于 right、up；
///   - 透视投影 P 的第 0、1 行是 (f/aspect, 0, 0) 与 (0, f, 0)，
///     同样只是 right / up 的正比例缩放，归一化后即可得到相机的 right 与 up；
///   - backward = cross(right, up)（右手系下 x × y = z）。
/// 相机位置（仅透视有意义）用齐次反解：
///   相机中心在视空间是 (0,0,0,1)，经 P 变换后 clip = (0, 0, c, 0)，
///   所以 eye = invVP * (0,0,1,0) 再除以 w 即可（c 是常数，不影响结果）。
struct ViewInfo {
    Vec3 cameraPos{0.0f, 0.0f, 0.0f};  ///< 透视相机的世界坐标（正交时无意义）
    Vec3 viewDir{0.0f, 0.0f, 1.0f};    ///< 从表面指向相机的方向（正交时是常量）
    bool perspective = false;
};

ViewInfo ExtractViewInfo(const Mat4& vp) {
    ViewInfo info;

    // 相机的 right / up：VP 的第 0、1 行归一化即可。
    const Vec3 right = Normalize(Vec3(vp.At(0, 0), vp.At(0, 1), vp.At(0, 2)));
    const Vec3 up = Normalize(Vec3(vp.At(1, 0), vp.At(1, 1), vp.At(1, 2)));
    // backward：右手系下 right × up = backward。
    Vec3 backward = Cross(right, up);
    if (Length(backward) < kEpsilon) {
        // right 与 up 退化（数值异常）时的兜底方向。
        backward = Vec3(0.0f, 0.0f, 1.0f);
    } else {
        backward = Normalize(backward);
    }
    info.viewDir = backward;

    // 区分透视与正交：
    //   透视：P 的第 3 行是 (0, 0, -1, 0)，于是 VP 的第 3 行 = -V 的第 2 行，
    //         其 xyz 分量等于 -backward，长度必然为 1（不会全零）；
    //   正交：P 的第 3 行是 (0, 0, 0, 1)，VP 的第 3 行前三个分量恒为 0。
    info.perspective = std::fabs(vp.At(3, 0)) + std::fabs(vp.At(3, 1)) +
                               std::fabs(vp.At(3, 2)) >
                       1e-6f;

    if (info.perspective) {
        // eye = invVP * (0, 0, 1, 0)，再除以 w 变成有限坐标。
        const Mat4 inv = vp.Inverse();
        const float w = inv.At(3, 2);
        if (std::fabs(w) > 1e-6f) {
            info.cameraPos = Vec3(inv.At(0, 2) / w, inv.At(1, 2) / w, inv.At(2, 2) / w);
        }
    }
    return info;
}

/// 判断网格的三角形绕序，并把索引统一成「CW 为正面」。
///
/// 为什么需要统一：
///   - OpenGL 默认 GL_CCW 为正面，但 PMX（左手系转右手系时反转了绕序）
///     与 VRM（glTF 规定 CCW）载入后可能相反；
///   - 描边 pass 用「反转正面 + 剔除正面」来只画背面，
///     只有在正面定义固定的情况下才是稳定的。
///
/// 判定方法：三角形的几何法线（右手系叉积）与顶点法线平均值的点积。
/// 点积为正说明「从外面看是逆时针」，即 CCW 正面，需要反转绕序。
bool NeedsWindingFlip(const SkinnedMesh& mesh) {
    if (mesh.vertices.size() < 3 || mesh.indices.size() < 3) return false;
    int positive = 0;
    int negative = 0;
    const std::size_t triangleCount = mesh.indices.size() / 3;
    // 只抽样一部分三角形即可判断整体绕序，避免大模型全量扫描。
    const std::size_t step = triangleCount > 512 ? triangleCount / 512 : 1;
    for (std::size_t t = 0; t < triangleCount; t += step) {
        const std::uint32_t i0 = mesh.indices[t * 3 + 0];
        const std::uint32_t i1 = mesh.indices[t * 3 + 1];
        const std::uint32_t i2 = mesh.indices[t * 3 + 2];
        if (i0 >= mesh.vertices.size() || i1 >= mesh.vertices.size() ||
            i2 >= mesh.vertices.size()) {
            continue;
        }
        const Vertex& v0 = mesh.vertices[i0];
        const Vertex& v1 = mesh.vertices[i1];
        const Vertex& v2 = mesh.vertices[i2];
        const Vec3 geo = Cross(v1.position - v0.position, v2.position - v0.position);
        const Vec3 avg = v0.normal + v1.normal + v2.normal;
        const float dot = Dot(geo, avg);
        if (dot > 1e-8f) {
            ++positive;
        } else if (dot < -1e-8f) {
            ++negative;
        }
    }
    return positive > negative;
}

/// 编译单个着色器阶段，失败时把日志写进 error。
unsigned int CompileStage(GLenum type, const std::string& source, const char* stageName,
                          std::string& error) {
    const unsigned int shader = glCreateShader(type);
    if (shader == 0) {
        error = std::string("创建") + stageName + "失败：glCreateShader 返回 0";
        return 0;
    }
    const char* src = source.c_str();
    const int len = static_cast<int>(source.size());
    glShaderSource(shader, 1, &src, &len);
    glCompileShader(shader);

    GLint status = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status == GL_TRUE) return shader;

    GLint logLength = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
    std::string log(static_cast<std::size_t>(logLength > 0 ? logLength : 1), '\0');
    glGetShaderInfoLog(shader, logLength, nullptr, log.data());
    if (logLength <= 0) log.clear();

    error = std::string("编译") + stageName + "失败。原始错误信息：\n" + log;
    glDeleteShader(shader);
    return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Shader
// ---------------------------------------------------------------------------

Shader::~Shader() {
    if (program_ != 0) {
        glDeleteProgram(program_);
        program_ = 0;
    }
}

bool Shader::Compile(const std::string& vertexSrc, const std::string& fragmentSrc,
                     std::string& error) {
    error.clear();

    const unsigned int vert = CompileStage(GL_VERTEX_SHADER, vertexSrc, "顶点着色器", error);
    if (vert == 0) return false;

    const unsigned int frag = CompileStage(GL_FRAGMENT_SHADER, fragmentSrc, "片元着色器", error);
    if (frag == 0) {
        glDeleteShader(vert);
        return false;
    }

    const unsigned int program = glCreateProgram();
    if (program == 0) {
        glDeleteShader(vert);
        glDeleteShader(frag);
        error = "创建着色器程序失败：glCreateProgram 返回 0";
        return false;
    }
    glAttachShader(program, vert);
    glAttachShader(program, frag);
    glLinkProgram(program);
    // 链接之后 shader 对象就可以删了，program 内部会保留引用。
    glDetachShader(program, vert);
    glDetachShader(program, frag);
    glDeleteShader(vert);
    glDeleteShader(frag);

    GLint status = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (status != GL_TRUE) {
        GLint logLength = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
        std::string log(static_cast<std::size_t>(logLength > 0 ? logLength : 1), '\0');
        glGetProgramInfoLog(program, logLength, nullptr, log.data());
        if (logLength <= 0) log.clear();

        error = "链接着色器程序失败。原始错误信息：\n" + log;
        glDeleteProgram(program);
        return false;
    }

    if (program_ != 0) glDeleteProgram(program_);
    program_ = program;
    // 换了 program，旧的 uniform 位置全部作废。
    uniformLocations_.clear();
    return true;
}

void Shader::Use() const {
    if (program_ != 0) glUseProgram(program_);
}

int Shader::UniformLocation(const char* name) const {
    if (program_ == 0 || name == nullptr) return -1;
    const auto it = uniformLocations_.find(name);
    if (it != uniformLocations_.end()) return it->second;

    const int location = glGetUniformLocation(program_, name);
    // 位置为 -1 说明这个 uniform 被优化掉了（或者名字写错了）。
    // 同样要缓存下来，避免每帧都去问驱动一次。
    uniformLocations_.emplace(name, location);
    return location;
}

void Shader::SetMat4(const char* name, const Mat4& m) const {
    const int location = UniformLocation(name);
    if (location < 0) return;
    // transpose = GL_FALSE：Mat4 已经是列主序，和 OpenGL 的存储顺序一致。
    glUniformMatrix4fv(location, 1, GL_FALSE, m.m);
}

void Shader::SetVec3(const char* name, const Vec3& v) const {
    const int location = UniformLocation(name);
    if (location < 0) return;
    glUniform3f(location, v.x, v.y, v.z);
}

void Shader::SetVec4(const char* name, float x, float y, float z, float w) const {
    const int location = UniformLocation(name);
    if (location < 0) return;
    glUniform4f(location, x, y, z, w);
}

void Shader::SetFloat(const char* name, float v) const {
    const int location = UniformLocation(name);
    if (location < 0) return;
    glUniform1f(location, v);
}

void Shader::SetInt(const char* name, int v) const {
    const int location = UniformLocation(name);
    if (location < 0) return;
    glUniform1i(location, v);
}

// ---------------------------------------------------------------------------
// Texture
// ---------------------------------------------------------------------------

Texture::~Texture() {
    if (id_ != 0) {
        glDeleteTextures(1, &id_);
        id_ = 0;
    }
}

Texture::Texture(Texture&& other) noexcept : id_(other.id_) {
    // 转移所有权后必须把源对象的 GL 名清零，
    // 否则两个对象析构时会把同一个纹理删两次（double free）。
    other.id_ = 0;
}

Texture& Texture::operator=(Texture&& other) noexcept {
    if (this == &other) return *this;
    if (id_ != 0) {
        glDeleteTextures(1, &id_);
        id_ = 0;
    }
    id_ = other.id_;
    other.id_ = 0;
    return *this;
}

bool Texture::Create(const unsigned char* pixels, int width, int height, int channels,
                     bool srgb, bool mipmap) {
    if (pixels == nullptr || width <= 0 || height <= 0) return false;
    if (channels < 1 || channels > 4) return false;

    // 通道数 -> 纹理格式
    GLenum format = GL_RGBA;
    switch (channels) {
        case 1: format = GL_RED; break;
        case 2: format = GL_RG; break;
        case 3: format = GL_RGB; break;
        default: format = GL_RGBA; break;
    }

    // 内部格式：sRGB 贴图用 sRGB 内部格式，硬件在采样时会自动解码到线性空间，
    // 这样着色器里的光照运算就是在线性空间进行的。
    //
    // 注意：单通道 / 双通道的 sRGB 内部格式（GL_SR8 / GL_SRG8）要到 GL 4.2 才有，
    // 本项目目标是 GL 3.3 core，所以这两种情况先扩展成 RGBA 再上传，
    // 保证颜色空间语义不变。
    GLenum internalFormat = GL_RGBA8;
    int uploadChannels = channels;
    if (srgb) {
        switch (channels) {
            case 1:
            case 2:
                internalFormat = GL_SRGB8_ALPHA8;
                format = GL_RGBA;
                uploadChannels = 4;
                break;
            case 3:
                internalFormat = GL_SRGB8;
                break;
            default:
                internalFormat = GL_SRGB8_ALPHA8;
                break;
        }
    } else {
        switch (channels) {
            case 1: internalFormat = GL_R8; break;
            case 2: internalFormat = GL_RG8; break;
            case 3: internalFormat = GL_RGB8; break;
            default: internalFormat = GL_RGBA8; break;
        }
    }

    // 垂直翻转：stb_image 的输出首行是图像的最上面一行，
    // 而 OpenGL 纹理坐标的原点在左下（v=0 对应首行）。
    // 统一在这里翻转成「首行在最下面」，着色器再用 1 - v 采样，
    // 这样 PMX（LoadFromFile）与 VRM（Create 直接传入解码结果）画面方向一致。
    const std::size_t rowBytes = static_cast<std::size_t>(width) *
                                 static_cast<std::size_t>(channels);
    const std::size_t outRowBytes = static_cast<std::size_t>(width) *
                                    static_cast<std::size_t>(uploadChannels);
    std::vector<unsigned char> flipped(outRowBytes * static_cast<std::size_t>(height), 255);
    for (int y = 0; y < height; ++y) {
        const unsigned char* src =
            pixels + static_cast<std::size_t>(height - 1 - y) * rowBytes;
        unsigned char* dst = flipped.data() + static_cast<std::size_t>(y) * outRowBytes;
        if (uploadChannels == channels) {
            std::memcpy(dst, src, rowBytes);
        } else {
            // 扩展到 RGBA：灰度复制到 RGB，alpha 补 1。
            for (int x = 0; x < width; ++x) {
                const unsigned char* px = src + static_cast<std::size_t>(x) * channels;
                unsigned char* out = dst + static_cast<std::size_t>(x) * 4;
                out[0] = px[0];
                out[1] = (channels >= 3) ? px[1] : px[0];
                out[2] = (channels >= 3) ? px[2] : px[0];
                out[3] = (channels >= 4) ? px[3] : 255;
            }
        }
    }

    if (id_ != 0) {
        glDeleteTextures(1, &id_);
        id_ = 0;
    }

    glGenTextures(1, &id_);
    if (id_ == 0) return false;

    glBindTexture(GL_TEXTURE_2D, id_);
    // 行字节数不是 4 的倍数时（例如 RGB 格式的奇数宽度），
    // 必须把对齐改成 1，否则每行都会被补齐，解出来的图会斜掉。
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internalFormat), width, height, 0, format,
                 GL_UNSIGNED_BYTE, flipped.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    if (mipmap) {
        glGenerateMipmap(GL_TEXTURE_2D);
        // 缩小时在 mip 层级之间插值，放大时线性插值。
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    } else {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // 桌宠的贴图经常边缘有内容，重复采样会在边界产生接缝，
    // 因此用 CLAMP_TO_EDGE 把边缘像素拉伸出去。
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    PPET_GL_CHECK();
    return true;
}

bool Texture::LoadFromFile(const std::string& path, bool srgb, bool mipmap) {
    if (path.empty()) return false;

    // 统一解码成 RGBA 四通道，渲染层不需要区分原始通道数。
    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* pixels = ppet_stbi_load(path.c_str(), &width, &height, &channels, 4);
    if (pixels == nullptr) {
        // 文件不存在或解码失败：不抛异常，调用方回退到纯色。
        return false;
    }

    const bool ok = Create(pixels, width, height, 4, srgb, mipmap);
    // 解码出来的内存必须释放，否则每次加载贴图都会泄漏。
    ppet_stbi_image_free(pixels);
    return ok;
}

void Texture::Bind(unsigned int unit) const {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, id_);
}

// ---------------------------------------------------------------------------
// SkinnedMeshRenderer
// ---------------------------------------------------------------------------

SkinnedMeshRenderer::~SkinnedMeshRenderer() {
    if (vao_ != 0) glDeleteVertexArrays(1, &vao_);
    if (vbo_ != 0) glDeleteBuffers(1, &vbo_);
    if (ebo_ != 0) glDeleteBuffers(1, &ebo_);
    if (boneVbo_ != 0) glDeleteBuffers(1, &boneVbo_);
}

bool SkinnedMeshRenderer::Upload(const SkinnedMesh& mesh) {
    if (mesh.vertices.empty() || mesh.indices.empty()) return false;

    // 材质与子网格是绘制时需要的，网格本身不保存，
    // 所以这里要缓存一份（契约里 Draw 的参数里没有 mesh）。
    materials_ = mesh.materials;
    subMeshes_ = mesh.subMeshes;
    if (subMeshes_.empty()) {
        // 没有子网格信息时，整块网格当作一个子网格用第 0 个材质。
        SkinnedMesh::SubMesh sub;
        sub.indexOffset = 0;
        sub.indexCount = static_cast<std::uint32_t>(mesh.indices.size());
        sub.materialIndex = 0;
        subMeshes_.push_back(sub);
    }
    if (materials_.empty()) materials_.push_back(Material{});

    if (!UploadVertices(mesh)) return false;

    // 骨骼矩阵缓冲区：UBO。至少要放一个单位阵，
    // 否则没有骨骼的静态模型会因为越界读而全部塌到原点。
    boneCount_ = mesh.bones.size() > 0 ? mesh.bones.size() : 1;

    // 着色器里 uBones 数组的长度是编译期常量，UBO 的实际容量必须与之相同，
    // 否则 uniform block 会读到缓冲区之外的数据（未定义行为）。
    // 容量取驱动允许的上限（GL_MAX_UNIFORM_BLOCK_SIZE），一次分配够用。
    boneCapacity_ = static_cast<std::size_t>(QueryMaxBoneCount());
    if (boneCount_ > boneCapacity_) {
        std::fprintf(stderr,
                     "[渲染层] 警告：模型骨骼数 %zu 超过本机 uniform block 上限 %zu，"
                     "超出部分的骨骼将被忽略。\n",
                     boneCount_, boneCapacity_);
        boneCount_ = boneCapacity_;
    }

    if (boneVbo_ != 0) glDeleteBuffers(1, &boneVbo_);
    glGenBuffers(1, &boneVbo_);
    glBindBuffer(GL_UNIFORM_BUFFER, boneVbo_);
    // 整个缓冲区先填单位阵：模型实际用不到的骨骼槽位保持「无变换」，
    // 这样着色器即使读到那些槽位也不会把顶点拉歪。
    {
        std::vector<Mat4> identity(boneCapacity_, Mat4::Identity());
        glBufferData(GL_UNIFORM_BUFFER,
                     static_cast<GLsizeiptr>(identity.size() * sizeof(Mat4)), identity.data(),
                     GL_DYNAMIC_DRAW);
    }
    glBindBufferBase(GL_UNIFORM_BUFFER, kBoneBindingPoint, boneVbo_);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    PPET_GL_CHECK();
    return true;
}

void SkinnedMeshRenderer::SetTextures(std::vector<Texture> textures) {
    // 纹理只能移动不能拷贝（GL 名字是唯一句柄），
    // 所以这里按值收下再移动进成员，旧纹理随赋值被释放。
    textures_ = std::move(textures);
}

bool SkinnedMeshRenderer::UploadVertices(const SkinnedMesh& mesh) {
    if (vbo_ != 0) glDeleteBuffers(1, &vbo_);
    if (ebo_ != 0) glDeleteBuffers(1, &ebo_);
    if (vao_ != 0) glDeleteVertexArrays(1, &vao_);
    vbo_ = 0;
    ebo_ = 0;
    vao_ = 0;

    std::vector<GpuVertex> vertices;
    vertices.reserve(mesh.vertices.size());
    for (const Vertex& v : mesh.vertices) {
        GpuVertex g{};
        g.px = v.position.x;
        g.py = v.position.y;
        g.pz = v.position.z;
        g.nx = v.normal.x;
        g.ny = v.normal.y;
        g.nz = v.normal.z;
        g.u = v.uv.x;
        g.v = v.uv.y;
        g.r = v.color.x;
        g.g = v.color.y;
        g.b = v.color.z;
        g.a = v.color.w;
        g.j0 = v.joints[0];
        g.j1 = v.joints[1];
        g.j2 = v.joints[2];
        g.j3 = v.joints[3];
        g.w0 = v.weights[0];
        g.w1 = v.weights[1];
        g.w2 = v.weights[2];
        g.w3 = v.weights[3];
        vertices.push_back(g);
    }

    // 索引统一成「CW 为正面」：PMX 与 VRM 的绕序可能相反，
    // 统一之后主 pass 与描边 pass 的正面定义才是一致的。
    std::vector<std::uint32_t> indices = mesh.indices;
    if (NeedsWindingFlip(mesh)) {
        for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
            std::swap(indices[t + 1], indices[t + 2]);
        }
    }

    glGenVertexArrays(1, &vao_);
    if (vao_ == 0) return false;
    glBindVertexArray(vao_);

    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(vertices.size() * sizeof(GpuVertex)), vertices.data(),
                 GL_STATIC_DRAW);

    const GLsizei stride = static_cast<GLsizei>(sizeof(GpuVertex));
    // 位置
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
    // 法线
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(GpuVertex, nx)));
    // UV
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(GpuVertex, u)));
    // 顶点色
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(GpuVertex, r)));
    // 骨骼索引：整型属性，必须用 glVertexAttribIPointer。
    glEnableVertexAttribArray(4);
    glVertexAttribIPointer(4, 4, GL_UNSIGNED_INT, stride,
                           reinterpret_cast<void*>(offsetof(GpuVertex, j0)));
    // 骨骼权重
    glEnableVertexAttribArray(5);
    glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(GpuVertex, w0)));

    glGenBuffers(1, &ebo_);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
    // 顶点数可能超过 65535，索引必须是 32 位。
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(indices.size() * sizeof(std::uint32_t)), indices.data(),
                 GL_STATIC_DRAW);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    indexCount_ = indices.size();
    return true;
}

void SkinnedMeshRenderer::Draw(const Mat4& viewProjection, const std::vector<Mat4>& skinning,
                               const Vec3& lightDir) {
    DrawInternal(viewProjection, skinning, lightDir, false);
}

void SkinnedMeshRenderer::DrawOutline(const Mat4& viewProjection,
                                      const std::vector<Mat4>& skinning, const Vec3& lightDir,
                                      float widthScale) {
    // widthScale 是材质 outlineWidth 之外的额外缩放：
    // PMX / VRM 都已经把描边宽度换算成世界单位，这里传 1.0 即可。
    outlineScale_ = widthScale;
    DrawInternal(viewProjection, skinning, lightDir, true);
}

void SkinnedMeshRenderer::DrawInternal(const Mat4& vp, const std::vector<Mat4>& skinning,
                                       const Vec3& lightDir, bool outlinePass) {
    if (vao_ == 0 || indexCount_ == 0) return;

    std::string error;
    // 着色器里的 uBones 数组长度是编译期常量，必须与 UBO 实际分配的容量一致，
    // 否则 uniform block 读到的数据会超出缓冲区范围。
    const int boneCapacity = static_cast<int>(boneCapacity_);
    if (!shader_) {
        shader_ = std::make_shared<Shader>();
        if (!shader_->Compile(BuildToonVertexShader(boneCapacity), kToonFragmentShader, error)) {
            std::fprintf(stderr, "[渲染层] toon 着色器创建失败：\n%s\n", error.c_str());
            shader_.reset();
            return;
        }
    }
    if (outlinePass && !outlineShader_) {
        outlineShader_ = std::make_shared<Shader>();
        if (!outlineShader_->Compile(BuildOutlineVertexShader(boneCapacity), kOutlineFragmentShader,
                                    error)) {
            std::fprintf(stderr, "[渲染层] 描边着色器创建失败：\n%s\n", error.c_str());
            outlineShader_.reset();
            return;
        }
    }

    Shader* program = outlinePass ? outlineShader_.get() : shader_.get();
    if (program == nullptr) return;

    // 把 UBO 的块绑定到固定槽位。两个 program 各绑一次即可，
    // 但 program 换了之后必须重新绑定，所以放在这里。
    {
        const unsigned int block = glGetUniformBlockIndex(program->Id(), "BoneBlock");
        if (block != GL_INVALID_INDEX) {
            glUniformBlockBinding(program->Id(), block, kBoneBindingPoint);
        }
    }

    // 上传骨骼矩阵。数量不足时补单位阵，避免越界。
    {
        std::vector<Mat4> matrices(boneCount_, Mat4::Identity());
        const std::size_t count = std::min(matrices.size(), skinning.size());
        for (std::size_t i = 0; i < count; ++i) matrices[i] = skinning[i];
        glBindBuffer(GL_UNIFORM_BUFFER, boneVbo_);
        glBufferSubData(GL_UNIFORM_BUFFER, 0,
                        static_cast<GLsizeiptr>(matrices.size() * sizeof(Mat4)), matrices.data());
    }

    program->Use();
    program->SetMat4("uViewProjection", vp);
    program->SetVec3("uLightDir", lightDir);

    if (outlinePass) {
        program->SetFloat("uOutlineScale", outlineScale_);
    } else {
        // 从 VP 矩阵反解相机信息，供顶点着色器算视线方向。
        const ViewInfo view = ExtractViewInfo(vp);
        program->SetVec3("uCameraPos", view.cameraPos);
        program->SetVec3("uViewDirConst", view.viewDir);
        program->SetInt("uPerspective", view.perspective ? 1 : 0);
        // 纹理单元：基础色 0，toon 渐变 1，球面 2。
        program->SetInt("uBaseColorTex", 0);
        program->SetInt("uToonRampTex", 1);
        program->SetInt("uSphereTex", 2);
    }

    glBindVertexArray(vao_);
    // UBO 的绑定是全局状态，每次 draw 前重新绑定一次最稳妥。
    glBindBufferBase(GL_UNIFORM_BUFFER, kBoneBindingPoint, boneVbo_);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);

    // 绕序在上传时已统一成「CW 为正面」，所以这里固定用 GL_CW。
    // 主 pass 剔除背面（只留朝向相机的一面），
    // 描边 pass 剔除正面（只留背面）——外扩后的背面在本体轮廓之外露出一圈。
    glFrontFace(GL_CW);
    if (outlinePass) {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_FRONT);
        // polygon offset 把描边往后推一点，避免与本体 z-fighting。
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(1.0f, 1.0f);
    } else {
        glDisable(GL_POLYGON_OFFSET_FILL);
    }

    for (const SkinnedMesh::SubMesh& sub : subMeshes_) {
        if (sub.indexCount == 0) continue;
        const std::size_t materialIndex = static_cast<std::size_t>(
            sub.materialIndex >= 0 ? sub.materialIndex : 0);
        if (materialIndex >= materials_.size()) continue;
        const Material& material = materials_[materialIndex];

        if (outlinePass) {
            if (material.outlineWidth <= 0.0f) continue;
            program->SetFloat("uOutlineWidth", material.outlineWidth);
            program->SetVec3("uOutlineColor", SrgbToLinear(material.outlineColor));
            // 描边是实心的，不参与 alpha 混合。
            glDisable(GL_BLEND);
            glDepthMask(GL_TRUE);
        } else {
            ApplyMaterial(material);
        }

        const GLvoid* offset =
            reinterpret_cast<const void*>(static_cast<std::uintptr_t>(sub.indexOffset) *
                                          sizeof(std::uint32_t));
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(sub.indexCount), GL_UNSIGNED_INT, offset);
    }

    glBindVertexArray(0);

    // 恢复状态：主 pass 的默认剔除方式是背面剔除。
    glFrontFace(GL_CW);
    glCullFace(GL_BACK);
    glDisable(GL_POLYGON_OFFSET_FILL);
}

void SkinnedMeshRenderer::ApplyMaterial(const Material& material) {
    if (!shader_) return;

    // 全部颜色都在线性空间参与光照运算，输出前再统一编码回 sRGB。
    // 材质颜色来自 PMX / MToon 的作者值（sRGB），所以要先转成线性；
    // alpha 是「不透明度」而不是颜色，不做 gamma 转换。
    const Vec3 diffuseLinear = SrgbToLinear(Vec3(material.diffuse.x, material.diffuse.y,
                                                 material.diffuse.z));
    shader_->SetVec4("uDiffuse", diffuseLinear.x, diffuseLinear.y, diffuseLinear.z,
                     material.diffuse.w);
    shader_->SetVec3("uAmbient", SrgbToLinear(material.ambient));
    shader_->SetVec3("uEmissive", SrgbToLinear(material.emissive));
    shader_->SetVec3("uShadeColor", SrgbToLinear(material.shadeColor));
    shader_->SetVec3("uRimColor", SrgbToLinear(material.rimColor));
    shader_->SetVec3("uSpecular", SrgbToLinear(material.specular));
    shader_->SetFloat("uShininess", material.shininess);
    shader_->SetFloat("uShadeShift", material.shadeShift);
    shader_->SetFloat("uShadeToony", material.shadeToony);
    shader_->SetFloat("uRimLightingMix", material.rimLightingMix);
    shader_->SetFloat("uRimFresnelPower", material.rimFresnelPower);
    shader_->SetFloat("uLightIntensity", material.lightIntensity);
    shader_->SetInt("uUseToon", material.useToon ? 1 : 0);
    shader_->SetInt("uUnlit", material.unlit ? 1 : 0);

    // 纹理绑定：索引越界或贴图无效时按「无贴图」处理。
    const bool hasBase = BindTexture(material.baseColorTex, 0);
    const bool hasRamp = BindTexture(material.toonRampTex, 1);
    const bool hasSphere = BindTexture(material.sphereTex, 2);
    shader_->SetInt("uHasBaseTex", hasBase ? 1 : 0);
    shader_->SetInt("uHasToonRamp", hasRamp ? 1 : 0);
    shader_->SetInt("uHasSphereTex", hasSphere ? 1 : 0);

    // Alpha 混合：桌宠需要透明叠加。
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    // 深度写入：全不透明时写深度（保证遮挡关系正确），
    // 半透明时不写深度（否则后面的部件会被挡掉）。
    const bool opaque = material.diffuse.w >= 0.999f;
    glDepthMask(opaque ? GL_TRUE : GL_FALSE);

    // 双面材质（PMX 的「关闭背面剔除」标志）要关掉背面剔除。
    if (material.doubleSided) {
        glDisable(GL_CULL_FACE);
    } else {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
    }
}

bool SkinnedMeshRenderer::BindTexture(int index, unsigned int unit) const {
    if (index < 0 || static_cast<std::size_t>(index) >= textures_.size()) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, 0);
        return false;
    }
    const Texture& texture = textures_[static_cast<std::size_t>(index)];
    if (!texture.IsValid()) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, 0);
        return false;
    }
    texture.Bind(unit);
    return true;
}

}  // namespace ppet
