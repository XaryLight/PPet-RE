// PPet - C++ 移植版
// 模型抽象接口：Live2D / PMX / VRM 三种后端统一由此驱动。
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <src/tools/core/Math.hpp>
#include <src/tools/model/SkinnedMesh.hpp>

namespace ppet {

enum class ModelType {
    Unknown,
    Live2D,  ///< .model3.json / .moc3
    Pmx,     ///< .pmx
    Vrm,     ///< .vrm
};

/// 模型类型可读名（"PMX" / "VRM" / "Live2D" / "未知"）。
/// 定义在头文件里：调试面板等轻量目标只需要这个名字，不必链接整个模型层。
inline const char* ModelTypeName(ModelType type) {
    switch (type) {
        case ModelType::Live2D:  return "Live2D";
        case ModelType::Pmx:     return "PMX";
        case ModelType::Vrm:     return "VRM";
        case ModelType::Unknown: break;
    }
    return "未知";
}

/// 由文件后缀推断模型类型。
ModelType DetectModelType(const std::string& path);

/// 渲染环境信息，描述当前窗口与全局鼠标状态。
struct RenderContext {
    int viewportWidth = 0;
    int viewportHeight = 0;
    float deltaTime = 0.0f;

    /// 全局鼠标位置（虚拟桌面坐标系，单位：像素）。
    /// 这是「全屏追踪」的核心数据源：不受模型窗口边界限制。
    Vec2 globalCursor{0.0f, 0.0f};

    /// 模型窗口中心在虚拟桌面中的位置（像素），用于把全局鼠标换算到模型局部空间。
    Vec2 windowOrigin{0.0f, 0.0f};
    Vec2 windowSize{0.0f, 0.0f};

    /// 模型在屏幕上的包围盒（像素），用于判断鼠标是否在模型附近。
    Vec2 modelScreenCenter{0.0f, 0.0f};
    float modelScreenRadius = 0.0f;

    /// 全局鼠标归一化到模型局部空间的坐标，范围约 [-1, 1]。
    /// x 向右为正，y 向上为正（OpenGL 约定）。
    Vec2 localCursor{0.0f, 0.0f};

    /// 全局鼠标是否有效（例如在无头环境或权限不足时为 false）。
    bool hasGlobalCursor = false;

    /// 渲染缩放：>1 表示模型在窗口中显示得更大，<1 表示更小。
    /// 取景时用它调整正交视体的半高（除以该系数即等比放大模型）。
    float renderScale = 1.0f;
};

/// 模型诊断快照：调试面板显示的全部内容都来自这里。
///
/// 分成四组，对应调试面板的四个区域：
///   概览 / 参数（规模）/ 性质（格式特征）/ 渲染状态。
/// 由各后端在 DebugInfo() 里填充；后端不提供的字段保持默认值，
/// 因此新增模型格式时调试面板不会崩，只是信息少一些。
struct ModelDebugInfo {
    // --- 概览 ---
    std::string modelId;          ///< 稳定标识（路径 + 类型哈希，用于日志/诊断对照）
    std::string displayName;      ///< 模型显示名
    std::string path;             ///< 实际加载的文件路径
    std::string sourceArchive;    ///< 由 zip 包导入时的原始压缩包路径（否则为空）
    ModelType type = ModelType::Unknown;
    bool loaded = false;

    // --- 参数（规模）---
    std::size_t vertexCount = 0;
    std::size_t indexCount = 0;
    std::size_t triangleCount = 0;
    std::size_t boneCount = 0;
    std::size_t materialCount = 0;
    std::size_t subMeshCount = 0;
    std::size_t morphCount = 0;
    std::size_t textureReferenced = 0;  ///< 模型引用的贴图总数
    std::size_t textureLoaded = 0;      ///< 成功解码/上传的贴图数
    std::size_t drawCalls = 0;          ///< 最近一帧的 draw call 数
    Vec3 boundsMin{0.0f, 0.0f, 0.0f};
    Vec3 boundsMax{0.0f, 0.0f, 0.0f};
    float modelHeight = 0.0f;

    // --- 性质 ---
    std::string formatVersion;   ///< "PMX 2.0" / "VRM 1.0" ...
    std::string textEncoding;    ///< "UTF-16LE" / "UTF-8" / "内嵌(glTF)"
    bool doubleSided = false;
    bool hasOutline = false;
    bool isSkinned = false;
    /// 额外的「键=值」性质（如坐标系、渲染管线、控制骨骼命中情况）。
    std::vector<std::string> properties;
    /// 模型引用但磁盘上找不到的资源（PMX 贴图最常见）。
    std::vector<std::string> missingResources;
    /// 模型的「部分」清单（材质/子网格，调试面板的列表来源）。
    struct Part {
        std::string name;
        std::size_t indexCount = 0;
        int materialIndex = -1;
        std::string detail;
    };
    std::vector<Part> parts;

    /// 骨架明细（调试面板「骨架信息」页）：逐根骨骼的名称、父级、层级与属性。
    struct BoneInfo {
        int index = -1;
        std::string name;
        int parentIndex = -1;
        int depth = 0;
        std::string flags;        ///< "可位移 / IK / 控制骨骼" 等
    };
    std::vector<BoneInfo> bones;

    /// 图片明细（调试面板「图片渲染」页）：贴图路径可用时可直接出缩略图。
    struct TextureInfo {
        std::string name;         ///< 模型里记录的名字/相对路径
        std::string path;         ///< 磁盘绝对路径（PMX）；内嵌贴图为空
        int width = 0;
        int height = 0;
        bool loaded = false;
        std::string note;         ///< 如「内嵌于 .vrm / 请查看渲染帧」
    };
    std::vector<TextureInfo> textures;

    // --- 渲染状态 ---
    bool gpuReady = false;        ///< GPU 资源是否已建立
    bool renderingOk = false;     ///< 综合判定：能否正常出图
    unsigned int glError = 0;     ///< 最近一次 GL 错误码（0 = 无错误）
    std::string renderStatus;     ///< 可读结论，例如「正常渲染」「贴图缺失 3 张，可能白模」

    /// 动作系统状态（Live2D 待机/互动动作）。例如「待机动作：Idle 1.2s」「互动动作：TapHead」。
    /// 其它后端为空字符串，诊断面板据此决定是否显示该行。
    std::string motionStatus;
    /// 动作组/命中区摘要（如「动作组 Idle×3 / TapHead×2 / TapBody×1，命中区 2」）。
    std::string motionSummary;

    // --- 错误（详细）---
    std::string errorCategory;    ///< 最近一次加载/渲染问题的分类名
    std::vector<std::string> errorDetails;  ///< 逐条明细，供面板与弹窗显示
};

/// 模型后端接口。
class IModel {
public:
    virtual ~IModel() = default;

    /// 加载模型。返回 false 时 error 中写入可读的错误信息。
    virtual bool Load(const std::string& path, std::string& error) = 0;

    /// 每帧更新（动画、视线追踪、弹簧骨等）。
    virtual void Update(const RenderContext& ctx) = 0;

    /// 绘制。调用方已设置好视口与清屏状态。
    virtual void Render(const RenderContext& ctx) = 0;

    /// 窗口尺寸变化。
    virtual void OnResize(int width, int height) = 0;

    /// 模型显示名（用于 UI 显示）。
    virtual std::string DisplayName() const = 0;

    /// 模型类型。
    virtual ModelType Type() const = 0;

    /// 是否成功加载。
    virtual bool IsLoaded() const = 0;

    /// 诊断快照（供调试面板）。默认实现只填概览，各后端可重写补充细节。
    /// 内联在头文件里：ppet_model 的很多测试目标并不编译 Model.cpp，
    /// 放在 .cpp 会导致 undefined reference / typeinfo for IModel。
    virtual ModelDebugInfo DebugInfo() const {
        ModelDebugInfo info;
        info.displayName = DisplayName();
        info.type = Type();
        info.loaded = IsLoaded();
        info.renderingOk = info.loaded;
        info.renderStatus = info.loaded ? "已加载（后端未提供详细诊断）" : "未加载模型";
        return info;
    }

    /// 模型引用但不存在的资源路径列表（供日志与诊断）。
    /// PMX 用它报告缺失贴图，并给出「请打包成 .zip」的建议。
    /// 默认返回空（VRM/Live2D 的图形数据内嵌在文件里）。
    virtual std::vector<std::string> MissingResources() const { return {}; }

    /// 用户点击了渲染窗口（归一化坐标 nx, ny ∈ [0,1]，左上原点、y 向下）。
    /// Live2D 后端据此按命中区触发互动动作；其它后端默认忽略。
    /// @return 是否触发了互动动作。
    virtual bool OnInteract(float nx, float ny) {
        (void)nx;
        (void)ny;
        return false;
    }
};

using ModelPtr = std::shared_ptr<IModel>;

/// 创建对应类型的模型实例。未找到后端时返回 nullptr。
ModelPtr CreateModel(ModelType type);

/// 从统一的 SkinnedMesh 填充诊断信息的「参数」部分（各后端复用）。
/// 同样内联，理由见 IModel::DebugInfo。
inline void FillMeshDebugInfo(ModelDebugInfo& info, const SkinnedMesh& mesh) {
    info.vertexCount = mesh.vertices.size();
    info.indexCount = mesh.indices.size();
    info.triangleCount = mesh.indices.size() / 3;
    info.boneCount = mesh.bones.size();
    info.materialCount = mesh.materials.size();
    info.subMeshCount = mesh.subMeshes.size();
    info.morphCount = mesh.morphs.size();
    info.boundsMin = mesh.boundsMin;
    info.boundsMax = mesh.boundsMax;
    info.modelHeight = mesh.modelHeight;
    info.isSkinned = mesh.isSkinned;

    for (const Material& material : mesh.materials) {
        if (material.doubleSided) info.doubleSided = true;
        if (material.outlineWidth > 0.0f || material.edgeScale > 0.0f) info.hasOutline = true;
    }

    info.parts.clear();
    info.parts.reserve(mesh.subMeshes.size());
    for (const SkinnedMesh::SubMesh& sub : mesh.subMeshes) {
        ModelDebugInfo::Part part;
        part.indexCount = sub.indexCount;
        part.materialIndex = sub.materialIndex;
        if (sub.materialIndex >= 0 &&
            static_cast<std::size_t>(sub.materialIndex) < mesh.materials.size()) {
            const Material& material = mesh.materials[static_cast<std::size_t>(sub.materialIndex)];
            part.name = material.name.empty()
                            ? "材质 #" + std::to_string(sub.materialIndex)
                            : material.name;
            part.detail = "三角形 " + std::to_string(sub.indexCount / 3) +
                          " / 贴图 " + std::to_string(material.baseColorTex) +
                          (material.doubleSided ? " / 双面" : " / 单面") +
                          (material.outlineWidth > 0.0f ? " / 描边" : "");
        } else {
            part.name = "子网格 #" + std::to_string(info.parts.size());
            part.detail = "三角形 " + std::to_string(sub.indexCount / 3);
        }
        info.parts.push_back(std::move(part));
    }

    // --- 骨架明细 ---
    // 调试面板要逐根骨骼显示「名称 / 父级 / 层级 / 属性」，这里统一从
    // SkinnedMesh 生成，PMX / VRM 两种后端无需各写一遍。
    // 层级用父链长度计算，带 guard 防止异常数据造成死循环。
    info.bones.clear();
    info.bones.reserve(mesh.bones.size());
    for (std::size_t i = 0; i < mesh.bones.size(); ++i) {
        const Bone& b = mesh.bones[i];
        ModelDebugInfo::BoneInfo out;
        out.index = static_cast<int>(i);
        out.name = b.name.empty() ? ("骨骼 #" + std::to_string(i)) : b.name;
        out.parentIndex = b.parent;

        int depth = 0;
        int parent = b.parent;
        for (int guard = 0;
             parent >= 0 && parent < static_cast<int>(mesh.bones.size()) && guard < 256;
             ++guard) {
            ++depth;
            parent = mesh.bones[static_cast<std::size_t>(parent)].parent;
        }
        out.depth = depth;

        if (b.hasTranslation) out.flags += "可位移 ";
        if (b.ikTarget >= 0) out.flags += "IK ";
        if (out.flags.empty()) out.flags = "-";
        info.bones.push_back(std::move(out));
    }
}

}  // namespace ppet
