// PPet - C++ 移植版
// PMX 模型后端实现：PMX -> SkinnedMesh 转换、待机动画、视线追踪、toon 渲染。
//
// 设计要点：
//   1. PMX 的坐标是左手系，转换到右手系时除了取反 z，还必须反转三角形绕序，
//      否则背面剔除会把模型剔掉（详见 PmxModel.hpp 顶部注释）。
//   2. PMX 的骨骼是「绝对坐标 + 无旋转」，因此绑定矩阵只有平移；
//      局部绑定姿态 = 骨骼绝对位置 - 父骨骼绝对位置。
//   3. PMX 允许父骨骼索引大于自身，且可能出现环，因此加载时做拓扑排序，
//      把骨骼重排成「父在前、子在后」，并同步重映射顶点里的骨骼索引。
//      重排后的顺序也是 ComputeSkinningMatrices 逐级累积世界矩阵所依赖的前提。
//   4. 视线追踪找不到对应骨骼时必须优雅降级（跳过该骨骼），不能崩。
#include <src/tools/model/pmx/PmxModel.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <unordered_map>

#include <src/tools/core/FileIO.hpp>

namespace ppet::pmx {
namespace {

// ---------------------------------------------------------------------------
// 常量
// ---------------------------------------------------------------------------

/// 头部转动的最大角度（弧度）。约 25 度，超过后表情会显得很夸张。
constexpr float kMaxHeadYaw = 0.4363f;
/// 头部俯仰的最大角度（弧度）。约 12 度。
constexpr float kMaxHeadPitch = 0.2094f;
/// 眼球转动的最大角度（弧度），比头部小得多。
constexpr float kMaxEyeYaw = 0.2443f;
constexpr float kMaxEyePitch = 0.1396f;

/// 视线角度的平滑系数（每秒衰减到 1/e 的比例）。
constexpr float kGazeSmoothing = 9.0f;

/// 待机摆动的幅度（弧度）与频率（弧度/秒）。
constexpr float kIdleSwayYawAmp = 0.0262f;    // 约 1.5 度
constexpr float kIdleSwayPitchAmp = 0.0131f;  // 约 0.75 度
constexpr float kIdleSwaySpeed = 0.7f;
constexpr float kIdleBreathSpeed = 1.6f;

/// 描边宽度归一化基准：PMX 的 edgeSize 以「像素」为单位（1.0 ≈ 1 像素），
/// 这里换算成世界单位。取「模型高度 / 1000」作为 1 像素对应的世界尺度，
/// 这样不同大小的模型描边粗细看起来一致。
constexpr float kEdgePixelsToWorld = 1000.0f;

/// 取景时模型四周留出的空白比例。
constexpr float kFramingMargin = 1.12f;

/// 灯光方向（世界空间，指向光源）：来自模型前上方偏左。
const Vec3 kLightDir = Normalize(Vec3(-0.45f, 0.75f, 0.55f));

/// PMX 材质面板编号 -> 表情分类名。
const char* MorphPanelCategory(std::uint8_t panel) {
    switch (panel) {
        case 1: return "eyebrow";
        case 2: return "eye";
        case 3: return "mouth";
        case 4: return "other";
        default: return "";
    }
}

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

/// ASCII 小写化（只处理 ASCII，UTF-8 多字节序列原样保留）。
std::string AsciiLower(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

/// 名称包含判断：ASCII 部分忽略大小写，UTF-8 多字节部分按字节匹配
/// （UTF-8 具有自同步特性，直接子串查找是安全的）。
bool NameContains(const std::string& name, const char* needle) {
    if (name.empty() || needle == nullptr || *needle == '\0') return false;
    const std::string lower = AsciiLower(name);
    const std::string target = AsciiLower(needle);
    return lower.find(target) != std::string::npos;
}

/// 取路径中目录部分（不含末尾分隔符）。没有目录时返回空串。
std::string DirNameOf(const std::string& path) {
    const std::size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return std::string();
    if (pos == 0) return "/";
    return path.substr(0, pos);
}

/// 把 PMX 里的贴图路径（通常用反斜杠）转换成当前平台可用的路径。
/// PMX 里的贴图路径是「相对于 .pmx 所在目录」的 Windows 风格路径：
///   tex\\body.png / .\\tex\\body.png / ..\\shared\\toon.bmp / D:\\tex\\a.png
/// 统一成 '/' 分隔并去掉开头的 "./"，Linux 上才能正确拼接。
std::string NormalizeTexturePath(const std::string& raw) {
    std::string out = raw;
    for (char& c : out) {
        if (c == '\\') c = '/';
    }
    while (out.size() >= 2 && out[0] == '.' && out[1] == '/') out.erase(0, 2);
    return out;
}

std::string LowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool IsAbsoluteTexturePath(const std::string& path) {
    if (!path.empty() && path[0] == '/') return true;
    return path.size() >= 2 && path[1] == ':';
}

/// 预建「模型目录下所有文件」的索引，用于贴图路径的大小写/目录容错。
///
/// 社区 PMX 包里贴图的实际大小写、所在子目录经常与 .pmx 里记录的不一致
/// （Windows 不区分大小写且作者常改目录结构）。MMD 在 Windows 上能显示，
/// 换到 Linux 就全变纯色。这里建两份索引：
///   byRelative —— 小写相对路径 -> 实际路径（精确匹配，大小写不敏感）
///   byName     —— 小写文件名   -> 实际路径（目录不对时按文件名兜底）
/// 只在 Load() 时遍历一次目录，避免每张贴图都递归扫描。
struct TextureFileIndex {
    std::unordered_map<std::string, std::string> byRelative;
    std::unordered_map<std::string, std::string> byName;
};

TextureFileIndex BuildTextureFileIndex(const std::string& root) {
    namespace fs = std::filesystem;
    TextureFileIndex index;
    if (root.empty()) return index;

    std::error_code ec;
    const fs::path rootPath = ToFsPath(root);
    if (!fs::is_directory(rootPath, ec)) return index;

    for (fs::recursive_directory_iterator it(rootPath, ec), end; it != end && !ec;
         it.increment(ec)) {
        if (ec) break;
        if (!fs::is_regular_file(it->path(), ec)) continue;
        const fs::path relFs = fs::relative(it->path(), rootPath, ec);
        if (ec) {
            ec.clear();
            continue;
        }
        const std::string rel = FromFsPathGeneric(relFs);
        const std::string full = FromFsPathGeneric(it->path());
        index.byRelative.emplace(LowerAscii(rel), full);
        index.byName.emplace(LowerAscii(FileName(rel)), full);
    }
    return index;
}

/// 把 PMX 记录的贴图路径解析成磁盘上的真实路径。
/// 找不到时仍返回拼接出的路径（调用方据此报告「缺失资源」）。
std::string ResolveTexturePath(const std::string& baseDir, const std::string& raw,
                               const TextureFileIndex& index) {
    const std::string rel = NormalizeTexturePath(raw);
    if (rel.empty()) return std::string();
    if (IsAbsoluteTexturePath(rel)) return rel;

    const std::string direct = baseDir.empty() ? rel : JoinPath(baseDir, rel);
    if (FileExists(direct)) return direct;  // 绝大多数情况走这里

    if (auto it = index.byRelative.find(LowerAscii(rel)); it != index.byRelative.end()) {
        return it->second;  // 大小写不一致
    }
    if (auto it = index.byName.find(LowerAscii(FileName(rel))); it != index.byName.end()) {
        return it->second;  // 目录层级不一致，按文件名兜底
    }
    return direct;
}

/// 归一化权重：把 weights 缩放到总和为 1（全 0 时退化为第一个骨骼权重 1）。
void NormalizeWeights(std::array<float, 4>& weights) {
    const float sum = weights[0] + weights[1] + weights[2] + weights[3];
    if (sum <= kEpsilon) {
        weights = {1.0f, 0.0f, 0.0f, 0.0f};
        return;
    }
    const float inv = 1.0f / sum;
    for (float& w : weights) w *= inv;
}

/// 计算「父在前、子在后」的骨骼顺序。
///
/// PMX 允许父骨骼索引大于自身索引，甚至存在环，因此不能直接按文件顺序遍历。
/// 算法：先算出每个骨骼的子骨骼数量，从叶子开始剥离得到后序序列，
/// 反转后即得到父在前的先序序列。若仍有骨骼未被剥离（说明存在环），
/// 则把这些骨骼视为根节点追加到末尾，打断环以避免世界矩阵计算时死循环。
///
/// @param parentIn  每个骨骼的父索引（原始下标），-1 表示无父。
/// @param order     输出：重排后的原始骨骼下标序列。
/// @param remap     输出：原始下标 -> 新下标。
void BuildBoneOrder(const std::vector<int>& parentIn, std::vector<int>& order,
                    std::vector<int>& remap) {
    const std::size_t n = parentIn.size();
    order.clear();
    remap.assign(n, -1);
    if (n == 0) return;

    std::vector<int> childrenCount(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        const int p = parentIn[i];
        // 越界或自环的父索引视为无效，避免统计出错。
        if (p < 0 || static_cast<std::size_t>(p) >= n || static_cast<std::size_t>(p) == i) continue;
        ++childrenCount[static_cast<std::size_t>(p)];
    }

    std::vector<std::size_t> stack;
    stack.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (childrenCount[i] == 0) stack.push_back(i);
    }
    std::vector<int> postOrder;
    postOrder.reserve(n);
    while (!stack.empty()) {
        const std::size_t cur = stack.back();
        stack.pop_back();
        postOrder.push_back(static_cast<int>(cur));
        const int p = parentIn[cur];
        if (p < 0 || static_cast<std::size_t>(p) >= n || static_cast<std::size_t>(p) == cur) continue;
        if (--childrenCount[static_cast<std::size_t>(p)] == 0) {
            stack.push_back(static_cast<std::size_t>(p));
        }
    }

    order.reserve(n);
    // 反转后序即得先序（父在前）。
    for (std::size_t i = postOrder.size(); i > 0; --i) {
        order.push_back(postOrder[i - 1]);
    }
    // 环中的骨骼：视为根节点追加，保证每个骨骼都出现且顺序确定。
    std::vector<bool> placed(n, false);
    for (int idx : order) placed[static_cast<std::size_t>(idx)] = true;
    for (std::size_t i = 0; i < n; ++i) {
        if (!placed[i]) {
            placed[i] = true;
            order.push_back(static_cast<int>(i));
        }
    }
    for (std::size_t i = 0; i < order.size(); ++i) {
        remap[static_cast<std::size_t>(order[i])] = static_cast<int>(i);
    }
}

/// 构造右手系下的 look-at 视图矩阵（相机看向 -Z，up 为 +Y）。
Mat4 LookAt(const Vec3& eye, const Vec3& target, const Vec3& up) {
    const Vec3 backward = Normalize(eye - target);
    Vec3 right = Cross(up, backward);
    if (Length(right) < kEpsilon) {
        // up 与视线共线时无法定义右向量，退化为固定轴以避免 NaN。
        right = Vec3(1.0f, 0.0f, 0.0f);
    } else {
        right = Normalize(right);
    }
    const Vec3 trueUp = Cross(backward, right);

    Mat4 view;
    view.At(0, 0) = right.x;    view.At(0, 1) = right.y;    view.At(0, 2) = right.z;
    view.At(1, 0) = trueUp.x;   view.At(1, 1) = trueUp.y;   view.At(1, 2) = trueUp.z;
    view.At(2, 0) = backward.x; view.At(2, 1) = backward.y; view.At(2, 2) = backward.z;
    view.At(0, 3) = -Dot(right, eye);
    view.At(1, 3) = -Dot(trueUp, eye);
    view.At(2, 3) = -Dot(backward, eye);
    view.At(3, 3) = 1.0f;
    return view;
}

/// 顶点索引越界标记（与 PmxParser 内部约定一致）。
constexpr std::uint32_t kInvalidVertexIndex = 0xFFFFFFFFu;

}  // namespace

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------

bool PmxModel::Load(const std::string& path, std::string& error) {
    loaded_ = false;
    gpuReady_ = false;
    textureLoaded_ = 0;
    drawCalls_ = 0;
    error.clear();

    // 把 VRM-VPet 风格的内置骨骼动作注册进随机待机池。
    // 早期只声明了 boneMotion_ 播放器却没有注册任何动作，导致 PMX / VRM
    // 待机时只有连续微动、永远不会有「点头 / 摇头 / 挥手」这类成套动作。
    boneMotion_.ClearRandomMotions();
    for (const model::BoneMotion& motion : model::bone_motions::Builtin()) {
        boneMotion_.AddRandomMotion(motion);
    }

    PmxDocument doc;
    if (!ParsePmx(path, doc, error)) {
        return false;
    }

    path_ = path;
    doc_ = std::move(doc);
    BuildMesh();
    PrepareTexturePaths(DirNameOf(path));
    FindControlBones();

    loaded_ = true;
    return true;
}

// ---------------------------------------------------------------------------
// BuildMesh：PMX -> SkinnedMesh
// ---------------------------------------------------------------------------

void PmxModel::BuildMesh() {
    mesh_ = SkinnedMesh();
    boneSource_.clear();
    morphSource_.clear();
    hasOutline_ = false;
    hasVertexColor_ = false;

    const std::size_t vertexCount = doc_.vertices.size();
    const std::size_t boneCount = doc_.bones.size();
    const std::size_t materialCount = doc_.materials.size();

    // --- 骨骼拓扑排序（父在前、子在后） -----------------------------------
    std::vector<int> parents(boneCount, -1);
    for (std::size_t i = 0; i < boneCount; ++i) {
        const int p = doc_.bones[i].parentIndex;
        parents[i] = (p >= 0 && static_cast<std::size_t>(p) < boneCount) ? p : -1;
    }
    std::vector<int> order;
    std::vector<int> remap;
    BuildBoneOrder(parents, order, remap);

    // --- 顶点 -------------------------------------------------------------
    mesh_.vertices.resize(vertexCount);
    for (std::size_t i = 0; i < vertexCount; ++i) {
        const PmxVertex& src = doc_.vertices[i];
        Vertex& dst = mesh_.vertices[i];

        // 左手系 -> 右手系：取反 z。
        dst.position = Vec3(src.position[0], src.position[1], -src.position[2]);
        dst.normal = Vec3(src.normal[0], src.normal[1], -src.normal[2]);
        dst.uv = Vec2(src.uv[0], src.uv[1]);

        // 顶点色默认白色；PMX 2.1 里「顶点色」存放在附加 UV1 中。
        dst.color = Vec4(1.0f, 1.0f, 1.0f, 1.0f);

        // 权重：SDEF 已由解析器降级为 BDEF2（球面混合无法用线性蒙皮表达）。
        dst.joints = {0, 0, 0, 0};
        dst.weights = {1.0f, 0.0f, 0.0f, 0.0f};
        std::array<int, 4> srcJoints = src.boneIndices;
        std::array<float, 4> srcWeights = src.weights;
        NormalizeWeights(srcWeights);
        for (int k = 0; k < 4; ++k) {
            const int bone = srcJoints[static_cast<std::size_t>(k)];
            if (bone < 0 || static_cast<std::size_t>(bone) >= boneCount) continue;
            if (srcWeights[static_cast<std::size_t>(k)] <= 0.0f) continue;
            dst.joints[static_cast<std::size_t>(k)] =
                static_cast<std::uint32_t>(remap[static_cast<std::size_t>(bone)]);
            dst.weights[static_cast<std::size_t>(k)] = srcWeights[static_cast<std::size_t>(k)];
        }
    }

    // --- 包围盒（材质描边宽度依赖模型高度，所以要先算） ---------------------
    ComputeBounds(mesh_);
    center_ = (mesh_.boundsMin + mesh_.boundsMax) * 0.5f;
    if (mesh_.modelHeight <= kEpsilon) mesh_.modelHeight = 1.0f;
    modelHeight_ = mesh_.modelHeight;

    // --- 材质 -------------------------------------------------------------
    mesh_.materials.resize(materialCount);
    for (std::size_t i = 0; i < materialCount; ++i) {
        const PmxMaterial& src = doc_.materials[i];
        Material& dst = mesh_.materials[i];

        dst.name = src.name.empty() ? src.nameEn : src.name;
        dst.diffuse = Vec4(src.diffuse[0], src.diffuse[1], src.diffuse[2], src.diffuse[3]);
        dst.specular = Vec3(src.specular[0], src.specular[1], src.specular[2]);
        dst.shininess = src.shininess;
        dst.ambient = Vec3(src.ambient[0], src.ambient[1], src.ambient[2]);

        // 描边：edgeSize 原始值存进 edgeScale，世界单位宽度存进 outlineWidth。
        dst.edgeScale = src.edgeSize;
        dst.outlineColor = Vec3(src.edgeColor[0], src.edgeColor[1], src.edgeColor[2]);
        const bool hasEdge = (src.flags & kMaterialHasEdge) != 0;
        dst.outlineWidth = hasEdge ? src.edgeSize * modelHeight_ / kEdgePixelsToWorld : 0.0f;
        if (hasEdge && src.edgeSize > 0.0f) hasOutline_ = true;

        // 双面渲染：PMX 的 bit0 表示「关闭背面剔除」。
        dst.doubleSided = (src.flags & kMaterialNoCull) != 0;
        dst.useToon = true;
        dst.unlit = false;

        // 贴图索引统一指向同一张贴图列表（PMX 贴图表），-1 表示无。
        dst.baseColorTex = src.textureIndex;
        dst.sphereTex = src.sphereTextureIndex;
        // toonFlag==0 是独立 toon 贴图（下标指向贴图表）；toonFlag==1 是 MMD 内置
        // 共享 toon 渐变，没有对应的贴图文件，这里置 -1，由渲染层使用内置渐变。
        dst.toonRampTex = (src.toonFlag == 0) ? src.toonIndex : -1;

        if ((src.flags & kMaterialVertexColor) != 0) hasVertexColor_ = true;
    }

    // 顶点色：PMX 把颜色放在附加 UV1（RGBA），材质 flag bit5 打开时生效。
    if (hasVertexColor_) {
        for (std::size_t i = 0; i < vertexCount; ++i) {
            const PmxVertex& src = doc_.vertices[i];
            if (src.additionalUvs.empty()) continue;
            const std::array<float, 4>& c = src.additionalUvs[0];
            mesh_.vertices[i].color = Vec4(c[0], c[1], c[2], c[3]);
        }
    }

    // --- 骨骼 -------------------------------------------------------------
    mesh_.bones.resize(order.size());
    boneSource_.resize(order.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        const std::size_t srcIdx = static_cast<std::size_t>(order[i]);
        const PmxBone& src = doc_.bones[srcIdx];
        Bone& dst = mesh_.bones[i];
        boneSource_[i] = static_cast<int>(srcIdx);

        dst.name = src.name.empty() ? src.nameEn : src.name;

        // 父骨骼索引重映射；若父骨骼在排序中排在后面（只可能是环），断开以免死循环。
        const int srcParent = parents[srcIdx];
        dst.parent = -1;
        if (srcParent >= 0) {
            const int mapped = remap[static_cast<std::size_t>(srcParent)];
            if (mapped >= 0 && static_cast<std::size_t>(mapped) < i) {
                dst.parent = mapped;
            }
        }

        // PMX 骨骼位置是模型空间绝对坐标；绑定姿态下没有旋转，只有平移。
        const Vec3 absPos(src.position[0], src.position[1], -src.position[2]);
        Vec3 parentAbs(0.0f, 0.0f, 0.0f);
        if (dst.parent >= 0) {
            // dst.parent 是重排后的新下标，需要换回原始下标才能取到父骨骼位置。
            const std::size_t parentSrcIdx = static_cast<std::size_t>(order[static_cast<std::size_t>(dst.parent)]);
            const PmxBone& pb = doc_.bones[parentSrcIdx];
            parentAbs = Vec3(pb.position[0], pb.position[1], -pb.position[2]);
        }
        dst.localBindPose = Mat4::Translation(absPos - parentAbs);
        dst.inverseBindMatrix = Mat4::Translation(-absPos);
        dst.currentLocal = dst.localBindPose;
        dst.world = dst.localBindPose;
        dst.parentWorld = Mat4::Identity();
        dst.restPosition = absPos;

        // MMD 的 flag bit1/bit2 表示该骨骼是否允许旋转 / 位移。
        dst.hasTranslation = (src.flags & kBoneTranslatable) != 0;
        dst.ikIteration = static_cast<int>(src.ikIteration);
        dst.ikLimitAngle = src.ikLimitAngle;
        if (src.hasIk && src.ikTargetIndex >= 0 &&
            static_cast<std::size_t>(src.ikTargetIndex) < boneCount) {
            dst.ikTarget = remap[static_cast<std::size_t>(src.ikTargetIndex)];
        }
    }

    // --- 索引与子网格 -----------------------------------------------------
    // PMX 材质的 faceCount 是「索引数」（恒为 3 的倍数），据此切分子网格。
    mesh_.indices.clear();
    mesh_.subMeshes.clear();
    std::size_t faceCursor = 0;
    for (std::size_t mi = 0; mi < materialCount; ++mi) {
        const PmxMaterial& src = doc_.materials[mi];
        SkinnedMesh::SubMesh sub;
        sub.materialIndex = static_cast<int>(mi);
        sub.indexOffset = static_cast<std::uint32_t>(mesh_.indices.size());

        const std::size_t triangleCount = static_cast<std::size_t>(src.faceCount) / 3;
        for (std::size_t t = 0; t < triangleCount; ++t) {
            if (faceCursor >= doc_.faces.size()) break;
            const std::array<std::uint32_t, 3>& tri = doc_.faces[faceCursor++];
            // 顶点索引越界的三角形在解析阶段已被标记，直接跳过。
            if (tri[0] == kInvalidVertexIndex || tri[1] == kInvalidVertexIndex ||
                tri[2] == kInvalidVertexIndex) {
                continue;
            }
            // 左手系 -> 右手系：反转绕序（0 <-> 2）。
            mesh_.indices.push_back(tri[0]);
            mesh_.indices.push_back(tri[2]);
            mesh_.indices.push_back(tri[1]);
        }
        sub.indexCount = static_cast<std::uint32_t>(mesh_.indices.size()) - sub.indexOffset;
        if (sub.indexCount > 0) {
            mesh_.subMeshes.push_back(sub);
        }
    }
    // 少数模型的面数总和会与材质声明的总和不一致，把剩余面并入最后一个子网格，
    // 避免几何体被静默丢弃。
    if (faceCursor < doc_.faces.size()) {
        for (std::size_t t = faceCursor; t < doc_.faces.size(); ++t) {
            const std::array<std::uint32_t, 3>& tri = doc_.faces[t];
            if (tri[0] == kInvalidVertexIndex || tri[1] == kInvalidVertexIndex ||
                tri[2] == kInvalidVertexIndex) {
                continue;
            }
            mesh_.indices.push_back(tri[0]);
            mesh_.indices.push_back(tri[2]);
            mesh_.indices.push_back(tri[1]);
        }
        if (!mesh_.subMeshes.empty()) {
            SkinnedMesh::SubMesh& last = mesh_.subMeshes.back();
            last.indexCount = static_cast<std::uint32_t>(mesh_.indices.size()) - last.indexOffset;
        }
    }
    // 模型没有任何材质但有面：补一个默认材质并建立单个子网格，避免模型整体不可见。
    if (mesh_.materials.empty() && !mesh_.indices.empty()) {
        mesh_.materials.push_back(Material{});
        SkinnedMesh::SubMesh sub;
        sub.materialIndex = 0;
        sub.indexOffset = 0;
        sub.indexCount = static_cast<std::uint32_t>(mesh_.indices.size());
        mesh_.subMeshes.clear();
        mesh_.subMeshes.push_back(sub);
    }

    // --- morph ------------------------------------------------------------
    // 只保留顶点形变（SkinnedMesh::MorphTarget 只描述位置/法线偏移）。
    morphSource_.assign(doc_.morphs.size(), -1);
    for (std::size_t i = 0; i < doc_.morphs.size(); ++i) {
        const PmxMorph& src = doc_.morphs[i];
        if (src.type != PmxMorphType::Vertex) continue;
        SkinnedMesh::MorphTarget target;
        target.name = src.name.empty() ? src.nameEn : src.name;
        target.positions.assign(vertexCount, Vec3(0.0f, 0.0f, 0.0f));
        target.normals.assign(vertexCount, Vec3(0.0f, 0.0f, 0.0f));
        for (const PmxMorphElement& e : src.elements) {
            if (e.index < 0 || static_cast<std::size_t>(e.index) >= vertexCount) continue;
            // 形变偏移同样是模型空间坐标，需要同样取反 z。
            target.positions[static_cast<std::size_t>(e.index)] =
                Vec3(e.offset[0], e.offset[1], -e.offset[2]);
        }
        morphSource_[i] = static_cast<int>(mesh_.morphs.size());
        mesh_.morphs.push_back(std::move(target));
    }

    // --- 表情（来自显示帧） -----------------------------------------------
    // 显示帧引用 morph 的部分转成 Expression，方便后续按名字驱动表情。
    for (const PmxDisplayFrame& frame : doc_.displayFrames) {
        SkinnedMesh::Expression expr;
        expr.name = frame.name.empty() ? frame.nameEn : frame.name;
        expr.isBlendShape = true;
        expr.category = MorphPanelCategory(0);
        for (const PmxDisplayFrame::Item& item : frame.items) {
            if (item.target != 1) continue;  // 只处理 morph 类型的条目
            if (item.index < 0 || static_cast<std::size_t>(item.index) >= morphSource_.size()) continue;
            const int mapped = morphSource_[static_cast<std::size_t>(item.index)];
            if (mapped < 0) continue;
            expr.bindings.emplace_back(mapped, 1.0f);
            const std::size_t morphIdx = static_cast<std::size_t>(item.index);
            if (morphIdx < doc_.morphs.size()) {
                expr.category = MorphPanelCategory(doc_.morphs[morphIdx].panel);
            }
        }
        if (expr.bindings.empty()) continue;
        mesh_.expressions.push_back(std::move(expr));
    }

    mesh_.isSkinned = !mesh_.bones.empty();
    BindExpressionDirector();
}

// ---------------------------------------------------------------------------
// 贴图路径
// ---------------------------------------------------------------------------

void PmxModel::PrepareTexturePaths(const std::string& baseDir) {
    texturePaths_.clear();
    texturePaths_.reserve(doc_.textures.size());
    // 建一次目录索引，供所有贴图做大小写/目录容错（PMX 包常见）。
    const TextureFileIndex index = BuildTextureFileIndex(baseDir);
    for (const std::string& raw : doc_.textures) {
        texturePaths_.push_back(ResolveTexturePath(baseDir, raw, index));
    }
}

ModelDebugInfo PmxModel::DebugInfo() const {
    ModelDebugInfo info;
    info.displayName = DisplayName();
    info.path = path_;
    info.type = ModelType::Pmx;
    info.loaded = loaded_;

    FillMeshDebugInfo(info, mesh_);

    char version[32];
    std::snprintf(version, sizeof(version), "PMX %.1f", static_cast<double>(doc_.version));
    info.formatVersion = version;
    info.textEncoding = doc_.utf16 ? "UTF-16LE" : "UTF-8";
    info.hasOutline = hasOutline_;
    info.textureReferenced = texturePaths_.size();
    info.textureLoaded = textureLoaded_;
    info.drawCalls = drawCalls_;

    // 图片明细：PMX 的贴图是磁盘文件，路径可直接用于 Qt 缩略图预览。
    info.textures.reserve(texturePaths_.size());
    for (std::size_t i = 0; i < texturePaths_.size(); ++i) {
        ModelDebugInfo::TextureInfo t;
        t.name = i < doc_.textures.size() ? doc_.textures[i]
                                          : ("贴图 #" + std::to_string(i));
        t.path = texturePaths_[i];
        t.loaded = !t.path.empty() && FileExists(t.path);
        t.note = t.loaded ? "磁盘贴图（可预览）"
                          : "文件不存在 —— 请把 .pmx 与贴图目录一起打包成 .zip";
        info.textures.push_back(t);
    }
    info.gpuReady = gpuReady_;
    info.missingResources = MissingResources();

    info.properties.push_back("坐标系=PMX 左手系 → OpenGL 右手系（z 取反 + 反转绕序）");
    info.properties.push_back("渲染管线=蒙皮网格 + toon 着色（与 VRM 共用）");
    info.properties.push_back(std::string("顶点色=") + (hasVertexColor_ ? "启用" : "未启用"));
    info.properties.push_back("控制骨骼=头 " + std::string(headBone_ >= 0 ? "命中" : "未找到") +
                              " / 左眼 " + (leftEyeBone_ >= 0 ? "命中" : "未找到") +
                              " / 右眼 " + (rightEyeBone_ >= 0 ? "命中" : "未找到") +
                              " / 躯干 " + (bodyBone_ >= 0 ? "命中" : "未找到"));
    info.properties.push_back("原始骨骼=" + std::to_string(doc_.bones.size()) +
                              "（已拓扑排序为父在前）");
    info.properties.push_back("三角形丢弃=" + std::to_string(doc_.droppedFaces) +
                              "（索引越界）");

    const bool meshOk = !mesh_.indices.empty() && !mesh_.materials.empty();
    info.renderingOk = loaded_ && gpuReady_ && meshOk && info.glError == 0;

    info.expressionStatus = expressionDirector_.StatusText();
    // 待机随机表情状态（诊断面板用）：开关 + 下次触发倒计时。
    info.expressionStatus += expressionDirector_.RandomIdle() ? " / 随机表情：开" : " / 随机表情：关";
    info.expressionSummary = "预设 " + std::to_string(expressionDirector_.Presets().size()) +
                            " / 通道 " + std::to_string(expressionDirector_.ChannelCount()) +
                            " / 当前 " + (expressionDirector_.ActivePreset().empty()
                                              ? "自定义" : expressionDirector_.ActivePreset());
    if (!loaded_) {
        info.renderStatus = "未加载模型";
    } else if (!gpuReady_) {
        info.renderStatus = "等待首次渲染建立 GPU 资源";
    } else if (!meshOk) {
        info.renderStatus = "网格为空，无法渲染";
    } else if (info.glError != 0) {
        char err[64];
        std::snprintf(err, sizeof(err), "GL 错误 0x%04x", info.glError);
        info.renderStatus = err;
    } else if (!info.missingResources.empty()) {
        info.renderStatus = "可出图，但缺 " + std::to_string(info.missingResources.size()) +
                            " 张贴图（可能白模）";
    } else {
        info.renderStatus = "正常渲染";
    }
    return info;
}

std::vector<std::string> PmxModel::MissingResources() const {
    std::vector<std::string> missing;
    for (const std::string& path : texturePaths_) {
        if (!path.empty() && !FileExists(path)) missing.push_back(path);
    }
    return missing;
}

// ---------------------------------------------------------------------------
// 控制骨骼识别
// ---------------------------------------------------------------------------

void PmxModel::FindControlBones() {
    headBone_ = -1;
    leftEyeBone_ = -1;
    rightEyeBone_ = -1;
    bodyBone_ = -1;
    centerBone_ = -1;
    spineBone_ = -1;
    chestBone_ = -1;
    leftArmBone_ = -1;
    rightArmBone_ = -1;
    leftElbowBone_ = -1;
    rightElbowBone_ = -1;
    leftLegBone_ = -1;
    rightLegBone_ = -1;

    // 头部优先用「頭 / head」，其次才是「首 / neck」：
    // 转动真正的头骨比转动脖子自然，但脖子同样能表达视线方向，作为回退。
    for (int pass = 0; pass < 2 && headBone_ < 0; ++pass) {
        for (std::size_t i = 0; i < mesh_.bones.size(); ++i) {
            const std::size_t srcIdx = static_cast<std::size_t>(boneSource_[i]);
            const PmxBone& src = doc_.bones[srcIdx];
            const bool strong = NameContains(src.name, "頭") || NameContains(src.nameEn, "head");
            const bool weak = NameContains(src.name, "首") || NameContains(src.nameEn, "neck");
            const bool match = (pass == 0) ? strong : weak;
            if (!match) continue;
            headBone_ = static_cast<int>(i);
            break;
        }
    }

    for (std::size_t i = 0; i < mesh_.bones.size(); ++i) {
        const std::size_t srcIdx = static_cast<std::size_t>(boneSource_[i]);
        const PmxBone& src = doc_.bones[srcIdx];
        const bool isEyeL = NameContains(src.name, "左目") || NameContains(src.nameEn, "eye_l") ||
                            NameContains(src.nameEn, "l_eye") || NameContains(src.nameEn, "left eye") ||
                            NameContains(src.nameEn, "left_eye") || NameContains(src.nameEn, "eyeball_l");
        const bool isEyeR = NameContains(src.name, "右目") || NameContains(src.nameEn, "eye_r") ||
                            NameContains(src.nameEn, "r_eye") || NameContains(src.nameEn, "right eye") ||
                            NameContains(src.nameEn, "right_eye") || NameContains(src.nameEn, "eyeball_r");
        const bool isBody = NameContains(src.name, "上半身") ||
                            NameContains(src.nameEn, "upper body") ||
                            NameContains(src.nameEn, "upper_body") || NameContains(src.nameEn, "torso");
        // --- 待机动画用到的骨骼 ---
        // 名字匹配按「更具体的优先」排序，因此先收集，最后再统一挑选。
        const bool isCenter = NameContains(src.name, "センター") || NameContains(src.name, "センター") ||
                             NameContains(src.nameEn, "center") || NameContains(src.nameEn, "root");
        const bool isChest = NameContains(src.name, "胸") || NameContains(src.nameEn, "chest");
        const bool isLeftArm = NameContains(src.name, "左腕") || NameContains(src.nameEn, "arm_l") ||
                               NameContains(src.nameEn, "left arm") ||
                               NameContains(src.nameEn, "left_arm") || NameContains(src.nameEn, "l_arm");
        const bool isRightArm = NameContains(src.name, "右腕") || NameContains(src.nameEn, "arm_r") ||
                                NameContains(src.nameEn, "right arm") ||
                                NameContains(src.nameEn, "right_arm") || NameContains(src.nameEn, "r_arm");
        const bool isLeftElbow = NameContains(src.name, "左ひじ") || NameContains(src.nameEn, "elbow_l") ||
                                 NameContains(src.nameEn, "left elbow") ||
                                 NameContains(src.nameEn, "left_elbow") || NameContains(src.nameEn, "l_elbow");
        const bool isRightElbow = NameContains(src.name, "右ひじ") || NameContains(src.nameEn, "elbow_r") ||
                                  NameContains(src.nameEn, "right elbow") ||
                                  NameContains(src.nameEn, "right_elbow") || NameContains(src.nameEn, "r_elbow");
        const bool isLeftLeg = NameContains(src.name, "左足") || NameContains(src.nameEn, "leg_l") ||
                               NameContains(src.nameEn, "left leg") || NameContains(src.nameEn, "left_leg");
        const bool isRightLeg = NameContains(src.name, "右足") || NameContains(src.nameEn, "leg_r") ||
                                NameContains(src.nameEn, "right leg") || NameContains(src.nameEn, "right_leg");

        if (leftEyeBone_ < 0 && isEyeL) leftEyeBone_ = static_cast<int>(i);
        if (rightEyeBone_ < 0 && isEyeR) rightEyeBone_ = static_cast<int>(i);
        if (bodyBone_ < 0 && isBody) bodyBone_ = static_cast<int>(i);
        if (centerBone_ < 0 && isCenter) centerBone_ = static_cast<int>(i);
        if (chestBone_ < 0 && isChest) chestBone_ = static_cast<int>(i);
        if (leftArmBone_ < 0 && isLeftArm) leftArmBone_ = static_cast<int>(i);
        if (rightArmBone_ < 0 && isRightArm) rightArmBone_ = static_cast<int>(i);
        if (leftElbowBone_ < 0 && isLeftElbow) leftElbowBone_ = static_cast<int>(i);
        if (rightElbowBone_ < 0 && isRightElbow) rightElbowBone_ = static_cast<int>(i);
        if (leftLegBone_ < 0 && isLeftLeg) leftLegBone_ = static_cast<int>(i);
        if (rightLegBone_ < 0 && isRightLeg) rightLegBone_ = static_cast<int>(i);
    }

    // 躯干骨骼缺失时退而使用根骨骼做摆动（根骨骼一定存在，除非模型没有骨骼）。
    if (bodyBone_ < 0 && !mesh_.bones.empty()) {
        for (std::size_t i = 0; i < mesh_.bones.size(); ++i) {
            if (mesh_.bones[i].parent < 0) {
                bodyBone_ = static_cast<int>(i);
                break;
            }
        }
    }
    // 躯干（spine）优先用「上半身」，没有就用 bodyBone_。
    if (spineBone_ < 0) spineBone_ = bodyBone_;
    // 中心骨骼缺失时用躯干骨骼代替（位移效果会略强，但不至于没有）。
    if (centerBone_ < 0) centerBone_ = spineBone_;

    mesh_.headBoneIndex = headBone_;
    mesh_.leftEyeBoneIndex = leftEyeBone_;
    mesh_.rightEyeBoneIndex = rightEyeBone_;
}

// ---------------------------------------------------------------------------
// Update
// ---------------------------------------------------------------------------

void PmxModel::Update(const RenderContext& ctx) {
    if (!loaded_) return;

    float dt = ctx.deltaTime;
    if (!(dt > 0.0f) || dt > 0.1f) dt = 1.0f / 60.0f;  // 防止切后台回来时动画跳变
    time_ += dt;

    UpdateGaze(ctx, dt);
    UpdateIdle(dt);
    ApplyBoneTransforms();
    UpdateSkinningMatrices();
    UpdateExpression(dt);
}
void PmxModel::UpdateGaze(const RenderContext& ctx, float dt) {
    if (!ctx.HasCursorTarget()) {
        targetGazeYaw_ = 0.0f;
        targetGazePitch_ = 0.0f;
    } else {
        // localCursor 已归一化到 [-1, 1]，y 向上为正（OpenGL 约定）。
        const float x = Clamp(ctx.localCursor.x, -1.0f, 1.0f);
        const float y = Clamp(ctx.localCursor.y, -1.0f, 1.0f);
        targetGazeYaw_ = x * kMaxHeadYaw;
        // RotationX(正角) 会让模型朝向从 +Z 偏向 -Y（即低头），
        // 所以「向上看」需要负角度，这里取负号。
        targetGazePitch_ = -y * kMaxHeadPitch;
    }

    // 指数平滑：避免鼠标抖动导致头部瞬移。
    const float alpha = 1.0f - std::exp(-kGazeSmoothing * dt);
    gazeYaw_ += (targetGazeYaw_ - gazeYaw_) * alpha;
    gazePitch_ += (targetGazePitch_ - gazePitch_) * alpha;
}

void PmxModel::UpdateIdle(float dt) {
    // 待机动画参数在这里计算，具体施加在 ApplyBoneTransforms 中，
    // 便于把所有骨骼变换集中在一处写入。
    // dt 只用于让呼吸节奏在低帧率下也保持稳定（用真实时间而不是帧数累加）。
    idleTime_ += dt;
    idleYaw_ = std::sin(idleTime_ * kIdleSwaySpeed) * kIdleSwayYawAmp;
    idlePitch_ = std::sin(idleTime_ * kIdleBreathSpeed) * kIdleSwayPitchAmp;

    // 程序化待机：呼吸 / 重心 / 头部 / 手臂 / 随机小动作。
    idlePose_ = idleAnimator_.Tick(dt);

    // 内置骨骼动作（VRM-VPet 风格：点头 / 摇头 / 挥手）。
    // 这里只**采样**结果，实际的骨骼变换在 ApplyBoneTransforms 里施加
    // （那里会先把骨骼复位到绑定姿态）。
    boneMotion_.Tick(dt, boneMotionPose_);
}

void PmxModel::ApplyBoneMotion(const model::BonePose& pose) {
    if (pose.entries.empty()) return;

    auto boneAt = [&](int index) -> Bone* {
        if (index < 0 || static_cast<std::size_t>(index) >= mesh_.bones.size()) return nullptr;
        return &mesh_.bones[static_cast<std::size_t>(index)];
    };

    for (const model::BonePose::Entry& entry : pose.entries) {
        // 动作里的骨骼名是「角色名」（head / leftArm / ...），映射到 PMX 骨骼。
        int index = -1;
        if (entry.bone == "head") {
            index = headBone_;
        } else if (entry.bone == "spine") {
            index = spineBone_;
        } else if (entry.bone == "chest") {
            index = chestBone_;
        } else if (entry.bone == "leftArm") {
            index = leftArmBone_;
        } else if (entry.bone == "rightArm") {
            index = rightArmBone_;
        } else if (entry.bone == "leftForearm") {
            index = leftElbowBone_;
        } else if (entry.bone == "rightForearm") {
            index = rightElbowBone_;
        }
        Bone* bone = boneAt(index);
        if (bone == nullptr) continue;

        // 骨骼动作的角度单位是度，这里转成弧度。
        constexpr float kDeg2Rad = 3.14159265358979323846f / 180.0f;
        const Mat4 rot = Mat4::RotationY(entry.yaw * kDeg2Rad) *
                         Mat4::RotationX(entry.pitch * kDeg2Rad) *
                         Mat4::RotationZ(entry.roll * kDeg2Rad);
        bone->currentLocal = bone->currentLocal * rot;
    }
}

void PmxModel::ApplyBoneTransforms() {
    // 先把所有骨骼复位到绑定姿态，避免上一帧的旋转累积。
    for (Bone& bone : mesh_.bones) {
        bone.currentLocal = bone.localBindPose;
    }

    const model::IdlePose& pose = idlePose_;

    // --- 整体位移与重心（センター / center）---
    if (centerBone_ >= 0 && static_cast<std::size_t>(centerBone_) < mesh_.bones.size()) {
        Bone& bone = mesh_.bones[static_cast<std::size_t>(centerBone_)];
        // 重心左右偏移 + 呼吸起伏。左右偏移用模型高度的千分之一量级，
        // 换算到 PMX 模型单位（通常模型高 10~20 单位）刚好是「几毫米到一厘米」。
        const float scale = modelHeight_ > 0.0f ? modelHeight_ * 0.012f : 0.02f;
        Vec3 offset(pose.weightShift * scale, pose.spineLift * scale, 0.0f);
        bone.currentLocal = bone.currentLocal * Mat4::Translation(offset);
    }

    // --- 躯干（重心的主要体现）---
    if (spineBone_ >= 0 && static_cast<std::size_t>(spineBone_) < mesh_.bones.size()) {
        Bone& bone = mesh_.bones[static_cast<std::size_t>(spineBone_)];
        const Mat4 rot = Mat4::RotationY(pose.spineYaw + idleYaw_) *
                         Mat4::RotationX(pose.spinePitch + idlePitch_) *
                         Mat4::RotationZ(pose.spineRoll);
        bone.currentLocal = bone.currentLocal * rot;
    }

    // --- 胸部（呼吸）---
    if (chestBone_ >= 0 && static_cast<std::size_t>(chestBone_) < mesh_.bones.size()) {
        Bone& bone = mesh_.bones[static_cast<std::size_t>(chestBone_)];
        const float s = 1.0f + pose.chestScale;
        bone.currentLocal = bone.currentLocal * Mat4::Scale(Vec3(s, s, s));
    }

    // --- 头部：视线追踪 + 待机微摆 ---
    if (headBone_ >= 0 && static_cast<std::size_t>(headBone_) < mesh_.bones.size()) {
        Bone& bone = mesh_.bones[static_cast<std::size_t>(headBone_)];
        const Mat4 rot = Mat4::RotationY(gazeYaw_ + pose.headYaw) *
                         Mat4::RotationX(gazePitch_ + pose.headPitch) *
                         Mat4::RotationZ(pose.headRoll);
        bone.currentLocal = bone.currentLocal * rot;
    }

    // --- 眼球：在头部转过的基础上再补一点额外转动 ---
    const float eyeYaw = (gazeYaw_ + pose.headYaw * 0.35f) * (kMaxEyeYaw / kMaxHeadYaw);
    const float eyePitch = (gazePitch_ + pose.headPitch * 0.35f) * (kMaxEyePitch / kMaxHeadPitch);
    if (leftEyeBone_ >= 0 && static_cast<std::size_t>(leftEyeBone_) < mesh_.bones.size()) {
        Bone& bone = mesh_.bones[static_cast<std::size_t>(leftEyeBone_)];
        bone.currentLocal = bone.currentLocal * Mat4::RotationY(eyeYaw) * Mat4::RotationX(eyePitch);
    }
    if (rightEyeBone_ >= 0 && static_cast<std::size_t>(rightEyeBone_) < mesh_.bones.size()) {
        Bone& bone = mesh_.bones[static_cast<std::size_t>(rightEyeBone_)];
        bone.currentLocal = bone.currentLocal * Mat4::RotationY(eyeYaw) * Mat4::RotationX(eyePitch);
    }

    // --- 手臂：自然下垂 + 待机小幅摆动 ---
    // PMX / VRM 的绑定姿态（A-pose / T-pose）里手臂斜举甚至水平平举，
    // 桌宠看起来「总抬着手」。localBindPose 只有平移（局部系 == 世界系），
    // 上臂沿 ±X 延伸，绕 Z 转 φ 时 +X -> (cosφ, sinφ)：
    //   右臂(+X) 放下需要 φ<0，左臂(-X) 放下需要 φ>0。
    // 骨骼动作播放期间按 BoneMotionPlayer::Weight() 逐步让位，挥手动作
    // （BoneMotion 的 lift 取相反符号）仍能完整抬起，淡出后平滑回到自然姿态。
    constexpr float kDeg2Rad = 3.14159265358979323846f / 180.0f;
    const float restDrop =
        armRestDropDeg_ * kDeg2Rad * (1.0f - std::clamp(boneMotion_.Weight(), 0.0f, 1.0f));
    if (leftArmBone_ >= 0 && static_cast<std::size_t>(leftArmBone_) < mesh_.bones.size()) {
        Bone& bone = mesh_.bones[static_cast<std::size_t>(leftArmBone_)];
        bone.currentLocal = bone.currentLocal * Mat4::RotationX(pose.leftArmPitch) *
                            Mat4::RotationZ(pose.leftArmRoll + restDrop);
    }
    if (rightArmBone_ >= 0 && static_cast<std::size_t>(rightArmBone_) < mesh_.bones.size()) {
        Bone& bone = mesh_.bones[static_cast<std::size_t>(rightArmBone_)];
        bone.currentLocal = bone.currentLocal * Mat4::RotationX(pose.rightArmPitch) *
                            Mat4::RotationZ(pose.rightArmRoll - restDrop);
    }
    if (leftElbowBone_ >= 0 && static_cast<std::size_t>(leftElbowBone_) < mesh_.bones.size()) {
        Bone& bone = mesh_.bones[static_cast<std::size_t>(leftElbowBone_)];
        bone.currentLocal = bone.currentLocal * Mat4::RotationX(pose.leftForearmPitch);
    }
    if (rightElbowBone_ >= 0 && static_cast<std::size_t>(rightElbowBone_) < mesh_.bones.size()) {
        Bone& bone = mesh_.bones[static_cast<std::size_t>(rightElbowBone_)];
        bone.currentLocal = bone.currentLocal * Mat4::RotationX(pose.rightForearmPitch);
    }

    // --- 腿：重心转移时支撑腿略微屈曲 ---
    if (leftLegBone_ >= 0 && static_cast<std::size_t>(leftLegBone_) < mesh_.bones.size()) {
        Bone& bone = mesh_.bones[static_cast<std::size_t>(leftLegBone_)];
        bone.currentLocal = bone.currentLocal * Mat4::RotationX(pose.leftLegLift);
    }
    if (rightLegBone_ >= 0 && static_cast<std::size_t>(rightLegBone_) < mesh_.bones.size()) {
        Bone& bone = mesh_.bones[static_cast<std::size_t>(rightLegBone_)];
        bone.currentLocal = bone.currentLocal * Mat4::RotationX(pose.rightLegLift);
    }

    // --- 内置骨骼动作（点头 / 摇头 / 挥手）---
    // 放在最后：动作是「显式的肢体动作」，优先级高于持续性的待机微摆。
    ApplyBoneMotion(boneMotionPose_);
}

void PmxModel::UpdateSkinningMatrices() {
    skinningMatrices_.clear();
    ComputeSkinningMatrices(mesh_, skinningMatrices_);
}

void PmxModel::BindExpressionDirector() {
    if (mesh_.morphs.empty()) return;
    std::vector<std::string> channels;
    channels.reserve(mesh_.morphs.size());
    for (const SkinnedMesh::MorphTarget& m : mesh_.morphs) channels.push_back(m.name);
    // PMX 的 morph 是连续顶点形变（无二元表情），不传 binaryFlags。
    expressionDirector_.Bind(channels);
    baseVertices_ = mesh_.vertices;
    morphWeights_.assign(mesh_.morphs.size(), 0.0f);
    uploadedMorphWeights_ = morphWeights_;
    // 口型叠加层与 morph 一一对应（初始全 0，即不张嘴）。
    mouthOverlay_.assign(mesh_.morphs.size(), 0.0f);
}

void PmxModel::UpdateExpression(float dt) {
    expressionDirector_.Tick(dt);
    if (mesh_.morphs.empty()) return;

    // ExpressionDirector 的通道就是 morph（PMX 把 morph 名作为通道），
    // 直接把它的输出映射成逐 morph 权重。
    const std::vector<float>& w = expressionDirector_.Weights();
    std::vector<float> weights(mesh_.morphs.size(), 0.0f);
    for (std::size_t i = 0; i < weights.size() && i < w.size(); ++i) weights[i] = w[i];

    // 口型叠加：元音 morph 权重加在表情权重之上（相加后钳制到 [0,1]）。
    // 「开口」类 morph 与情绪 morph 是同一批通道时，取较大值而不是相加，
    // 避免两次叠加后超过 1 造成形变过度。
    for (std::size_t i = 0; i < weights.size() && i < mouthOverlay_.size(); ++i) {
        const float mouth = mouthOverlay_[i];
        if (mouth <= 0.0f) continue;
        weights[i] = std::max(weights[i], mouth);
    }

    if (weights != morphWeights_) {
        morphWeights_ = weights;
        morphDirty_ = true;
    }
    if (!morphDirty_) return;

    // 顶点缓冲需要重新上传（渲染层只接受静态顶点缓冲）。
    mesh_.vertices = baseVertices_;
    ApplyMorphs(mesh_, morphWeights_, mesh_.vertices);
    if (gpuReady_) {
        renderer_.Upload(mesh_);
        uploadedMorphWeights_ = morphWeights_;
        morphDirty_ = false;
    }
}

bool PmxModel::SetExpressionPreset(const std::string& id) {
    const bool ok = expressionDirector_.ApplyPreset(id);
    // 用户手动切换表情后进入保护期，避免刚选好就被随机表情改掉。
    if (ok) expressionDirector_.NotifyManualChange();
    return ok;
}

void PmxModel::SetRandomIdleExpression(bool enabled) {
    expressionDirector_.SetRandomIdle(enabled);
}

bool PmxModel::RandomIdleExpression() const {
    return expressionDirector_.RandomIdle();
}

bool PmxModel::TriggerRandomExpression() {
    return expressionDirector_.TriggerRandomIdle();
}

bool PmxModel::SetExpressionWeights(const std::vector<std::pair<std::string, float>>& weights) {
    if (mesh_.morphs.empty()) return false;
    // 口型走「叠加层」而不是 PreviewWeights：后者会清空基础权重，
    // 导致每帧写口型都把用户当前选的表情（开心/生气…）抹掉。
    // 这里只更新口型通道（aa/ih/ou/ee/oh 等元音 morph），其余通道不动。
    std::fill(mouthOverlay_.begin(), mouthOverlay_.end(), 0.0f);
    for (const auto& kv : weights) {
        const int index = expressionDirector_.Resolve(kv.first);
        if (index < 0 || static_cast<std::size_t>(index) >= mouthOverlay_.size()) continue;
        mouthOverlay_[static_cast<std::size_t>(index)] =
            std::clamp(kv.second, 0.0f, 1.0f);
    }
    return true;
}

std::vector<std::string> PmxModel::ExpressionPresetIds() const {
    std::vector<std::string> ids;
    for (const ExpressionPreset& p : expressionDirector_.Presets()) ids.push_back(p.id);
    return ids;
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void PmxModel::Render(const RenderContext& ctx) {
    drawCalls_ = 0;
    if (!loaded_) return;
    // 材质为空或索引为空时直接返回，避免渲染层拿到空网格。
    if (mesh_.materials.empty() || mesh_.indices.empty()) return;

    EnsureGpuResources();
    if (!gpuReady_) return;

    const int width = ctx.viewportWidth > 0 ? ctx.viewportWidth : viewportWidth_;
    const int height = ctx.viewportHeight > 0 ? ctx.viewportHeight : viewportHeight_;
    if (width <= 0 || height <= 0) return;

    if (skinningMatrices_.size() != mesh_.bones.size()) {
        UpdateSkinningMatrices();
    }

    const Mat4 viewProj = ComputeViewProjection(width, height, ctx.renderScale);
    renderer_.Draw(viewProj, skinningMatrices_, kLightDir);
    drawCalls_ += mesh_.subMeshes.size();
    if (hasOutline_) {
        // widthScale=1：描边宽度已经在材质里换算成世界单位。
        renderer_.DrawOutline(viewProj, skinningMatrices_, kLightDir, 1.0f);
        drawCalls_ += mesh_.subMeshes.size();
    }
}

std::vector<std::string> PmxModel::MotionNames() const {
    std::vector<std::string> out;
    for (const model::BoneMotion& motion : model::bone_motions::Builtin()) {
        out.push_back(motion.id);
    }
    return out;
}

bool PmxModel::PlayMotion(const std::string& id) {
    model::BoneMotion motion;
    if (!model::bone_motions::Find(id, motion)) {
        // 宽松匹配：允许只敲名字前缀。
        for (const model::BoneMotion& candidate : model::bone_motions::Builtin()) {
            if (candidate.id.find(id) != std::string::npos) {
                motion = candidate;
                return boneMotion_.Play(motion);
            }
        }
        return false;
    }
    return boneMotion_.Play(motion);
}

bool PmxModel::StopMotion() {
    boneMotion_.Stop();
    return true;
}

void PmxModel::ReloadRenderer() {
    // Renderer::Upload / SetTextures 会在下一帧 EnsureGpuResources() 里重新执行，
    // 旧的 Texture 对象由 vector 析构按 RAII 释放。
    gpuReady_ = false;
}

float PmxModel::PreferredFrameAspect() const {
    const float w = mesh_.boundsMax.x - mesh_.boundsMin.x;
    const float h = mesh_.boundsMax.y - mesh_.boundsMin.y;
    if (w <= 1e-4f || h <= 1e-4f) return 0.0f;
    const float aspect = w / h;
    return (aspect > 0.05f && aspect < 20.0f) ? aspect : 0.0f;
}

void PmxModel::SetArmRestDrop(float degrees) {
    if (degrees < 0.0f) {
        armRestDropDeg_ = 0.0f;  // 显式关闭
    } else if (degrees > 0.0f) {
        armRestDropDeg_ = std::min(degrees, 80.0f);
    }
    // degrees == 0：保留 PMX 后端默认值（18°）。
}

void PmxModel::EnsureGpuResources() {
    if (gpuReady_) return;
    gpuReady_ = true;
    textureLoaded_ = 0;

    renderer_.Upload(mesh_);

    textures_.clear();
    textures_.reserve(texturePaths_.size());
    for (const std::string& path : texturePaths_) {
        Texture tex;
        if (path.empty()) {
            textures_.push_back(std::move(tex));
            continue;
        }
        // 贴图缺失时保留无效纹理，渲染层会回退到纯色；
        // 成功数用于调试面板判断「是不是白模」。
        if (tex.LoadFromFile(path)) ++textureLoaded_;
        textures_.push_back(std::move(tex));
    }
    renderer_.SetTextures(std::move(textures_));
}

Mat4 PmxModel::ComputeViewProjection(int viewportWidth, int viewportHeight,
                                      float renderScale) const {
    const float aspect = static_cast<float>(viewportWidth) / static_cast<float>(viewportHeight);

    // 包围盒决定取景范围：模型高度决定纵向范围，宽度按宽高比换算。
    const float modelWidth = std::max(mesh_.boundsMax.x - mesh_.boundsMin.x, kEpsilon);
    const float height = std::max(modelHeight_, kEpsilon);
    float halfHeight = std::max(height * 0.5f, (modelWidth * 0.5f) / std::max(aspect, kEpsilon));
    halfHeight *= kFramingMargin;
    // 渲染缩放：视体越小，模型在窗口里显示得越大。
    halfHeight /= std::max(renderScale, 0.01f);

    // 相机放在模型正前方（+Z），看向模型中心。
    // 注意：PMX 模型朝向 -Z，取反 z 之后模型朝向 +Z，因此相机放在 +Z 侧。
    const float distance = std::max(height, modelWidth) * 4.0f;
    const Vec3 eye(center_.x, center_.y, center_.z + distance);
    const Mat4 view = LookAt(eye, center_, Vec3(0.0f, 1.0f, 0.0f));

    const float nearZ = std::max(distance - halfHeight * 2.0f - height, 0.01f);
    const float farZ = distance + halfHeight * 2.0f + height;
    const Mat4 proj = Ortho(-halfHeight * aspect, halfHeight * aspect, -halfHeight,
                            halfHeight, nearZ, farZ);
    return proj * view;
}

// ---------------------------------------------------------------------------
// 其它接口
// ---------------------------------------------------------------------------

void PmxModel::OnResize(int width, int height) {
    viewportWidth_ = width;
    viewportHeight_ = height;
}

std::string PmxModel::DisplayName() const {
    if (!doc_.name.empty()) return doc_.name;
    if (!doc_.nameEn.empty()) return doc_.nameEn;
    if (!path_.empty()) return BaseName(path_);
    return "PMX Model";
}

}  // namespace ppet::pmx
