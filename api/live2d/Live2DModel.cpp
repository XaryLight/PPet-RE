// PPet - C++ 移植版
// Live2D 模型后端实现。
#include <api/live2d/Live2DModel.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

#include <src/tools/core/FileIO.hpp>

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
#include <src/tools/render/RenderUtils.hpp>

namespace ppet::live2d {
namespace {

namespace cubism = ppet::live2d::cubism;

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

void main() {
    gl_Position = u_matrix * vec4(a_position, 0.0, 1.0);
    // Live2D 的 UV 原点在左上，OpenGL 纹理坐标原点在左下，需要翻转 V。
    v_texCoord = vec2(a_texCoord.x, 1.0 - a_texCoord.y);
}
)";

/// 片元着色器（GLSL 330 core）。
///
/// 输出 premultiplied alpha：桌宠窗口是透明叠加窗口，
/// 需要用 GL_ONE / GL_ONE_MINUS_SRC_ALPHA 混合，
/// 因此 RGB 必须预先乘以 alpha。
const char* kFragmentShader = R"(#version 330 core
in vec2 v_texCoord;

uniform sampler2D s_texture0;
uniform vec4 u_baseColor;

out vec4 fragColor;

void main() {
    vec4 texColor = texture(s_texture0, v_texCoord);
    // premultiplied alpha：RGB 预乘 alpha。
    fragColor = vec4(texColor.rgb * texColor.a, texColor.a) * u_baseColor;
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

    /// 白色占位纹理。
    GLuint whiteTexture = 0;

    /// 上一帧的部件数量，用于检测模型结构变化。
    int drawableCount = 0;
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

    loaded_ = true;
    return true;
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
    impl_->motionGroups.clear();

    std::vector<MotionGroup> directorGroups;

    // 动作组：manifest.motionGroups 里已经解析出了绝对路径与组名。
    // 旧实现把组名丢掉、把所有动作混进一个扁平数组随机播放，导致
    // 「互动动作在待机时自己冒出来、点击也没有反应」。现在按组名分类，
    // 交给 MotionDirector 做 Idle 待机循环 + 互动优先级调度。
    for (const Live2DMotionGroup& group : impl_->manifest.motionGroups) {
        MotionGroup dg;
        dg.name = group.name;
        dg.kind = ClassifyMotionGroup(group.name);
        dg.hitArea = HitAreaFromGroupName(group.name, dg.kind);
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
            dg.motionIndices.push_back(static_cast<int>(impl_->motions.size()));
            impl_->motions.push_back(std::move(motion));
        }
        if (!dg.motionIndices.empty()) {
            impl_->motionGroups.push_back(dg.motionIndices);
            directorGroups.push_back(std::move(dg));
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
    durations.reserve(impl_->motions.size());
    fadeIns.reserve(impl_->motions.size());
    fadeOuts.reserve(impl_->motions.size());
    for (const Motion& m : impl_->motions) {
        durations.push_back(m.duration);
        fadeIns.push_back(m.fadeInTime);
        fadeOuts.push_back(m.fadeOutTime);
    }

    motionDirector_.Configure(std::move(directorGroups), std::move(hitAreaNames),
                              impl_->motions.size());
    motionDirector_.SetMotionMeta(durations, fadeIns, fadeOuts);
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

    ApplyTracking(ctx);
    ApplyEyeBlink(dt);
    ApplyBreathing(dt);
    UpdateMotion(dt);

    // 依据参数重算顶点。
    cubism::UpdateModel(impl_->model);
}

void Live2DModel::ApplyTracking(const RenderContext& ctx) {
    if (!impl_ || !impl_->model) return;
    if (!ctx.hasGlobalCursor) return;

    // 视线追踪：把归一化的鼠标位置映射到模型参数。
    //
    // 【参数映射表】
    //   ParamAngleX     = localCursor.x * maxTrackingAngle   （头部左右）
    //   ParamAngleY     = localCursor.y * maxTrackingAngle   （头部上下）
    //   ParamEyeBallX   = localCursor.x * maxTrackingAngle   （眼球左右）
    //   ParamEyeBallY   = localCursor.y * maxTrackingAngle   （眼球上下）
    //   ParamBodyAngleX = localCursor.x * maxTrackingAngle   （身体左右）
    //   ParamBodyAngleZ = localCursor.x * maxTrackingAngle   （身体倾斜）
    //
    // 重要：很多模型**没有**眼球参数或身体参数，
    // 因此每个参数都必须先检查是否存在，不存在就跳过，
    // 绝不能给不存在的参数写值（Core 会直接忽略，但索引越界会出问题）。
    const float angle = maxTrackingAngle_;
    const float cursorX = ctx.localCursor.x;
    const float cursorY = ctx.localCursor.y;

    auto set = [&](int index, float value) {
        if (index < 0) return;
        // 参数值要落在模型允许的范围内，否则 Core 会把它裁剪掉，
        // 表现为「转头到一半就不动了」。
        float minValue = -1.0f;
        float maxValue = 1.0f;
        if (cubism::ParameterRange(impl_->model, index, minValue, maxValue)) {
            value = Clamp(value, minValue, maxValue);
        }
        cubism::SetParameterValue(impl_->model, index, value);
    };

    set(paramAngleX_, cursorX * angle);
    set(paramAngleY_, cursorY * angle);
    set(paramEyeBallX_, cursorX * angle);
    set(paramEyeBallY_, cursorY * angle);
    set(paramBodyAngleX_, cursorX * angle);
    set(paramBodyAngleZ_, cursorX * angle);
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

    const MotionBlend blend = motionDirector_.Blend();
    if (blend.motionIndex < 0 || blend.weight <= 0.0f) return;
    if (blend.motionIndex >= static_cast<int>(impl_->motions.size())) return;

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

        // 与「当前基线」按 fade 权重混合：基线已包含视线/眨眼/呼吸与默认值，
        // 因此动作淡入/淡出时姿态平滑过渡，淡出结束自然回到待机，不会跳变。
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

bool Live2DModel::OnInteract(float nx, float ny) {
    if (!loaded_) return false;
    return motionDirector_.Interact(nx, ny);
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
    // Cubism Core 的顶点坐标（csmGetDrawableVertexPositions）单位是「模型单位」
    // （model unit），不是画布像素。以 aidang_2 实测：
    //   sizeInPixels = (3400, 4100)、pixelsPerUnit = 186.36
    //   -> 画布尺寸 = sizeInPixels / pixelsPerUnit = (18.24, 22.0) 模型单位
    //   -> 顶点范围约 (-7.8, -8.9) ~ (5.0, 10.7) 模型单位
    // 因此换算必须除以 pixelsPerUnit，否则模型会被缩小上百倍（看起来只有一个小点）。
    //
    // 官方 Framework 的 GetCanvasWidth() 同样是 sizeInPixels / pixelsPerUnit，
    // 可作为权威依据（CubismModel.cpp）。
    //
    // 顶点已以模型原点为中心、Y 轴向上，与 OpenGL 裁剪空间一致，无需翻转。
    const cubism::CanvasInfo& canvas = impl_->canvas;
    const float ppu = canvas.pixelsPerUnit > 0.0f ? canvas.pixelsPerUnit : 1.0f;
    const float W = canvas.sizeInPixels.x / ppu;  // 模型单位下的画布宽
    const float H = canvas.sizeInPixels.y / ppu;  // 模型单位下的画布高

    const float vw = ctx.viewportWidth > 0 ? static_cast<float>(ctx.viewportWidth) : 1.0f;
    const float vh = ctx.viewportHeight > 0 ? static_cast<float>(ctx.viewportHeight) : 1.0f;

    // 等比适配：模型单位 -> 窗口像素的缩放系数（取较小边铺满，留 5% 边距）。
    // 顶点以模型原点为中心，因此平移为 0，模型自然居中。
    const float fit = 0.95f;
    const float k = std::min(vw / (W > 0.0f ? W : 1.0f),
                              vh / (H > 0.0f ? H : 1.0f)) * fit * ctx.renderScale;

    const float sx = k / (vw * 0.5f);
    const float sy = k / (vh * 0.5f);

    Mat4 m = Mat4::Identity();
    m.At(0, 0) = sx;
    m.At(1, 1) = sy;
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
    }

    const Mat4 transform = BuildTransform(ctx);

    // --- GL 状态 ---
    glUseProgram(impl_->program);
    glUniformMatrix4fv(impl_->uniformMatrix, 1, GL_FALSE, transform.m);
    glUniform1i(impl_->uniformTexture, 0);
    glActiveTexture(GL_TEXTURE0);

    // 桌宠窗口是透明叠加窗口：premultiplied alpha 混合。
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    // --- 按渲染顺序绘制 ---
    //
    // Core 的 RenderOrder 与 DrawOrder 不同：前者是考虑了遮罩关系之后的
    // 实际绘制顺序，必须按它排序，否则部件的遮挡关系会错。
    std::vector<int> order(static_cast<std::size_t>(std::max(0, drawableCount)));
    for (int i = 0; i < drawableCount; ++i) order[static_cast<std::size_t>(i)] = i;
    std::stable_sort(order.begin(), order.end(), [this](int a, int b) {
        cubism::Drawable da;
        cubism::Drawable db;
        const bool oka = cubism::GetDrawable(impl_->model, a, da);
        const bool okb = cubism::GetDrawable(impl_->model, b, db);
        if (!oka || !okb) return a < b;
        return da.renderOrder < db.renderOrder;
    });

    for (int index : order) {
        DrawDrawable(index, transform);
    }

    glUseProgram(0);
}

void Live2DModel::DrawDrawable(int index, const Mat4& transform) {
    cubism::Drawable drawable;
    if (!cubism::GetDrawable(impl_->model, index, drawable)) return;
    if (!drawable.visible) return;
    if (drawable.opacity <= 0.0f) return;
    if (!drawable.positions || !drawable.uvs || !drawable.indices) return;

    // transform（模型视图矩阵）在 BuildTransform 中给出，用于把模型放到屏幕位置；
    // 当前顶点已按画布坐标系上传，绘制暂未显式乘入该矩阵，保留参数以便后续接入。
    (void)transform;

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

    // --- 混合模式 ---
    if (drawable.additiveBlend) {
        // 加法混合：官方公式 O_rgb = D_rgb + S_rgb。
        glBlendFunc(GL_ONE, GL_ONE);
    } else if (drawable.multiplicativeBlend) {
        // 乘法混合：官方公式 O_rgb = D_rgb * (1 - S_a) + S_rgb * D_rgb。
        glBlendFunc(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    }

    // --- 纹理 ---
    GLuint texture = impl_->whiteTexture;
    const int textureIndex = drawable.textureIndex;
    if (textureIndex >= 0 && textureIndex < static_cast<int>(impl_->textures.size())) {
        texture = impl_->textures[static_cast<std::size_t>(textureIndex)];
    }
    if (!texture) return;
    glBindTexture(GL_TEXTURE_2D, texture);

    // 部件不透明度通过颜色 uniform 传入。
    glUniform4f(impl_->uniformBaseColor, 1.0f, 1.0f, 1.0f, drawable.opacity);

    glDrawElements(GL_TRIANGLES, drawable.indexCount, GL_UNSIGNED_SHORT, nullptr);
    glBindVertexArray(0);
}

void Live2DModel::ReleaseGraphics() {
    if (!impl_) return;

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
