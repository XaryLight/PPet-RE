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
#include "model/pmx/PmxModel.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

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
std::string NormalizeTexturePath(const std::string& raw) {
    std::string out = raw;
    for (char& c : out) {
        if (c == '\\') c = '/';
    }
    return out;
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
    error.clear();

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
}

// ---------------------------------------------------------------------------
// 贴图路径
// ---------------------------------------------------------------------------

void PmxModel::PrepareTexturePaths(const std::string& baseDir) {
    texturePaths_.clear();
    texturePaths_.reserve(doc_.textures.size());
    for (const std::string& raw : doc_.textures) {
        const std::string rel = NormalizeTexturePath(raw);
        if (rel.empty()) {
            texturePaths_.push_back(rel);
            continue;
        }
        const bool absolute = rel[0] == '/';
        texturePaths_.push_back(absolute || baseDir.empty() ? rel : baseDir + "/" + rel);
    }
}

// ---------------------------------------------------------------------------
// 控制骨骼识别
// ---------------------------------------------------------------------------

void PmxModel::FindControlBones() {
    headBone_ = -1;
    leftEyeBone_ = -1;
    rightEyeBone_ = -1;
    bodyBone_ = -1;

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

        if (leftEyeBone_ < 0 && isEyeL) leftEyeBone_ = static_cast<int>(i);
        if (rightEyeBone_ < 0 && isEyeR) rightEyeBone_ = static_cast<int>(i);
        if (bodyBone_ < 0 && isBody) bodyBone_ = static_cast<int>(i);
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
}

void PmxModel::UpdateGaze(const RenderContext& ctx, float dt) {
    if (!ctx.hasGlobalCursor) {
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
}

void PmxModel::ApplyBoneTransforms() {
    // 先把所有骨骼复位到绑定姿态，避免上一帧的旋转累积。
    for (Bone& bone : mesh_.bones) {
        bone.currentLocal = bone.localBindPose;
    }

    // 躯干摆动：轻微左右转 + 呼吸般的上下点头。
    if (bodyBone_ >= 0 && static_cast<std::size_t>(bodyBone_) < mesh_.bones.size()) {
        Bone& bone = mesh_.bones[static_cast<std::size_t>(bodyBone_)];
        const Mat4 rot = Mat4::RotationY(idleYaw_) * Mat4::RotationX(idlePitch_);
        bone.currentLocal = bone.localBindPose * rot;
    }

    // 视线追踪：头部先转，眼球再补一点额外的转动。
    // 找不到对应骨骼时直接跳过（优雅降级）。
    if (headBone_ >= 0 && static_cast<std::size_t>(headBone_) < mesh_.bones.size()) {
        Bone& bone = mesh_.bones[static_cast<std::size_t>(headBone_)];
        const Mat4 rot = Mat4::RotationY(gazeYaw_) * Mat4::RotationX(gazePitch_);
        bone.currentLocal = bone.currentLocal * rot;
    }
    const float eyeYaw = gazeYaw_ * (kMaxEyeYaw / kMaxHeadYaw);
    const float eyePitch = gazePitch_ * (kMaxEyePitch / kMaxHeadPitch);
    if (leftEyeBone_ >= 0 && static_cast<std::size_t>(leftEyeBone_) < mesh_.bones.size()) {
        Bone& bone = mesh_.bones[static_cast<std::size_t>(leftEyeBone_)];
        bone.currentLocal = bone.currentLocal * Mat4::RotationY(eyeYaw) * Mat4::RotationX(eyePitch);
    }
    if (rightEyeBone_ >= 0 && static_cast<std::size_t>(rightEyeBone_) < mesh_.bones.size()) {
        Bone& bone = mesh_.bones[static_cast<std::size_t>(rightEyeBone_)];
        bone.currentLocal = bone.currentLocal * Mat4::RotationY(eyeYaw) * Mat4::RotationX(eyePitch);
    }
}

void PmxModel::UpdateSkinningMatrices() {
    skinningMatrices_.clear();
    ComputeSkinningMatrices(mesh_, skinningMatrices_);
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void PmxModel::Render(const RenderContext& ctx) {
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
    if (hasOutline_) {
        // widthScale=1：描边宽度已经在材质里换算成世界单位。
        renderer_.DrawOutline(viewProj, skinningMatrices_, kLightDir, 1.0f);
    }
}

void PmxModel::EnsureGpuResources() {
    if (gpuReady_) return;
    gpuReady_ = true;

    renderer_.Upload(mesh_);

    textures_.clear();
    textures_.reserve(texturePaths_.size());
    for (const std::string& path : texturePaths_) {
        Texture tex;
        if (path.empty()) {
            textures_.push_back(std::move(tex));
            continue;
        }
        // 贴图缺失时保留无效纹理，渲染层会回退到纯色。
        tex.LoadFromFile(path);
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
