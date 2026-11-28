// PPet - C++ 移植版
// Toon / 描边着色器 GLSL 源码（报告 2.3.2：从 Renderer.cpp 抽出，降低巨型 TU 体积）。
//
// 纯字符串常量与构造函数，header-only（inline），无 GL 依赖：
//   - BuildToonVertexShader(maxBones) / kToonFragmentShader
//   - BuildOutlineVertexShader(maxBones) / kOutlineFragmentShader
// 放在独立头文件后，Renderer.cpp 从混杂「stb 实现 + GLSL + 三个类」变为只保留
// 业务类；GLSL 也可被未来的着色器单测直接引用而无需链接整个渲染层。
#pragma once

#include <string>

namespace ppet::render {

/// 主渲染（toon）顶点着色器。maxBones 拼进源码（uniform block 数组长度须为编译期常量）。
inline std::string BuildToonVertexShader(int maxBones) {
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
inline constexpr char kToonFragmentShader[] = R"(#version 330 core

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
uniform int uAlphaTest;     ///< 1 = MASK 模式，按 alphaCutoff 丢弃片元
uniform float uAlphaCutoff; ///< MASK 模式的 alpha 阈值

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

    // MASK 模式（VRM 头发/睫毛/裙边的镂空贴图）：丢弃 alpha 低于阈值的片元，
    // 否则镂空区域会渲染成不透明的实心块，模型看起来「破相」。
    if (uAlphaTest == 1 && base.a < uAlphaCutoff) discard;

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

/// 描边顶点着色器：沿法线外扩顶点位置。maxBones 拼进源码（数组长度须为编译期常量）。
inline std::string BuildOutlineVertexShader(int maxBones) {
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
inline constexpr char kOutlineFragmentShader[] = R"(#version 330 core

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

}  // namespace ppet::render
