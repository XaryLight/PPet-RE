// PPet - C++ 移植版
// VRM 模型后端实现：VRM -> SkinnedMesh 转换、视线追踪、眨眼、弹簧骨、toon 渲染。
//
// 设计要点：
//   1. glTF / VRM 1.0 的模型正面朝 +Z；VRM 0.x 正面朝 -Z，
//      载入 0.x 时整体绕 Y 轴旋转 180°，渲染层因此只需要考虑一种朝向。
//   2. 骨骼数组按「父在前、子在后」排列，这是 ComputeSkinningMatrices 的前提。
//      glTF 的节点树不保证父节点在前，因此这里按节点深度排序。
//   3. 骨骼的 world 是「模型空间」的全局变换，包含场景根节点自身的变换；
//      顶点也转换到同一空间，两者必须一致，否则模型会整体偏移或缩放。
//   4. 视线追踪、眨眼、弹簧骨都必须在找不到对应骨骼时优雅降级，绝不能崩。
#include "model/vrm/VrmModel.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <sstream>

#include "model/vrm/GltfParser.hpp"
#include "model/vrm/VrmImage.hpp"

namespace ppet::vrm {
namespace {

// ---------------------------------------------------------------------------
// 常量
// ---------------------------------------------------------------------------

/// 头部绕 Y 轴的最大转角（弧度）。VRM lookAt 的默认 rangeMap 是 15 度。
constexpr float kDefaultLookYaw = 0.2618f;    // 15 度
/// 头部绕 X 轴的最大转角（弧度）。VRM lookAt 的默认 rangeMap 是 15 度，
/// 这里取 10 度以避免低头时穿模。
constexpr float kDefaultLookPitch = 0.1745f;  // 10 度

/// 视线角度的平滑系数（每秒衰减到 1/e 的比例）。
constexpr float kGazeSmoothing = 9.0f;

/// 待机摆动的幅度（弧度）与频率。
constexpr float kIdleSwayYawAmp = 0.0262f;    // 约 1.5 度
constexpr float kIdleSwayPitchAmp = 0.0131f;  // 约 0.75 度
constexpr float kIdleSwaySpeed = 0.7f;
constexpr float kIdleBreathSpeed = 1.6f;

/// 自动眨眼的间隔范围（秒）与时长（秒）。
constexpr float kBlinkMinInterval = 2.0f;
constexpr float kBlinkMaxInterval = 5.0f;

/// 取景时模型四周留出的空白比例。
constexpr float kFramingMargin = 1.12f;

/// 灯光方向（世界空间，指向光源）：来自模型前上方偏左。
const Vec3 kLightDir = Normalize(Vec3(-0.45f, 0.75f, 0.55f));

/// 节点遍历的最大深度，防止恶意文件构造超深嵌套导致栈溢出。
constexpr int kMaxNodeDepth = 256;
/// 单条弹簧骨链的最大关节数。
constexpr std::size_t kMaxSpringChain = 64;
/// 单个弹簧骨链的最大深度。
constexpr int kMaxSpringDepth = 32;

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

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

/// 绕任意轴的旋转矩阵（罗德里格公式）。
Mat4 RotationAxis(const Vec3& axis, float angle) {
    const Vec3 n = Normalize(axis);
    if (Length(n) < kEpsilon) return Mat4::Identity();
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    const float t = 1.0f - c;
    Mat4 r;
    r.At(0, 0) = t * n.x * n.x + c;
    r.At(0, 1) = t * n.x * n.y - s * n.z;
    r.At(0, 2) = t * n.x * n.z + s * n.y;
    r.At(1, 0) = t * n.x * n.y + s * n.z;
    r.At(1, 1) = t * n.y * n.y + c;
    r.At(1, 2) = t * n.y * n.z - s * n.x;
    r.At(2, 0) = t * n.x * n.z - s * n.y;
    r.At(2, 1) = t * n.y * n.z + s * n.x;
    r.At(2, 2) = t * n.z * n.z + c;
    return r;
}

/// 构造「把方向 a 转到方向 b」的最小旋转矩阵。
Mat4 RotationBetween(const Vec3& a, const Vec3& b) {
    const Vec3 from = Normalize(a);
    const Vec3 to = Normalize(b);
    if (Length(from) < kEpsilon || Length(to) < kEpsilon) return Mat4::Identity();
    const float dot = Clamp(Dot(from, to), -1.0f, 1.0f);
    if (dot > 1.0f - 1e-6f) return Mat4::Identity();
    Vec3 axis = Cross(from, to);
    const float len = Length(axis);
    if (len < 1e-6f) {
        // 方向相反：绕任意垂直轴旋转 180 度。
        const Vec3 helper = (std::fabs(from.x) < 0.9f) ? Vec3(1.0f, 0.0f, 0.0f)
                                                        : Vec3(0.0f, 1.0f, 0.0f);
        axis = Normalize(Cross(from, helper));
        return RotationAxis(axis, kPi);
    }
    return RotationAxis(axis, std::atan2(len, dot));
}

/// 四元数线性插值 + 归一化（nlerp，足够用于表情 / 骨骼动画）。
Vec4 NlerpQuat(const Vec4& a, const Vec4& b, float t) {
    float sign = 1.0f;
    if (Dot(a, b) < 0.0f) sign = -1.0f;
    const Vec4 r(a.x + (b.x * sign - a.x) * t, a.y + (b.y * sign - a.y) * t,
                 a.z + (b.z * sign - a.z) * t, a.w + (b.w * sign - a.w) * t);
    const float len = std::sqrt(Dot(r, r));
    if (len < kEpsilon) return Vec4(0.0f, 0.0f, 0.0f, 1.0f);
    return Vec4(r.x / len, r.y / len, r.z / len, r.w / len);
}

/// 构造右手系下的 look-at 视图矩阵（相机看向 -Z，up 为 +Y）。
Mat4 LookAt(const Vec3& eye, const Vec3& target, const Vec3& up) {
    const Vec3 backward = Normalize(eye - target);
    Vec3 right = Cross(up, backward);
    if (Length(right) < kEpsilon) {
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

/// 从 JSON 对象里取整数（类型不符或缺失时返回默认值）。
int JsonInt(const nlohmann::json& obj, const char* key, int def) {
    if (!obj.is_object()) return def;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_number()) return def;
    const double d = it->get<double>();
    if (!std::isfinite(d)) return def;
    if (d > 2147483647.0) return 2147483647;
    if (d < -2147483648.0) return -2147483648;
    return static_cast<int>(d);
}

/// 取 JSON 数组元素里的整数（不是对象字段）。
///
/// 弹簧骨扩展里有大量「纯数值数组」的下标字段
/// （joints[].node、colliderGroups[].colliders 等），
/// 需要一个直接从 JSON 值读整数的入口。
int JsonIntValue(const nlohmann::json& value, int def) {
    if (!value.is_number()) return def;
    const double d = value.get<double>();
    if (!std::isfinite(d)) return def;
    if (d > 2147483647.0) return 2147483647;
    if (d < -2147483648.0) return -2147483648;
    return static_cast<int>(d);
}

/// 从 JSON 对象里取浮点数。
float JsonFloat(const nlohmann::json& obj, const char* key, float def) {
    if (!obj.is_object()) return def;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_number()) return def;
    const double d = it->get<double>();
    if (!std::isfinite(d)) return def;
    return static_cast<float>(d);
}

/// 从 JSON 对象里取布尔值。
bool JsonBool(const nlohmann::json& obj, const char* key, bool def) {
    if (!obj.is_object()) return def;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_boolean()) return def;
    return it->get<bool>();
}

/// 从 JSON 对象里取字符串。
std::string JsonString(const nlohmann::json& obj, const char* key) {
    if (!obj.is_object()) return std::string();
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return std::string();
    return it->get<std::string>();
}

/// 从 JSON 对象里取数组形式的 vec3。
Vec3 JsonVec3(const nlohmann::json& obj, const char* key, const Vec3& def) {
    if (!obj.is_object()) return def;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_array() || it->size() < 3) return def;
    Vec3 out = def;
    for (int i = 0; i < 3; ++i) {
        if (!(*it)[static_cast<std::size_t>(i)].is_number()) return def;
        const double d = (*it)[static_cast<std::size_t>(i)].get<double>();
        if (!std::isfinite(d)) return def;
        (&out.x)[i] = static_cast<float>(d);
    }
    return out;
}

/// 取 JSON 对象里的某个子对象（不存在或类型不符时返回 nullptr）。
const nlohmann::json* JsonChild(const nlohmann::json& obj, const char* key) {
    if (!obj.is_object()) return nullptr;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_object()) return nullptr;
    return &(*it);
}

/// 取 JSON 对象里的某个子数组（不存在或类型不符时返回 nullptr）。
///
/// VRM 的很多字段是数组（springBone 的 springs / colliders / colliderGroups、
/// VRM 0.x 的 blendShapes），必须与对象字段区分开取。
const nlohmann::json* JsonArray(const nlohmann::json& obj, const char* key) {
    if (!obj.is_object()) return nullptr;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_array()) return nullptr;
    return &(*it);
}

/// 取 JSON 对象里的某个子值（不限制类型；不存在时返回 nullptr）。
///
/// 用于「同一字段可能有两种写法」的场景，例如 humanBones 既可以是对象
/// 也可以是数组。
const nlohmann::json* JsonValue(const nlohmann::json& obj, const char* key) {
    if (!obj.is_object()) return nullptr;
    const auto it = obj.find(key);
    if (it == obj.end()) return nullptr;
    return &(*it);
}

/// VRM 表情名 -> 表情分类。
///
/// VRM 1.0 的 preset 表情名是固定的；custom 表情需要按名字推断。
/// 分类的取值与 SkinnedMesh::Expression::category 的约定一致：
/// happy / angry / sad / relaxed / surprised / blink / look / aa / ih / ou / ee / oh。
std::string ExpressionCategory(const std::string& name, bool preset) {
    static const char* kPresetCategories[][2] = {
        {"happy", "happy"},       {"angry", "angry"},   {"sad", "sad"},
        {"relaxed", "relaxed"},   {"surprised", "surprised"},
        {"blink", "blink"},       {"blinkLeft", "blinkLeft"},
        {"blinkRight", "blinkRight"},
        {"aa", "aa"},             {"ih", "ih"},         {"ou", "ou"},
        {"ee", "ee"},             {"oh", "oh"},
        {"lookUp", "look"},       {"lookDown", "look"},
        {"lookLeft", "look"},     {"lookRight", "look"},
    };
    if (preset) {
        for (const auto& entry : kPresetCategories) {
            if (name == entry[0]) return entry[1];
        }
        return std::string();
    }
    // custom：按名字包含关系推断（大小写不敏感）。
    std::string lower = name;
    for (char& c : lower) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    auto contains = [&lower](const char* needle) {
        return lower.find(needle) != std::string::npos;
    };
    if (contains("blink")) return "blink";
    if (contains("look") || contains("eye") || contains("gaze")) return "look";
    if (contains("happy") || contains("joy")) return "happy";
    if (contains("angry")) return "angry";
    if (contains("sad") || contains("sorrow")) return "sad";
    if (contains("relax")) return "relaxed";
    if (contains("surprise")) return "surprised";
    if (contains("aa")) return "aa";
    if (contains("ih")) return "ih";
    if (contains("ou")) return "ou";
    if (contains("ee")) return "ee";
    if (contains("oh")) return "oh";
    return std::string();
}

/// VRM 0.x 的 presetName -> 表情分类。
///
/// VRM 0.x 用的是一套更早的 preset 命名（joy / sorrow / fun / a / i / u / e / o），
/// 与 VRM 1.0 的 preset 名并不相同，这里统一映射到 VRM 1.0 的分类名，
/// 上层（眨眼驱动、UI 分组）因此可以用同一套规则处理两种版本。
/// presetName 的写法不统一（大小写都有），所以比较前先转小写。
std::string ExpressionCategoryVrm0(const std::string& presetName) {
    static const char* kCategories[][2] = {
        {"joy", "happy"},   {"angry", "angry"},  {"sorrow", "sad"},
        {"fun", "relaxed"}, {"surprised", "surprised"},
        {"blink", "blink"}, {"blink_l", "blinkLeft"}, {"blink_r", "blinkRight"},
        {"lookup", "look"}, {"lookdown", "look"}, {"lookleft", "look"},
        {"lookright", "look"},
        {"a", "aa"}, {"i", "ih"}, {"u", "ou"}, {"e", "ee"}, {"o", "oh"},
    };
    std::string lower = presetName;
    for (char& c : lower) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    for (const auto& entry : kCategories) {
        if (lower == entry[0]) return entry[1];
    }
    return std::string();
}

}  // namespace

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------

VrmModel::VrmModel() = default;

VrmModel::~VrmModel() = default;

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------

bool VrmModel::Load(const std::string& path, std::string& error) {
    loaded_ = false;
    gpuReady_ = false;
    error.clear();

    doc_ = std::unique_ptr<GltfDocument>(new GltfDocument());
    if (!LoadGltf(path, *doc_, error)) {
        doc_.reset();
        return false;
    }

    path_ = path;
    // 转换阶段会大量访问 JSON，nlohmann 在遇到类型不符或数值溢出时会抛异常。
    // 统一兜底成「加载失败」，保证畸形文件不会让程序崩溃。
    try {
        if (!BuildMesh(error)) {
            doc_.reset();
            return false;
        }
    } catch (const nlohmann::json::exception& e) {
        error = std::string("VRM 扩展数据异常：") + e.what();
        doc_.reset();
        return false;
    } catch (const std::exception& e) {
        error = std::string("VRM 加载异常：") + e.what();
        doc_.reset();
        return false;
    }

    loaded_ = true;
    return true;
}

// ---------------------------------------------------------------------------
// BuildMesh
// ---------------------------------------------------------------------------

bool VrmModel::BuildMesh(std::string& error) {
    mesh_ = SkinnedMesh();
    baseVertices_.clear();
    morphWeights_.clear();
    expressionWeights_.clear();
    expressionBinary_.clear();
    uploadedMorphWeights_.clear();
    springStates_.clear();
    animChannels_.clear();
    hasAnimation_ = false;
    animDuration_ = 0.0f;
    hasOutline_ = false;
    headBone_ = -1;
    leftEyeBone_ = -1;
    rightEyeBone_ = -1;
    bodyBone_ = -1;
    vrmVersion_ = -1;
    metaName_.clear();

    const nlohmann::json& root = doc_->Json();

    // --- VRM 版本与模型朝向 -------------------------------------------------
    const nlohmann::json* extensions = JsonChild(root, "extensions");
    const bool hasVrm10 = extensions != nullptr && extensions->contains("VRMC_vrm");
    const bool hasVrm0x = extensions != nullptr && extensions->contains("VRM");
    vrmVersion_ = hasVrm10 ? 1 : (hasVrm0x ? 0 : -1);
    // VRM 0.x 的模型正面朝 -Z（Unity 约定），VRM 1.0 正面朝 +Z（glTF 约定）。
    // 这里统一到 +Z，渲染层的相机固定放在 +Z 侧即可。
    modelTransform_ = (vrmVersion_ == 0) ? Mat4::RotationY(kPi) : Mat4::Identity();

    // --- 节点图 -------------------------------------------------------------
    BuildSceneGraph();

    // --- 弹簧骨链条（骨骼集合的收集要用到） ---------------------------------
    CollectSpringChains();

    // --- 骨骼 ---------------------------------------------------------------
    CollectBoneNodes();

    // --- 网格 ---------------------------------------------------------------
    if (!BuildMeshes(error)) return false;

    // --- 包围盒（材质描边宽度依赖模型高度，所以要先算） ---------------------
    ComputeBounds(mesh_);
    center_ = (mesh_.boundsMin + mesh_.boundsMax) * 0.5f;
    if (mesh_.modelHeight <= kEpsilon) mesh_.modelHeight = 1.0f;
    modelHeight_ = mesh_.modelHeight;

    // --- 材质与贴图 ---------------------------------------------------------
    BuildMaterials();

    // --- morph 目标 ---------------------------------------------------------
    BuildMorphs();

    // --- humanoid / 表情 / 弹簧骨 ------------------------------------------
    BuildHumanoid();
    BuildExpressions();
    BuildSpringBones();

    // --- 动画 ---------------------------------------------------------------
    BuildAnimations();

    mesh_.isSkinned = !mesh_.bones.empty();
    baseVertices_ = mesh_.vertices;
    morphWeights_.assign(mesh_.morphs.size(), 0.0f);
    animMorphWeights_.assign(mesh_.morphs.size(), 0.0f);
    expressionWeights_.assign(mesh_.expressions.size(), 0.0f);
    expressionBinary_.assign(mesh_.expressions.size(), 0);
    uploadedMorphWeights_ = morphWeights_;
    return true;
}

void VrmModel::BuildSceneGraph() {
    const std::vector<GltfNode>& nodes = doc_->Nodes();
    const std::size_t nodeCount = nodes.size();
    nodeParent_.assign(nodeCount, -1);
    nodeGlobal_.assign(nodeCount, Mat4::Identity());
    nodeDepth_.assign(nodeCount, 0);

    // 建立父子关系：一个节点只能有一个父节点（恶意文件可能把同一节点列在多个父节点下，
    // 这里只认第一次出现的）。
    std::vector<char> hasParent(nodeCount, 0);
    for (std::size_t i = 0; i < nodeCount; ++i) {
        for (const int child : nodes[i].children) {
            if (child < 0 || static_cast<std::size_t>(child) >= nodeCount) continue;
            if (hasParent[static_cast<std::size_t>(child)]) continue;
            hasParent[static_cast<std::size_t>(child)] = 1;
            nodeParent_[static_cast<std::size_t>(child)] = static_cast<int>(i);
        }
    }

    std::vector<char> visited(nodeCount, 0);
    std::vector<char> onPath(nodeCount, 0);

    // 从场景根节点做迭代式 DFS，计算全局变换。
    // 用显式栈而不是递归，避免恶意文件构造超深节点树导致栈溢出；
    // onPath 用来检测环（glTF 理论上无环，但恶意文件可能有）。
    struct Frame {
        int node = -1;
        std::size_t nextChild = 0;
        Mat4 parentGlobal = Mat4::Identity();
        int depth = 0;
        bool entered = false;
    };
    std::vector<Frame> stack;
    for (const int root : doc_->SceneRoots()) {
        if (root < 0 || static_cast<std::size_t>(root) >= nodeCount) continue;
        stack.clear();
        stack.push_back(Frame{root, 0, Mat4::Identity(), 0, false});
        while (!stack.empty()) {
            const int node = stack.back().node;
            if (!stack.back().entered) {
                stack.back().entered = true;
                onPath[static_cast<std::size_t>(node)] = 1;
                nodeGlobal_[static_cast<std::size_t>(node)] =
                    stack.back().parentGlobal * nodes[static_cast<std::size_t>(node)].local;
                nodeDepth_[static_cast<std::size_t>(node)] = stack.back().depth;
                visited[static_cast<std::size_t>(node)] = 1;
            }
            int child = -1;
            if (stack.back().nextChild < nodes[static_cast<std::size_t>(node)].children.size()) {
                child = nodes[static_cast<std::size_t>(node)].children[stack.back().nextChild++];
            }
            if (child >= 0 && static_cast<std::size_t>(child) < nodeCount &&
                !onPath[static_cast<std::size_t>(child)] && stack.back().depth < kMaxNodeDepth) {
                stack.push_back(Frame{child, 0, nodeGlobal_[static_cast<std::size_t>(node)],
                                      stack.back().depth + 1, false});
                continue;
            }
            onPath[static_cast<std::size_t>(node)] = 0;
            stack.pop_back();
        }
    }

    // 不在场景里的节点（例如被引用但未挂到场景的关节），
    // 沿父链向上找到已解析的祖先再逐级算下来。
    for (std::size_t i = 0; i < nodeCount; ++i) {
        if (visited[i]) continue;
        std::vector<int> chain;
        int current = static_cast<int>(i);
        int guard = 0;
        while (current >= 0 && !visited[static_cast<std::size_t>(current)] &&
               guard++ < kMaxNodeDepth) {
            chain.push_back(current);
            current = nodeParent_[static_cast<std::size_t>(current)];
        }
        Mat4 global = (current >= 0) ? nodeGlobal_[static_cast<std::size_t>(current)]
                                      : Mat4::Identity();
        // 深度必须逐级递增：骨骼数组要按「父在前、子在后」排列，
        // 深度相同会让排序失效，进而把父子骨骼的顺序排错。
        int depth = (current >= 0) ? nodeDepth_[static_cast<std::size_t>(current)] : -1;
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            global = global * nodes[static_cast<std::size_t>(*it)].local;
            nodeGlobal_[static_cast<std::size_t>(*it)] = global;
            ++depth;
            nodeDepth_[static_cast<std::size_t>(*it)] = depth;
            visited[static_cast<std::size_t>(*it)] = 1;
        }
    }
}

void VrmModel::CollectBoneNodes() {
    const std::vector<GltfNode>& nodes = doc_->Nodes();
    const std::size_t nodeCount = nodes.size();
    boneOfNode_.assign(nodeCount, -1);
    nodeOfBone_.clear();

    std::vector<char> isBone(nodeCount, 0);

    // 1. 蒙皮的关节节点。
    for (const GltfSkin& skin : doc_->Skins()) {
        for (const int joint : skin.joints) {
            if (joint < 0 || static_cast<std::size_t>(joint) >= nodeCount) continue;
            isBone[static_cast<std::size_t>(joint)] = 1;
        }
    }

    // 2. 弹簧骨的关节节点（也要作为骨骼，否则无法施加物理旋转）。
    //    链条上的每一个节点都要进骨骼集合——物理模拟是逐关节进行的，
    //    少一个节点链条就断了。
    for (const SpringChain& chain : springChains_) {
        for (const int node : chain.nodes) {
            if (node < 0 || static_cast<std::size_t>(node) >= nodeCount) continue;
            isBone[static_cast<std::size_t>(node)] = 1;
        }
    }

    // 3. 把关节的所有祖先也纳入骨骼集合。
    //    这样每根骨骼的父骨骼一定也在集合里，局部变换的空间才是连续的
    //    （否则骨骼的 world 会丢掉中间祖先的变换）。
    for (std::size_t i = 0; i < nodeCount; ++i) {
        if (!isBone[i]) continue;
        int current = nodeParent_[i];
        int guard = 0;
        while (current >= 0 && guard++ < kMaxNodeDepth) {
            const std::size_t idx = static_cast<std::size_t>(current);
            if (isBone[idx]) break;
            isBone[idx] = 1;
            current = nodeParent_[idx];
        }
    }

    // 4. 按节点深度排序，保证「父在前、子在后」
    //    （ComputeSkinningMatrices 依赖这个顺序逐级累积世界矩阵）。
    std::vector<int> boneNodes;
    boneNodes.reserve(nodeCount);
    for (std::size_t i = 0; i < nodeCount; ++i) {
        if (isBone[i]) boneNodes.push_back(static_cast<int>(i));
    }
    std::stable_sort(boneNodes.begin(), boneNodes.end(), [this](int a, int b) {
        return nodeDepth_[static_cast<std::size_t>(a)] < nodeDepth_[static_cast<std::size_t>(b)];
    });

    // 5. 建立骨骼。
    //
    //    模型朝向变换（VRM 0.x 的 180° 旋转）的逆要先算出来：
    //    逆绑定矩阵要作用在它上面，见第 6 步。
    const Mat4 modelInverse = modelTransform_.Inverse();
    mesh_.bones.clear();
    mesh_.bones.reserve(boneNodes.size());
    boneModelPrefix_.assign(boneNodes.size(), Mat4::Identity());
    boneLocalBind_.clear();
    boneLocalBind_.reserve(boneNodes.size());
    for (std::size_t i = 0; i < boneNodes.size(); ++i) {
        const int nodeIndex = boneNodes[i];
        const std::size_t srcIdx = static_cast<std::size_t>(nodeIndex);
        Bone bone;
        bone.name = nodes[srcIdx].name;
        bone.localBindPose = nodes[srcIdx].local;
        bone.currentLocal = nodes[srcIdx].local;
        bone.world = modelTransform_ * nodeGlobal_[srcIdx];
        // 默认的逆绑定矩阵：绑定姿态下世界矩阵的逆。
        // 只有被 skin 引用的关节会被文件里的 inverseBindMatrices 覆盖；
        // 纯弹簧骨节点（例如头发的末端）没有 skin 数据，
        // 必须用这个默认值，否则蒙皮矩阵在绑定姿态下就不是单位阵。
        bone.inverseBindMatrix = nodeGlobal_[srcIdx].Inverse() * modelInverse;
        bone.parentWorld = Mat4::Identity();
        bone.restPosition = bone.world.TransformPoint(Vec3(0.0f, 0.0f, 0.0f));
        bone.hasTranslation = true;

        const int parent = nodeParent_[srcIdx];
        bone.parent = -1;
        if (parent >= 0) {
            const std::size_t parentIdx = static_cast<std::size_t>(parent);
            if (parentIdx < boneOfNode_.size()) {
                const int mapped = boneOfNode_[parentIdx];
                // 父骨骼必须排在前面，否则世界矩阵累积会算错。
                if (mapped >= 0 && mapped < static_cast<int>(i)) bone.parent = mapped;
            }
        }

        // 骨骼链的根（父骨骼为 -1）需要特殊处理。
        //
        // 契约里的 ComputeSkinningMatrices 会从 currentLocal 重新累积世界矩阵：
        //     world = parentWorld * currentLocal，父为 -1 时 parentWorld 是单位阵。
        // 也就是说，链根骨骼的 currentLocal 直接就是它的世界矩阵。
        // 如果这里只放节点的局部变换，模型朝向变换（VRM 0.x 的 180° 旋转）
        // 就会在累积过程中丢失，绑定姿态下的蒙皮矩阵会变成 M^{-1} 而不是单位阵，
        // 整个模型会莫名其妙地转半圈。
        //
        // 因此链根骨骼的绑定局部变换要改成「模型空间的全局变换」。
        // 注意这里用 nodeGlobal_（glTF 空间）而不是 bone.world，
        // 因为 bone.world 已经乘过 modelTransform_，不能重复乘。
        if (bone.parent < 0) {
            bone.localBindPose = modelTransform_ * nodeGlobal_[srcIdx];
            bone.currentLocal = bone.localBindPose;
            // 链根骨骼的局部变换要表示「模型空间的全局变换」，
            // 动画重建时需要补上节点父级累积的那一段前缀，
            // 前缀 = 模型朝向变换 * 父节点的 glTF 空间全局变换。
            Mat4 prefix = modelTransform_;
            if (parent >= 0 && static_cast<std::size_t>(parent) < nodeGlobal_.size()) {
                prefix = modelTransform_ * nodeGlobal_[static_cast<std::size_t>(parent)];
            }
            boneModelPrefix_[i] = prefix;
        }

        boneOfNode_[srcIdx] = static_cast<int>(i);
        nodeOfBone_.push_back(nodeIndex);
        // 记录节点原始的局部变换：链根骨骼的 localBindPose 已经是
        // 「模型空间的全局变换」，语义与普通骨骼不同，
        // 弹簧骨算偏移方向时必须用这份原始值。
        boneLocalBind_.push_back(nodes[srcIdx].local);
        mesh_.bones.push_back(std::move(bone));
    }

    // 6. 逆绑定矩阵。
    //    glTF 的 inverseBindMatrices 是「关节绑定姿态世界矩阵的逆」，
    //    即 B = inverse(JointWorld_bind)，因此绑定姿态下
    //    bone.world * bone.inverseBindMatrix == 单位阵。
    //    模型朝向变换（VRM 0.x 的 180° 旋转）要作用在等式右边，
    //    所以 inverseBind = B * modelTransform^{-1}。
    for (const GltfSkin& skin : doc_->Skins()) {
        std::vector<Mat4> bindMatrices;
        if (skin.inverseBindMatricesAccessor >= 0) {
            std::string readError;
            if (!doc_->ReadAccessorMat4(skin.inverseBindMatricesAccessor, bindMatrices,
                                        readError)) {
                bindMatrices.clear();
            }
        }
        for (std::size_t p = 0; p < skin.joints.size(); ++p) {
            const int nodeIndex = skin.joints[p];
            if (nodeIndex < 0 || static_cast<std::size_t>(nodeIndex) >= boneOfNode_.size()) continue;
            const int boneIndex = boneOfNode_[static_cast<std::size_t>(nodeIndex)];
            if (boneIndex < 0) continue;
            Mat4 bind = Mat4::Identity();
            if (p < bindMatrices.size()) {
                bind = bindMatrices[p];
            } else {
                // 文件没有提供逆绑定矩阵时，按规范自行计算：
                // 绑定姿态下关节的世界矩阵的逆。
                // 注意这里用 nodeGlobal_ 而不是 bone.world——
                // bone.world 还额外乘了模型朝向变换，
                // 而下面的 modelInverse 会再乘一次，不能重复。
                bind = nodeGlobal_[static_cast<std::size_t>(nodeIndex)].Inverse();
            }
            mesh_.bones[static_cast<std::size_t>(boneIndex)].inverseBindMatrix =
                bind * modelInverse;
        }
    }
}

bool VrmModel::BuildMeshes(std::string& error) {
    const std::vector<GltfNode>& nodes = doc_->Nodes();
    const std::vector<GltfMesh>& meshes = doc_->Meshes();

    // 按场景树的先序遍历处理网格节点：
    // 顶点顺序必须与导出器一致（VRM 0.x 的 blendShape 绑定用的是全局顶点下标）。
    std::vector<int> stack;
    std::vector<char> visited(nodes.size(), 0);
    for (auto it = doc_->SceneRoots().rbegin(); it != doc_->SceneRoots().rend(); ++it) {
        stack.push_back(*it);
    }
    while (!stack.empty()) {
        const int nodeIndex = stack.back();
        stack.pop_back();
        if (nodeIndex < 0 || static_cast<std::size_t>(nodeIndex) >= nodes.size()) continue;
        if (visited[static_cast<std::size_t>(nodeIndex)]) continue;
        visited[static_cast<std::size_t>(nodeIndex)] = 1;

        const GltfNode& node = nodes[static_cast<std::size_t>(nodeIndex)];
        // 子节点逆序入栈，保证出栈顺序与文件顺序一致。
        for (auto it = node.children.rbegin(); it != node.children.rend(); ++it) {
            stack.push_back(*it);
        }

        if (node.mesh < 0 || static_cast<std::size_t>(node.mesh) >= meshes.size()) continue;
        const GltfMesh& srcMesh = meshes[static_cast<std::size_t>(node.mesh)];

        // 网格节点的世界变换（含场景根节点）。
        const Mat4 meshGlobal = modelTransform_ * nodeGlobal_[static_cast<std::size_t>(nodeIndex)];
        const Mat4 meshInverse = meshGlobal.Inverse();
        // 法线矩阵 = 逆转置的左上 3x3。
        const Mat4 normalMatrix = meshInverse.Transpose();

        for (const GltfPrimitive& prim : srcMesh.primitives) {
            if (prim.mode != 4) {
                // 只支持三角形；其它模式（点、线、条带）直接跳过。
                continue;
            }
            if (prim.positionAccessor < 0) continue;

            std::vector<Vec3> positions;
            std::string readError;
            if (!doc_->ReadAccessorVec3(prim.positionAccessor, positions, readError)) continue;

            std::vector<Vec3> normals;
            if (prim.normalAccessor >= 0) {
                doc_->ReadAccessorVec3(prim.normalAccessor, normals, readError);
            }
            std::vector<Vec2> uvs;
            if (prim.texcoordAccessor >= 0) {
                std::vector<float> raw;
                if (doc_->ReadAccessorFloats(prim.texcoordAccessor, raw, readError)) {
                    for (std::size_t i = 0; i + 1 < raw.size(); i += 2) {
                        uvs.push_back(Vec2(raw[i], raw[i + 1]));
                    }
                }
            }
            std::vector<Vec4> colors;
            if (prim.colorAccessor >= 0) {
                doc_->ReadAccessorVec4(prim.colorAccessor, colors, readError);
            }
            std::vector<std::uint32_t> joints;
            if (prim.jointsAccessor >= 0) {
                doc_->ReadAccessorU32(prim.jointsAccessor, joints, readError);
            }
            std::vector<float> weights;
            if (prim.weightsAccessor >= 0) {
                doc_->ReadAccessorFloats(prim.weightsAccessor, weights, readError);
            }
            std::vector<std::uint32_t> indices;
            if (prim.indicesAccessor >= 0) {
                doc_->ReadAccessorU32(prim.indicesAccessor, indices, readError);
            }
            if (indices.empty()) {
                // 没有索引时按顺序生成三角形。
                indices.resize(positions.size());
                for (std::size_t i = 0; i < positions.size(); ++i) {
                    indices[i] = static_cast<std::uint32_t>(i);
                }
            }

            // 关节下标是「skin.joints 数组内的位置」，需要映射到骨骼下标。
            std::vector<int> jointToBone;
            if (node.skin >= 0 && static_cast<std::size_t>(node.skin) < doc_->Skins().size()) {
                const GltfSkin& skin = doc_->Skins()[static_cast<std::size_t>(node.skin)];
                jointToBone.resize(skin.joints.size(), -1);
                for (std::size_t j = 0; j < skin.joints.size(); ++j) {
                    const int jointNode = skin.joints[j];
                    if (jointNode < 0 || static_cast<std::size_t>(jointNode) >= boneOfNode_.size()) {
                        continue;
                    }
                    jointToBone[j] = boneOfNode_[static_cast<std::size_t>(jointNode)];
                }
            }

            const std::size_t vertexBase = mesh_.vertices.size();
            mesh_.vertices.resize(vertexBase + positions.size());
            for (std::size_t i = 0; i < positions.size(); ++i) {
                Vertex& vertex = mesh_.vertices[vertexBase + i];
                vertex.position = meshGlobal.TransformPoint(positions[i]);
                if (i < normals.size()) {
                    const Vec3& n = normals[i];
                    vertex.normal = Normalize(Vec3(normalMatrix.At(0, 0) * n.x +
                                                       normalMatrix.At(0, 1) * n.y +
                                                       normalMatrix.At(0, 2) * n.z,
                                                   normalMatrix.At(1, 0) * n.x +
                                                       normalMatrix.At(1, 1) * n.y +
                                                       normalMatrix.At(1, 2) * n.z,
                                                   normalMatrix.At(2, 0) * n.x +
                                                       normalMatrix.At(2, 1) * n.y +
                                                       normalMatrix.At(2, 2) * n.z));
                }
                if (i < uvs.size()) vertex.uv = uvs[i];
                if (i < colors.size()) {
                    vertex.color = colors[i];
                } else {
                    vertex.color = Vec4(1.0f, 1.0f, 1.0f, 1.0f);
                }

                vertex.joints = {0, 0, 0, 0};
                vertex.weights = {1.0f, 0.0f, 0.0f, 0.0f};
                std::array<float, 4> w = {0.0f, 0.0f, 0.0f, 0.0f};
                for (int k = 0; k < 4; ++k) {
                    const std::size_t idx = i * 4 + static_cast<std::size_t>(k);
                    if (idx >= weights.size()) break;
                    w[static_cast<std::size_t>(k)] = weights[idx];
                }
                NormalizeWeights(w);
                for (int k = 0; k < 4; ++k) {
                    const std::size_t idx = i * 4 + static_cast<std::size_t>(k);
                    std::uint32_t joint = 0;
                    if (idx < joints.size()) joint = joints[idx];
                    int bone = -1;
                    if (joint < jointToBone.size()) bone = jointToBone[joint];
                    if (bone < 0 || w[static_cast<std::size_t>(k)] <= 0.0f) continue;
                    vertex.joints[static_cast<std::size_t>(k)] = static_cast<std::uint32_t>(bone);
                    vertex.weights[static_cast<std::size_t>(k)] = w[static_cast<std::size_t>(k)];
                }
            }

            // 材质（每个 primitive 一个子网格）。
            // 这里先记录 glTF 的材质下标，BuildMaterials 建立正式材质列表后再修正。
            SkinnedMesh::SubMesh subMesh;
            subMesh.materialIndex = prim.material >= 0 ? prim.material : 0;
            subMesh.indexOffset = static_cast<std::uint32_t>(mesh_.indices.size());
            for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
                const std::uint32_t i0 = indices[i];
                const std::uint32_t i1 = indices[i + 1];
                const std::uint32_t i2 = indices[i + 2];
                // 顶点下标越界的三角形直接丢弃。
                if (i0 >= positions.size() || i1 >= positions.size() || i2 >= positions.size()) {
                    continue;
                }
                mesh_.indices.push_back(static_cast<std::uint32_t>(vertexBase) + i0);
                mesh_.indices.push_back(static_cast<std::uint32_t>(vertexBase) + i1);
                mesh_.indices.push_back(static_cast<std::uint32_t>(vertexBase) + i2);
            }
            subMesh.indexCount =
                static_cast<std::uint32_t>(mesh_.indices.size()) - subMesh.indexOffset;
            if (subMesh.indexCount > 0) mesh_.subMeshes.push_back(subMesh);

            // 记录顶点区间，供 morph 目标构建使用。
            MeshPrimitiveRange range;
            range.nodeIndex = nodeIndex;
            range.meshIndex = node.mesh;
            range.primitiveIndex = static_cast<int>(&prim - &srcMesh.primitives[0]);
            range.vertexBase = static_cast<std::uint32_t>(vertexBase);
            range.vertexCount = static_cast<std::uint32_t>(positions.size());
            meshRanges_.push_back(range);
        }
    }
    (void)error;
    return true;
}

// ---------------------------------------------------------------------------
// 材质与贴图
// ---------------------------------------------------------------------------

void VrmModel::BuildMaterials() {
    const std::vector<GltfMaterial>& source = doc_->Materials();

    // 只保留被使用到的材质，并建立 glTF 下标 -> 内部下标的映射。
    std::vector<int> materialMap(source.size(), -1);
    std::vector<int> usedMaterials;
    for (const SkinnedMesh::SubMesh& sub : mesh_.subMeshes) {
        if (sub.materialIndex < 0 ||
            static_cast<std::size_t>(sub.materialIndex) >= source.size()) {
            continue;
        }
        const std::size_t index = static_cast<std::size_t>(sub.materialIndex);
        if (materialMap[index] >= 0) continue;
        // 内部材质列表里的下标就是它在 usedMaterials 中的位置。
        materialMap[index] = static_cast<int>(usedMaterials.size());
        usedMaterials.push_back(sub.materialIndex);
    }

    // 贴图列表：所有被材质引用到的 glTF 纹理。
    std::vector<int> textureMap(doc_->Textures().size(), -1);
    textureBytes_.clear();
    auto textureIndexOf = [&](int gltfTexture) -> int {
        if (gltfTexture < 0 || static_cast<std::size_t>(gltfTexture) >= textureMap.size()) {
            return -1;
        }
        const std::size_t index = static_cast<std::size_t>(gltfTexture);
        int& slot = textureMap[index];
        if (slot >= 0) return slot;
        slot = static_cast<int>(textureBytes_.size());
        std::vector<unsigned char> bytes;
        const GltfTexture& texture = doc_->Textures()[index];
        if (texture.source >= 0) {
            std::string mime;
            std::string error;
            if (!doc_->GetImageBytes(texture.source, bytes, mime, error)) bytes.clear();
        }
        textureBytes_.push_back(std::move(bytes));
        return slot;
    };

    mesh_.materials.clear();
    mesh_.materials.reserve(usedMaterials.size());
    for (const int gltfIndex : usedMaterials) {
        const GltfMaterial& src = source[static_cast<std::size_t>(gltfIndex)];
        Material mat;
        mat.name = src.name;

        // 基础色：MToon 的漫反射同样来自 pbrMetallicRoughness。
        mat.diffuse = src.baseColorFactor;
        mat.emissive = src.emissiveFactor;
        mat.ambient = Vec3(0.0f, 0.0f, 0.0f);
        mat.specular = Vec3(0.0f, 0.0f, 0.0f);
        mat.shininess = 5.0f;
        mat.useToon = true;
        mat.unlit = src.unlit;
        // VRM 模型大量使用双面材质（尤其是头发与衣物），
        // 未显式声明时按双面渲染，避免出现「模型破洞」。
        mat.doubleSided = src.doubleSided;

        if (src.hasMtoon) {
            mat.shadeColor = src.shadeColorFactor;
            mat.rimColor = src.rimColorFactor;
            mat.rimLightingMix = src.rimLightingMixFactor;
            mat.rimFresnelPower = src.parametricRimFresnelPowerFactor;
            mat.shadeShift = src.shadingShiftFactor;
            mat.shadeToony = src.shadingToonyFactor;
            mat.outlineColor = src.outlineColorFactor;
            // 描边宽度：worldCoordinates 已经是米；screenCoordinates 是屏幕比例，
            // 这里按模型高度折算成世界单位（渲染时模型基本占满屏幕）。
            if (src.outlineWidthMode == "worldCoordinates") {
                mat.outlineWidth = src.outlineWidthFactor;
            } else if (src.outlineWidthMode == "screenCoordinates") {
                mat.outlineWidth = src.outlineWidthFactor * modelHeight_;
            }
        }

        // 贴图：
        //   baseColorTex <- 基础色贴图
        //   toonRampTex  <- MToon 的阴影色贴图（shadeMultiply / shadeColor）
        //   normalTex    <- 法线贴图
        //   sphereTex    <- MToon 的边缘光贴图（rimMultiply / rimColor）
        mat.baseColorTex = textureIndexOf(src.baseColorTexture);
        mat.normalTex = textureIndexOf(src.normalTexture);
        if (src.hasMtoon) {
            const int shade = src.shadeMultiplyTexture >= 0 ? src.shadeMultiplyTexture
                                                            : src.shadeColorTexture;
            mat.toonRampTex = textureIndexOf(shade);
            const int rim = src.rimMultiplyTexture >= 0 ? src.rimMultiplyTexture
                                                        : src.rimColorTexture;
            mat.sphereTex = textureIndexOf(rim);
        }

        if (mat.outlineWidth > 0.0f) hasOutline_ = true;
        mesh_.materials.push_back(std::move(mat));
    }

    // 修正子网格的材质下标（现在是内部材质列表的下标）。
    for (SkinnedMesh::SubMesh& sub : mesh_.subMeshes) {
        int mapped = -1;
        if (sub.materialIndex >= 0 &&
            static_cast<std::size_t>(sub.materialIndex) < materialMap.size()) {
            mapped = materialMap[static_cast<std::size_t>(sub.materialIndex)];
        }
        sub.materialIndex = mapped >= 0 ? mapped : 0;
    }
    // 极端情况下（例如材质的下标越界）仍然要保证至少有一个材质，
    // 否则渲染层拿不到材质，模型会整个不可见。
    if (mesh_.materials.empty() && !mesh_.subMeshes.empty()) {
        mesh_.materials.push_back(Material{});
    }
}

// ---------------------------------------------------------------------------
// morph 目标
// ---------------------------------------------------------------------------

void VrmModel::BuildMorphs() {
    mesh_.morphs.clear();
    morphBaseOfNode_.assign(doc_->Nodes().size(), -1);
    // glTF mesh 索引 -> 该 mesh 在各引用节点上建立的 morph 段。
    // VRM 0.x 的 blendShapeGroups 用 (mesh, index) 引用 morph 目标，
    // 这里提前把表建好，供 BuildExpressions 使用。
    morphBasesOfMesh_.assign(doc_->Meshes().size(), std::vector<int>());
    nodeMorphRanges_.clear();
    if (meshRanges_.empty()) return;

    const std::size_t vertexCount = mesh_.vertices.size();

    // 按网格节点分组（meshRanges_ 已经按场景遍历顺序排列）。
    std::vector<int> nodeOrder;
    for (const MeshPrimitiveRange& range : meshRanges_) {
        if (nodeOrder.empty() || nodeOrder.back() != range.nodeIndex) {
            nodeOrder.push_back(range.nodeIndex);
        }
    }

    for (const int nodeIndex : nodeOrder) {
        const GltfNode& node = doc_->Nodes()[static_cast<std::size_t>(nodeIndex)];
        if (node.mesh < 0 || static_cast<std::size_t>(node.mesh) >= doc_->Meshes().size()) {
            continue;
        }
        const GltfMesh& srcMesh = doc_->Meshes()[static_cast<std::size_t>(node.mesh)];

        // 该网格的 morph 目标数量：各 primitive 取最大值。
        std::size_t targetCount = 0;
        for (const MeshPrimitiveRange& range : meshRanges_) {
            if (range.nodeIndex != nodeIndex) continue;
            if (range.primitiveIndex < 0 ||
                static_cast<std::size_t>(range.primitiveIndex) >= srcMesh.primitives.size()) {
                continue;
            }
            const GltfPrimitive& prim =
                srcMesh.primitives[static_cast<std::size_t>(range.primitiveIndex)];
            targetCount = std::max(targetCount, prim.targets.size());
        }
        if (targetCount == 0) continue;

        const int base = static_cast<int>(mesh_.morphs.size());
        for (std::size_t t = 0; t < targetCount; ++t) {
            SkinnedMesh::MorphTarget target;
            if (t < srcMesh.targetNames.size()) {
                target.name = srcMesh.targetNames[t];
            } else {
                std::ostringstream oss;
                oss << "morph_" << t;
                target.name = oss.str();
            }
            target.positions.assign(vertexCount, Vec3(0.0f, 0.0f, 0.0f));
            target.normals.assign(vertexCount, Vec3(0.0f, 0.0f, 0.0f));
            mesh_.morphs.push_back(std::move(target));
        }
        morphBaseOfNode_[static_cast<std::size_t>(nodeIndex)] = base;
        // 记录「glTF mesh -> morph 段」的映射，供 VRM 0.x 的 blendShapeGroups 使用。
        // 同一个 mesh 可能被多个节点引用（例如带 BlendShape 与不带的两份网格），
        // 每个节点都会建立独立的 morph 段，这里把段都记下来。
        if (node.mesh >= 0 && static_cast<std::size_t>(node.mesh) < morphBasesOfMesh_.size()) {
            morphBasesOfMesh_[static_cast<std::size_t>(node.mesh)].push_back(base);
        }

        // 填充各 primitive 的目标数据。
        const Mat4 meshGlobal = modelTransform_ * nodeGlobal_[static_cast<std::size_t>(nodeIndex)];
        const Mat4 normalMatrix = meshGlobal.Inverse().Transpose();
        for (const MeshPrimitiveRange& range : meshRanges_) {
            if (range.nodeIndex != nodeIndex) continue;
            if (range.primitiveIndex < 0 ||
                static_cast<std::size_t>(range.primitiveIndex) >= srcMesh.primitives.size()) {
                continue;
            }
            const GltfPrimitive& prim =
                srcMesh.primitives[static_cast<std::size_t>(range.primitiveIndex)];
            for (std::size_t t = 0; t < prim.targets.size(); ++t) {
                const int morphIndex = base + static_cast<int>(t);
                if (morphIndex < 0 || static_cast<std::size_t>(morphIndex) >= mesh_.morphs.size()) {
                    continue;
                }
                SkinnedMesh::MorphTarget& target = mesh_.morphs[static_cast<std::size_t>(morphIndex)];
                std::vector<Vec3> deltas;
                std::string readError;
                if (prim.targets[t].position >= 0 &&
                    doc_->ReadAccessorVec3(prim.targets[t].position, deltas, readError)) {
                    for (std::size_t i = 0; i < deltas.size(); ++i) {
                        const std::size_t vertex = range.vertexBase + i;
                        if (vertex >= target.positions.size()) break;
                        // morph 的位移同样是模型空间坐标，需要乘上网格节点的世界变换。
                        target.positions[vertex] = meshGlobal.TransformDirection(deltas[i]);
                    }
                }
                deltas.clear();
                if (prim.targets[t].normal >= 0 &&
                    doc_->ReadAccessorVec3(prim.targets[t].normal, deltas, readError)) {
                    for (std::size_t i = 0; i < deltas.size(); ++i) {
                        const std::size_t vertex = range.vertexBase + i;
                        if (vertex >= target.normals.size()) break;
                        const Vec3& n = deltas[i];
                        target.normals[vertex] =
                            Vec3(normalMatrix.At(0, 0) * n.x + normalMatrix.At(0, 1) * n.y +
                                     normalMatrix.At(0, 2) * n.z,
                                 normalMatrix.At(1, 0) * n.x + normalMatrix.At(1, 1) * n.y +
                                     normalMatrix.At(1, 2) * n.z,
                                 normalMatrix.At(2, 0) * n.x + normalMatrix.At(2, 1) * n.y +
                                     normalMatrix.At(2, 2) * n.z);
                    }
                }
            }
        }

        NodeMorphRange nodeRange;
        nodeRange.node = nodeIndex;
        nodeRange.base = base;
        nodeRange.count = static_cast<int>(targetCount);
        nodeMorphRanges_.push_back(nodeRange);
    }
}

// ---------------------------------------------------------------------------
// humanoid 骨骼
// ---------------------------------------------------------------------------

void VrmModel::BuildHumanoid() {
    const nlohmann::json& root = doc_->Json();
    const nlohmann::json* extensions = JsonChild(root, "extensions");
    if (extensions == nullptr) return;
    const nlohmann::json* vrm = JsonChild(*extensions, "VRMC_vrm");
    if (vrm == nullptr) vrm = JsonChild(*extensions, "VRM");
    if (vrm == nullptr) return;

    const nlohmann::json* meta = JsonChild(*vrm, "meta");
    if (meta != nullptr) metaName_ = JsonString(*meta, "name");

    // lookAt 的角度范围（rangeMap 的 outputScale 单位是度）。
    lookYawLimit_ = kDefaultLookYaw;
    lookPitchLimit_ = kDefaultLookPitch;
    const nlohmann::json* lookAt = JsonChild(*vrm, "lookAt");
    if (lookAt != nullptr) {
        const nlohmann::json* horizontal = JsonChild(*lookAt, "rangeMapHorizontal");
        if (horizontal != nullptr) {
            const float scale = JsonFloat(*horizontal, "outputScale", 15.0f);
            lookYawLimit_ = scale * kPi / 180.0f;
        }
        const nlohmann::json* vertical = JsonChild(*lookAt, "rangeMapVertical");
        if (vertical != nullptr) {
            const float scale = JsonFloat(*vertical, "outputScale", 10.0f);
            lookPitchLimit_ = scale * kPi / 180.0f;
        }
    }

    const nlohmann::json* humanoid = JsonChild(*vrm, "humanoid");
    if (humanoid == nullptr) return;
    const nlohmann::json* bones = JsonValue(*humanoid, "humanBones");
    if (bones == nullptr) return;

    // humanBones 有两种写法：
    //   对象形式：{ "hips": { "node": 0 }, ... }（VRM 1.0 规范与大多数导出器）
    //   数组形式：[{ "bone": "hips", "node": 0 }, ...]（部分 VRM 0.x 导出器）
    std::vector<std::pair<std::string, int>> mapping;
    if (bones->is_object()) {
        for (auto it = bones->begin(); it != bones->end(); ++it) {
            mapping.emplace_back(it.key(), JsonInt(it.value(), "node", -1));
        }
    } else if (bones->is_array()) {
        for (const auto& entry : *bones) {
            const std::string name = JsonString(entry, "bone");
            if (name.empty()) continue;
            mapping.emplace_back(name, JsonInt(entry, "node", -1));
        }
    }

    auto boneOf = [&](const char* name) -> int {
        for (const auto& entry : mapping) {
            if (entry.first != name) continue;
            const int node = entry.second;
            if (node < 0 || static_cast<std::size_t>(node) >= boneOfNode_.size()) return -1;
            return boneOfNode_[static_cast<std::size_t>(node)];
        }
        return -1;
    };

    headBone_ = boneOf("head");
    leftEyeBone_ = boneOf("leftEye");
    rightEyeBone_ = boneOf("rightEye");
    bodyBone_ = boneOf("hips");
    if (bodyBone_ < 0) bodyBone_ = boneOf("spine");

    mesh_.headBoneIndex = headBone_;
    mesh_.leftEyeBoneIndex = leftEyeBone_;
    mesh_.rightEyeBoneIndex = rightEyeBone_;
}

// ---------------------------------------------------------------------------
// 表情
// ---------------------------------------------------------------------------

void VrmModel::BuildExpressions() {
    mesh_.expressions.clear();

    const nlohmann::json& root = doc_->Json();
    const nlohmann::json* extensions = JsonChild(root, "extensions");
    if (extensions == nullptr) return;

    const nlohmann::json* vrm = JsonChild(*extensions, "VRMC_vrm");
    if (vrm != nullptr) {
        const nlohmann::json* expressions = JsonChild(*vrm, "expressions");
        if (expressions == nullptr) return;

        // preset 表情：名字固定，分类直接对应。
        const nlohmann::json* preset = JsonChild(*expressions, "preset");
        if (preset != nullptr && preset->is_object()) {
            for (auto it = preset->begin(); it != preset->end(); ++it) {
                SkinnedMesh::Expression expr;
                expr.name = it.key();
                expr.category = ExpressionCategory(it.key(), true);
                expr.isBlendShape = true;
                const bool binary = JsonBool(it.value(), "isBinary", false);
                const auto binds = it.value().find("morphTargetBinds");
                if (binds != it.value().end() && binds->is_array()) {
                    for (const auto& bind : *binds) {
                        const int node = JsonInt(bind, "node", -1);
                        const int index = JsonInt(bind, "index", -1);
                        const float weight = JsonFloat(bind, "weight", 1.0f);
                        if (node < 0 || static_cast<std::size_t>(node) >= morphBaseOfNode_.size()) {
                            continue;
                        }
                        const int base = morphBaseOfNode_[static_cast<std::size_t>(node)];
                        if (base < 0 || index < 0) continue;
                        const int morph = base + index;
                        if (morph < 0 || static_cast<std::size_t>(morph) >= mesh_.morphs.size()) {
                            continue;
                        }
                        expr.bindings.emplace_back(morph, weight);
                    }
                }
                if (expr.bindings.empty()) continue;
                mesh_.expressions.push_back(std::move(expr));
                expressionBinary_.push_back(binary ? 1 : 0);
            }
        }

        // custom 表情：分类按名字推断。
        const nlohmann::json* custom = JsonChild(*expressions, "custom");
        if (custom != nullptr && custom->is_object()) {
            for (auto it = custom->begin(); it != custom->end(); ++it) {
                SkinnedMesh::Expression expr;
                expr.name = it.key();
                expr.category = ExpressionCategory(it.key(), false);
                expr.isBlendShape = true;
                const bool binary = JsonBool(it.value(), "isBinary", false);
                const auto binds = it.value().find("morphTargetBinds");
                if (binds != it.value().end() && binds->is_array()) {
                    for (const auto& bind : *binds) {
                        const int node = JsonInt(bind, "node", -1);
                        const int index = JsonInt(bind, "index", -1);
                        const float weight = JsonFloat(bind, "weight", 1.0f);
                        if (node < 0 || static_cast<std::size_t>(node) >= morphBaseOfNode_.size()) {
                            continue;
                        }
                        const int base = morphBaseOfNode_[static_cast<std::size_t>(node)];
                        if (base < 0 || index < 0) continue;
                        const int morph = base + index;
                        if (morph < 0 || static_cast<std::size_t>(morph) >= mesh_.morphs.size()) {
                            continue;
                        }
                        expr.bindings.emplace_back(morph, weight);
                    }
                }
                if (expr.bindings.empty()) continue;
                mesh_.expressions.push_back(std::move(expr));
                expressionBinary_.push_back(binary ? 1 : 0);
            }
        }
        return;
    }

    // --- VRM 0.x：blendShapeMaster -----------------------------------------
    const nlohmann::json* vrm0 = JsonChild(*extensions, "VRM");
    if (vrm0 == nullptr) return;
    const nlohmann::json* master = JsonChild(*vrm0, "blendShapeMaster");
    if (master == nullptr) return;

    // VRM 0.x 有两种写法，真实导出器（UniVRM 等）用的是第一种：
    //
    //   1. blendShapeGroups[]：
    //      binds[] 用 (mesh, index) 引用 morph 目标——
    //      mesh 是 glTF 的 mesh 下标，index 是该 mesh 内的 morph 目标下标，
    //      weight 是 0~100 的百分比。
    //
    //   2. blendShapes[]（VRM 0.0 的老写法）：
    //      用 targetNames 引用 morph 目标名，binds 是逐顶点的权重。
    const nlohmann::json* groups = JsonArray(*master, "blendShapeGroups");
    if (groups != nullptr) {
        for (const auto& group : *groups) {
            if (!group.is_object()) continue;
            const std::string name = JsonString(group, "name");
            if (name.empty()) continue;

            SkinnedMesh::Expression expr;
            expr.name = name;
            // presetName 是 VRM 0.x 的分类名（joy / sorrow / a ...），
            // 映射成 VRM 1.0 的分类名；映射不到时按表情名推断。
            expr.category = ExpressionCategoryVrm0(JsonString(group, "presetName"));
            if (expr.category.empty()) expr.category = ExpressionCategory(name, false);
            expr.isBlendShape = true;
            const bool binary = JsonBool(group, "isBinary", false);

            const auto binds = group.find("binds");
            if (binds != group.end() && binds->is_array()) {
                for (const auto& bind : *binds) {
                    const int meshIndex = JsonInt(bind, "mesh", -1);
                    const int index = JsonInt(bind, "index", -1);
                    // weight 是 0~100 的百分比，归一化到 0~1。
                    float weight = JsonFloat(bind, "weight", 0.0f);
                    if (weight > 1.0f) weight /= 100.0f;
                    if (meshIndex < 0 ||
                        static_cast<std::size_t>(meshIndex) >= morphBasesOfMesh_.size()) {
                        continue;
                    }
                    if (index < 0) continue;
                    // 同一个 mesh 可能被多个节点引用，每个节点都有独立的 morph 段，
                    // 表情要同时驱动这些段，否则会出现「只有一份网格变形」的问题。
                    for (const int base : morphBasesOfMesh_[static_cast<std::size_t>(meshIndex)]) {
                        const int morph = base + index;
                        if (morph < 0 || static_cast<std::size_t>(morph) >= mesh_.morphs.size()) {
                            continue;
                        }
                        expr.bindings.emplace_back(morph, Clamp(weight, 0.0f, 1.0f));
                    }
                }
            }
            if (expr.bindings.empty()) continue;
            mesh_.expressions.push_back(std::move(expr));
            expressionBinary_.push_back(binary ? 1 : 0);
        }
        return;
    }

    const nlohmann::json* blendShapes = JsonArray(*master, "blendShapes");
    if (blendShapes == nullptr) return;

    const std::size_t vertexCount = mesh_.vertices.size();
    for (const auto& shape : *blendShapes) {
        const std::string name = JsonString(shape, "name");
        if (name.empty()) continue;

        // 找到该表情对应的 morph 目标（VRM 0.x 用 targetNames 引用）。
        int targetMorph = -1;
        const auto targetNames = shape.find("targetNames");
        if (targetNames != shape.end() && targetNames->is_array() && !targetNames->empty()) {
            const std::string targetName =
                (*targetNames)[0].is_string() ? (*targetNames)[0].get<std::string>() : "";
            for (std::size_t i = 0; i < mesh_.morphs.size(); ++i) {
                if (mesh_.morphs[i].name == targetName) {
                    targetMorph = static_cast<int>(i);
                    break;
                }
            }
        }

        SkinnedMesh::MorphTarget morph;
        morph.name = name;
        morph.positions.assign(vertexCount, Vec3(0.0f, 0.0f, 0.0f));
        morph.normals.assign(vertexCount, Vec3(0.0f, 0.0f, 0.0f));

        // binds 里的 vertexIndex 是模型的全局顶点下标，weight 是位移的缩放系数。
        const auto binds = shape.find("binds");
        if (binds != shape.end() && binds->is_array() && targetMorph >= 0) {
            const SkinnedMesh::MorphTarget& target =
                mesh_.morphs[static_cast<std::size_t>(targetMorph)];
            for (const auto& bind : *binds) {
                const int vertexIndex = JsonInt(bind, "vertexIndex", -1);
                const float weight = JsonFloat(bind, "weight", 0.0f);
                if (vertexIndex < 0 || static_cast<std::size_t>(vertexIndex) >= vertexCount) {
                    continue;
                }
                const std::size_t v = static_cast<std::size_t>(vertexIndex);
                morph.positions[v] = target.positions[v] * weight;
                morph.normals[v] = target.normals[v] * weight;
            }
        }
        const int morphIndex = static_cast<int>(mesh_.morphs.size());
        mesh_.morphs.push_back(std::move(morph));

        SkinnedMesh::Expression expr;
        expr.name = name;
        expr.category = ExpressionCategory(name, false);
        expr.isBlendShape = true;
        expr.bindings.emplace_back(morphIndex, 1.0f);
        mesh_.expressions.push_back(std::move(expr));
        expressionBinary_.push_back(0);
    }
}

// ---------------------------------------------------------------------------
// 弹簧骨
// ---------------------------------------------------------------------------

void VrmModel::CollectSpringChains() {
    springChains_.clear();
    springColliders_.clear();

    const nlohmann::json& root = doc_->Json();
    const nlohmann::json* extensions = JsonChild(root, "extensions");
    if (extensions == nullptr) return;

    const nlohmann::json* springBone = nullptr;
    if (vrmVersion_ == 0) {
        const nlohmann::json* vrm0 = JsonChild(*extensions, "VRM");
        if (vrm0 != nullptr) springBone = JsonChild(*vrm0, "secondaryAnimation");
    } else {
        springBone = JsonChild(*extensions, "VRMC_springBone");
    }
    if (springBone == nullptr) return;

    const std::size_t nodeCount = doc_->Nodes().size();

    // --- 碰撞体 ------------------------------------------------------------
    //
    // VRM 1.0：顶层 colliders[]，每项含 node 与 shape.sphere / shape.capsule。
    // VRM 0.x：碰撞体内联在 colliderGroups[].colliders 里，没有顶层数组。
    const nlohmann::json* collidersJson = JsonArray(*springBone, "colliders");
    if (collidersJson != nullptr) {
        for (const auto& item : *collidersJson) {
            SpringCollider collider;
            collider.node = JsonInt(item, "node", -1);
            collider.offset = JsonVec3(item, "offset", Vec3(0.0f, 0.0f, 0.0f));
            collider.radius = JsonFloat(item, "radius", 0.0f);
            // VRM 1.0 的最终形态：形状放在 shape 下面。
            const nlohmann::json* shape = JsonChild(item, "shape");
            if (shape != nullptr) {
                const nlohmann::json* sphere = JsonChild(*shape, "sphere");
                if (sphere != nullptr) {
                    collider.offset = JsonVec3(*sphere, "offset", collider.offset);
                    collider.radius = JsonFloat(*sphere, "radius", collider.radius);
                }
                const nlohmann::json* capsule = JsonChild(*shape, "capsule");
                if (capsule != nullptr) {
                    // 胶囊体：契约里每根弹簧骨只保存一个碰撞球，
                    // 这里取胶囊的起点球（offset + radius），忽略尾端。
                    collider.offset = JsonVec3(*capsule, "offset", collider.offset);
                    collider.radius = JsonFloat(*capsule, "radius", collider.radius);
                }
            }
            springColliders_.push_back(collider);
        }
    }

    // --- 碰撞组 ------------------------------------------------------------
    //
    // VRM 1.0：colliderGroups[].colliders 是 colliders 数组的下标。
    // VRM 0.x：colliderGroups[].colliders 是内联对象（offset + radius），
    //   此时碰撞组自带节点，碰撞体坐标是相对该节点的局部偏移。
    struct ColliderGroup {
        int node = -1;
        std::vector<int> colliders;  ///< 指向 springColliders_ 的下标
    };
    std::vector<ColliderGroup> groups;
    const nlohmann::json* groupsJson = JsonArray(*springBone, "colliderGroups");
    if (groupsJson != nullptr) {
        for (const auto& item : *groupsJson) {
            ColliderGroup group;
            group.node = JsonInt(item, "node", -1);
            const auto list = item.find("colliders");
            if (list != item.end() && list->is_array()) {
                for (const auto& entry : *list) {
                    if (entry.is_object()) {
                        // VRM 0.x 的内联碰撞球。
                        SpringCollider collider;
                        collider.node = group.node;
                        // VRM 0.x 的 offset 是 {x,y,z} 对象。
                        const nlohmann::json* offset = JsonChild(entry, "offset");
                        if (offset != nullptr) {
                            collider.offset = Vec3(JsonFloat(*offset, "x", 0.0f),
                                                   JsonFloat(*offset, "y", 0.0f),
                                                   JsonFloat(*offset, "z", 0.0f));
                        }
                        collider.radius = JsonFloat(entry, "radius", 0.0f);
                        group.colliders.push_back(static_cast<int>(springColliders_.size()));
                        springColliders_.push_back(collider);
                        continue;
                    }
                    if (!entry.is_number()) continue;
                    const int index = JsonIntValue(entry, -1);
                    // VRM 1.0：colliders 数组下标。
                    if (index < 0 || static_cast<std::size_t>(index) >= springColliders_.size()) {
                        continue;
                    }
                    group.colliders.push_back(index);
                }
            }
            groups.push_back(std::move(group));
        }
    }

    // --- 链条 --------------------------------------------------------------
    //
    // VRM 1.0：springs[].joints[] 逐关节描述，参数在关节级。
    // VRM 0.x：secondaryAnimation.boneGroups[]，参数在组级，链条是 bones[]。
    if (vrmVersion_ == 0) {
        const nlohmann::json* groups0 = JsonArray(*springBone, "boneGroups");
        if (groups0 == nullptr) return;
        for (const auto& item : *groups0) {
            SpringChain chain;
            chain.name = JsonString(item, "name");
            // VRM 0.x 的字段名有两个变体：规范文档写的是 "stiffiness"（拼写错误），
            // 实际导出器多用 "stiffness"，两个都读。
            float stiffness = JsonFloat(item, "stiffness", -1.0f);
            if (stiffness < 0.0f) stiffness = JsonFloat(item, "stiffiness", 1.0f);
            const float dragForce = JsonFloat(item, "dragForce", 0.5f);
            const float gravityPower = JsonFloat(item, "gravityPower", 0.0f);
            const float hitRadius = JsonFloat(item, "hitRadius", 0.0f);
            // VRM 0.x 的 gravityDir 是 {x,y,z} 对象。
            Vec3 gravityDir(0.0f, -1.0f, 0.0f);
            const nlohmann::json* dir = JsonChild(item, "gravityDir");
            if (dir != nullptr) {
                gravityDir = Vec3(JsonFloat(*dir, "x", 0.0f), JsonFloat(*dir, "y", -1.0f),
                                  JsonFloat(*dir, "z", 0.0f));
            }

            const auto bones = item.find("bones");
            if (bones != item.end() && bones->is_array()) {
                for (const auto& b : *bones) {
                    const int node = JsonIntValue(b, -1);
                    if (node < 0 || static_cast<std::size_t>(node) >= nodeCount) continue;
                    chain.nodes.push_back(node);
                    chain.stiffness.push_back(stiffness);
                    chain.dragForce.push_back(dragForce);
                    chain.gravityPower.push_back(gravityPower);
                    chain.hitRadius.push_back(hitRadius);
                    chain.gravityDir.push_back(gravityDir);
                }
            }
            if (chain.nodes.empty()) continue;

            // 碰撞组：boneGroups[].colliderGroups 是碰撞组数组的下标。
            const auto groupList = item.find("colliderGroups");
            if (groupList != item.end() && groupList->is_array()) {
                for (const auto& g : *groupList) {
                    const int index = JsonIntValue(g, -1);
                    if (index < 0 || static_cast<std::size_t>(index) >= groups.size()) continue;
                    for (const int collider : groups[static_cast<std::size_t>(index)].colliders) {
                        chain.colliders.push_back(collider);
                    }
                }
            }
            springChains_.push_back(std::move(chain));
        }
        return;
    }

    // VRM 1.0
    const nlohmann::json* springs = JsonArray(*springBone, "springs");
    if (springs == nullptr) return;
    for (const auto& item : *springs) {
        SpringChain chain;
        chain.name = JsonString(item, "name");

        // 链级默认值（关节级参数缺失时使用）。
        const float defaultStiffness = JsonFloat(item, "stiffness", 1.0f);
        const float defaultDrag = JsonFloat(item, "dragForce", 0.5f);
        const float defaultGravityPower = JsonFloat(item, "gravityPower", 0.0f);
        const float defaultHitRadius = JsonFloat(item, "hitRadius", 0.0f);
        const Vec3 defaultGravityDir =
            JsonVec3(item, "gravityDir", Vec3(0.0f, -1.0f, 0.0f));

        const auto joints = JsonArray(item, "joints");
        if (joints != nullptr) {
            for (const auto& joint : *joints) {
                const int node = JsonInt(joint, "node", -1);
                if (node < 0 || static_cast<std::size_t>(node) >= nodeCount) continue;
                chain.nodes.push_back(node);
                chain.stiffness.push_back(JsonFloat(joint, "stiffness", defaultStiffness));
                chain.dragForce.push_back(JsonFloat(joint, "dragForce", defaultDrag));
                chain.gravityPower.push_back(JsonFloat(joint, "gravityPower", defaultGravityPower));
                chain.hitRadius.push_back(JsonFloat(joint, "hitRadius", defaultHitRadius));
                chain.gravityDir.push_back(JsonVec3(joint, "gravityDir", defaultGravityDir));
            }
        }
        if (chain.nodes.empty()) {
            // 早期写法：jointNode 指定链条根节点，整棵子树都是链条。
            const int jointNode = JsonInt(item, "jointNode", -1);
            if (jointNode < 0 || static_cast<std::size_t>(jointNode) >= nodeCount) continue;
            std::vector<int> descendants;
            CollectDescendants(jointNode, descendants);
            chain.nodes.push_back(jointNode);
            for (const int node : descendants) chain.nodes.push_back(node);
            for (std::size_t i = 0; i < chain.nodes.size(); ++i) {
                chain.stiffness.push_back(defaultStiffness);
                chain.dragForce.push_back(defaultDrag);
                chain.gravityPower.push_back(defaultGravityPower);
                chain.hitRadius.push_back(defaultHitRadius);
                chain.gravityDir.push_back(defaultGravityDir);
            }
        }

        // 碰撞组：springs[].colliderGroups 是碰撞组数组的下标。
        const auto groupList = item.find("colliderGroups");
        if (groupList != item.end() && groupList->is_array()) {
            for (const auto& g : *groupList) {
                const int index = JsonIntValue(g, -1);
                if (index < 0 || static_cast<std::size_t>(index) >= groups.size()) continue;
                for (const int collider : groups[static_cast<std::size_t>(index)].colliders) {
                    chain.colliders.push_back(collider);
                }
            }
        }
        springChains_.push_back(std::move(chain));
    }
}

void VrmModel::BuildSpringBones() {
    mesh_.springBones.clear();
    springStates_.clear();

    for (const SpringChain& chain : springChains_) {
        if (chain.nodes.empty()) continue;

        // 碰撞球的世界坐标与半径。
        // 契约里每根弹簧骨只保存一个碰撞球，这里取链条的第一个碰撞体。
        Vec3 colliderCenter;
        float colliderRadius = 0.0f;
        if (!chain.colliders.empty()) {
            const int colliderIndex = chain.colliders.front();
            if (colliderIndex >= 0 &&
                static_cast<std::size_t>(colliderIndex) < springColliders_.size()) {
                const SpringCollider& collider =
                    springColliders_[static_cast<std::size_t>(colliderIndex)];
                if (collider.node >= 0 &&
                    static_cast<std::size_t>(collider.node) < nodeGlobal_.size()) {
                    const Mat4 world =
                        modelTransform_ * nodeGlobal_[static_cast<std::size_t>(collider.node)];
                    colliderCenter = world.TransformPoint(collider.offset);
                }
                colliderRadius = collider.radius;
            }
        }

        for (std::size_t i = 0; i < chain.nodes.size(); ++i) {
            const int node = chain.nodes[i];
            if (node < 0 || static_cast<std::size_t>(node) >= boneOfNode_.size()) continue;
            const int boneIndex = boneOfNode_[static_cast<std::size_t>(node)];
            if (boneIndex < 0) continue;

            SkinnedMesh::SpringBone def;
            def.name = chain.name.empty() ? mesh_.bones[static_cast<std::size_t>(boneIndex)].name
                                          : chain.name;
            def.boneIndex = boneIndex;
            // 父骨骼用节点树的父节点，保证与骨骼层级一致。
            def.parentBoneIndex = mesh_.bones[static_cast<std::size_t>(boneIndex)].parent;
            def.colliderCenter = colliderCenter;
            def.colliderRadius = colliderRadius;
            def.stiffness = chain.stiffness[i];
            def.dragForce = chain.dragForce[i];
            def.gravityPower = chain.gravityPower[i];
            def.hitRadius = chain.hitRadius[i];

            // 重力方向从模型空间转到世界空间。
            const Vec3 gravityWorld =
                Normalize(modelTransform_.TransformDirection(chain.gravityDir[i]));

            SpringState state;
            state.position = mesh_.bones[static_cast<std::size_t>(boneIndex)].world.TransformPoint(
                Vec3(0.0f, 0.0f, 0.0f));
            state.previous = state.position;
            state.gravityDir = gravityWorld;
            if (def.parentBoneIndex >= 0) {
                const Vec3 parentPosition =
                    mesh_.bones[static_cast<std::size_t>(def.parentBoneIndex)].world.TransformPoint(
                        Vec3(0.0f, 0.0f, 0.0f));
                state.restLength = Length(state.position - parentPosition);
            }
            springStates_.push_back(state);
            mesh_.springBones.push_back(def);
        }
    }
}

// ---------------------------------------------------------------------------
// 节点图辅助
// ---------------------------------------------------------------------------

void VrmModel::CollectDescendants(int node, std::vector<int>& out) const {
    if (node < 0 || static_cast<std::size_t>(node) >= nodeParent_.size()) return;
    const std::vector<GltfNode>& nodes = doc_->Nodes();
    // 迭代式遍历，避免深递归；限制深度与数量，防止恶意文件构造超大子树。
    std::vector<std::pair<int, int>> stack;  // (节点, 深度)
    stack.emplace_back(node, 0);
    while (!stack.empty() && out.size() < kMaxSpringChain) {
        const int current = stack.back().first;
        const int depth = stack.back().second;
        stack.pop_back();
        if (depth >= kMaxSpringDepth) continue;
        for (auto it = nodes[static_cast<std::size_t>(current)].children.rbegin();
             it != nodes[static_cast<std::size_t>(current)].children.rend(); ++it) {
            const int child = *it;
            if (child < 0 || static_cast<std::size_t>(child) >= nodes.size()) continue;
            out.push_back(child);
            stack.emplace_back(child, depth + 1);
        }
    }
}

bool VrmModel::IsAncestorNode(int ancestor, int node) const {
    if (ancestor < 0 || node < 0) return false;
    if (ancestor == node) return true;
    int current = node;
    int guard = 0;
    while (current >= 0 && guard++ < kMaxNodeDepth) {
        if (current == ancestor) return true;
        if (static_cast<std::size_t>(current) >= nodeParent_.size()) break;
        current = nodeParent_[static_cast<std::size_t>(current)];
    }
    return false;
}

// ---------------------------------------------------------------------------
// Update
// ---------------------------------------------------------------------------

void VrmModel::Update(const RenderContext& ctx) {
    if (!loaded_) return;

    float dt = ctx.deltaTime;
    if (!(dt > 0.0f) || dt > 0.1f) dt = 1.0f / 60.0f;  // 防止切后台回来时动画跳变

    // 复位骨骼，避免上一帧的变换累积。
    for (Bone& bone : mesh_.bones) bone.currentLocal = bone.localBindPose;

    UpdateAnimation(dt);
    UpdateIdle(dt);
    UpdateGaze(ctx, dt);

    // 先算一次世界矩阵：弹簧骨需要父骨骼的世界坐标。
    UpdateSkinningMatrices();
    UpdateSpringBones(dt);
    UpdateSkinningMatrices();

    UpdateBlink(dt);
    UpdateMorphWeights();
}

void VrmModel::UpdateAnimation(float dt) {
    if (animChannels_.empty()) return;
    animTime_ += dt;
    if (animDuration_ > 0.0f) {
        // 待机动画循环播放。
        animTime_ = std::fmod(animTime_, animDuration_);
        if (animTime_ < 0.0f) animTime_ += animDuration_;
    }

    std::vector<float> sampled;
    for (const AnimChannel& channel : animChannels_) {
        if (channel.bone < 0 || static_cast<std::size_t>(channel.bone) >= mesh_.bones.size()) {
            continue;
        }
        if (channel.components <= 0) continue;

        sampled.assign(static_cast<std::size_t>(channel.components), 0.0f);
        SampleChannel(channel, animTime_, sampled.data());

        if (channel.path == 3) {
            // morph 权重通道。
            for (const NodeMorphRange& range : nodeMorphRanges_) {
                if (range.node != channel.node) continue;
                for (int i = 0; i < range.count; ++i) {
                    const int morph = range.base + i;
                    if (morph < 0 || static_cast<std::size_t>(morph) >= animMorphWeights_.size()) {
                        continue;
                    }
                    if (i < static_cast<int>(sampled.size())) {
                        animMorphWeights_[static_cast<std::size_t>(morph)] =
                            Clamp(sampled[static_cast<std::size_t>(i)], 0.0f, 1.0f);
                    }
                }
            }
            continue;
        }

        // TRS 通道：以节点原始的 TRS 为基准，用动画值覆盖对应分量后重新合成。
        Bone& bone = mesh_.bones[static_cast<std::size_t>(channel.bone)];
        const GltfNode& node = doc_->Nodes()[static_cast<std::size_t>(nodeOfBone_[static_cast<std::size_t>(channel.bone)])];
        Vec3 translation = node.translation;
        Vec3 scale = node.scale;
        Vec4 rotation = node.rotation;
        if (channel.path == 0 && sampled.size() >= 3) {
            translation = Vec3(sampled[0], sampled[1], sampled[2]);
        } else if (channel.path == 1 && sampled.size() >= 4) {
            rotation = Vec4(sampled[0], sampled[1], sampled[2], sampled[3]);
        } else if (channel.path == 2 && sampled.size() >= 3) {
            scale = Vec3(sampled[0], sampled[1], sampled[2]);
        } else {
            continue;
        }
        Mat4 local = Mat4::Translation(translation) * QuaternionToMat4(rotation) *
                     Mat4::Scale(scale);
        // 骨骼链根（父骨骼为 -1）的局部变换在契约里被直接当作世界矩阵使用，
        // 所以必须补上「模型朝向变换 * 父节点全局变换」这一段前缀，
        // 否则播放动画时链根会丢掉模型朝向变换，整个模型会转半圈。
        if (bone.parent < 0 &&
            static_cast<std::size_t>(channel.bone) < boneModelPrefix_.size()) {
            local = boneModelPrefix_[static_cast<std::size_t>(channel.bone)] * local;
        }
        bone.currentLocal = local;
    }
}

void VrmModel::UpdateIdle(float dt) {
    // 有 glTF 动画时以动画为准，不再叠加待机动作。
    if (hasAnimation_) return;
    idleTime_ += dt;
    idleYaw_ = std::sin(idleTime_ * kIdleSwaySpeed) * kIdleSwayYawAmp;
    idlePitch_ = std::sin(idleTime_ * kIdleBreathSpeed) * kIdleSwayPitchAmp;
    if (bodyBone_ < 0 || static_cast<std::size_t>(bodyBone_) >= mesh_.bones.size()) return;
    Bone& bone = mesh_.bones[static_cast<std::size_t>(bodyBone_)];
    bone.currentLocal = bone.currentLocal * Mat4::RotationY(idleYaw_) * Mat4::RotationX(idlePitch_);
}

void VrmModel::UpdateGaze(const RenderContext& ctx, float dt) {
    if (ctx.hasGlobalCursor) {
        // localCursor 已归一化到 [-1, 1]，y 向上为正（OpenGL 约定）。
        const float x = Clamp(ctx.localCursor.x, -1.0f, 1.0f);
        const float y = Clamp(ctx.localCursor.y, -1.0f, 1.0f);
        targetGazeYaw_ = x * lookYawLimit_;
        // RotationX(正角) 会让模型朝向从 +Z 偏向 -Y（即低头），
        // 所以「向上看」需要负角度。
        targetGazePitch_ = -y * lookPitchLimit_;
    } else {
        targetGazeYaw_ = 0.0f;
        targetGazePitch_ = 0.0f;
    }

    // 指数平滑：避免鼠标抖动导致头部瞬移。
    const float alpha = 1.0f - std::exp(-kGazeSmoothing * dt);
    gazeYaw_ += (targetGazeYaw_ - gazeYaw_) * alpha;
    gazePitch_ += (targetGazePitch_ - gazePitch_) * alpha;

    // 视线追踪统一用骨骼方式驱动（VRM lookAt.type == "expression" 的模型同样适用），
    // 找不到对应骨骼时直接跳过（优雅降级）。
    if (headBone_ >= 0 && static_cast<std::size_t>(headBone_) < mesh_.bones.size()) {
        Bone& bone = mesh_.bones[static_cast<std::size_t>(headBone_)];
        bone.currentLocal = bone.currentLocal * Mat4::RotationY(gazeYaw_) *
                            Mat4::RotationX(gazePitch_);
    }
    if (leftEyeBone_ >= 0 && static_cast<std::size_t>(leftEyeBone_) < mesh_.bones.size()) {
        Bone& bone = mesh_.bones[static_cast<std::size_t>(leftEyeBone_)];
        bone.currentLocal = bone.currentLocal * Mat4::RotationY(gazeYaw_) *
                            Mat4::RotationX(gazePitch_);
    }
    if (rightEyeBone_ >= 0 && static_cast<std::size_t>(rightEyeBone_) < mesh_.bones.size()) {
        Bone& bone = mesh_.bones[static_cast<std::size_t>(rightEyeBone_)];
        bone.currentLocal = bone.currentLocal * Mat4::RotationY(gazeYaw_) *
                            Mat4::RotationX(gazePitch_);
    }
}

void VrmModel::UpdateBlink(float dt) {
    blinkTimer_ -= dt;
    if (blinkTimer_ <= 0.0f) {
        // 开始一次眨眼，并重新排定下一次的时间。
        blinking_ = true;
        blinkValue_ = 0.0f;
        // 伪随机：使用一个简单的线性同余序列，避免引入随机数引擎的状态。
        static unsigned int seed = 12345u;
        seed = seed * 1103515245u + 12345u;
        const float r = static_cast<float>((seed >> 16) & 0x7FFF) / 32767.0f;
        blinkTimer_ = kBlinkMinInterval + r * (kBlinkMaxInterval - kBlinkMinInterval) +
                      blinkDuration_;
    }
    float weight = 0.0f;
    if (blinking_) {
        blinkValue_ += dt / blinkDuration_;
        if (blinkValue_ >= 1.0f) {
            blinkValue_ = 0.0f;
            blinking_ = false;
        }
        // 闭眼再睁眼：用正弦曲线模拟眼皮的开合。
        weight = std::sin(blinkValue_ * kPi);
    }

    // 驱动 blink 表情。
    for (std::size_t i = 0; i < mesh_.expressions.size() && i < expressionWeights_.size(); ++i) {
        const SkinnedMesh::Expression& expr = mesh_.expressions[i];
        if (expr.category != "blink" && expr.name != "blink") continue;
        if (weight > expressionWeights_[i]) expressionWeights_[i] = weight;
    }
}

void VrmModel::UpdateSpringBones(float dt) {
    if (mesh_.springBones.empty() || springStates_.size() != mesh_.springBones.size()) return;

    // 按骨骼下标排序，保证父骨骼先于子骨骼更新
    // （子骨骼的锚点依赖父骨骼当帧的世界位置）。
    std::vector<std::size_t> order(springStates_.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [this](std::size_t a, std::size_t b) {
        return mesh_.springBones[a].boneIndex < mesh_.springBones[b].boneIndex;
    });

    const float dtSq = dt * dt;
    for (const std::size_t index : order) {
        const SkinnedMesh::SpringBone& def = mesh_.springBones[index];
        SpringState& state = springStates_[index];
        const int boneIndex = def.boneIndex;
        if (boneIndex < 0 || static_cast<std::size_t>(boneIndex) >= mesh_.bones.size()) continue;
        Bone& bone = mesh_.bones[static_cast<std::size_t>(boneIndex)];

        // 锚点：父骨骼的原点。
        Vec3 anchor(0.0f, 0.0f, 0.0f);
        Mat4 anchorWorld = Mat4::Identity();
        if (def.parentBoneIndex >= 0 &&
            static_cast<std::size_t>(def.parentBoneIndex) < mesh_.bones.size()) {
            anchorWorld = mesh_.bones[static_cast<std::size_t>(def.parentBoneIndex)].world;
            anchor = anchorWorld.TransformPoint(Vec3(0.0f, 0.0f, 0.0f));
        }

        // verlet 积分：位置 + 阻尼 + 重力。
        const Vec3 delta = (state.position - state.previous) * def.dragForce;
        state.previous = state.position;
        state.position = state.position + delta + state.gravityDir * (def.gravityPower * dtSq);

        // 长度约束：骨骼长度保持不变（只做上限约束，避免被拉长）。
        const Vec3 toAnchor = state.position - anchor;
        const float length = Length(toAnchor);
        if (state.restLength > kEpsilon && length > state.restLength && length > kEpsilon) {
            state.position = anchor + toAnchor * (state.restLength / length);
        }

        // 碰撞球约束。
        if (def.colliderRadius > 0.0f) {
            const Vec3 fromCollider = state.position - def.colliderCenter;
            const float distance = Length(fromCollider);
            const float minDistance = def.colliderRadius + def.hitRadius;
            if (distance < minDistance && distance > kEpsilon) {
                state.position = def.colliderCenter + fromCollider * (minDistance / distance);
            }
        }

        // 把模拟结果写回骨骼：绕父骨骼旋转，使骨骼指向模拟位置。
        //
        // 基准必须用节点原始的局部变换（boneLocalBind_），
        // 不能用 bone.localBindPose——骨骼链根的 localBindPose 是
        // 「模型空间的全局变换」，语义完全不同，会把偏移方向算错。
        if (bone.parent < 0 ||
            static_cast<std::size_t>(boneIndex) >= boneLocalBind_.size()) {
            // 骨骼链根没有父骨骼，锚点与旋转基准都不成立，跳过模拟。
            continue;
        }
        const Mat4& baseLocal = boneLocalBind_[static_cast<std::size_t>(boneIndex)];
        const Vec3 bindDirection = Vec3(baseLocal.At(0, 3), baseLocal.At(1, 3),
                                         baseLocal.At(2, 3));
        const Vec3 targetDirection = state.position - anchor;
        if (Length(bindDirection) > kEpsilon && Length(targetDirection) > kEpsilon) {
            // 目标方向转换到父骨骼的局部空间。
            const Mat4 anchorInverse = anchorWorld.Inverse();
            const Vec3 localDirection =
                Normalize(anchorInverse.TransformDirection(targetDirection));
            const Mat4 rotation = RotationBetween(Normalize(bindDirection), localDirection);
            bone.currentLocal = rotation * baseLocal;
            // 更新世界矩阵，后面的子骨骼才能拿到正确的父骨骼位置。
            bone.parentWorld = anchorWorld;
            bone.world = anchorWorld * bone.currentLocal;
        }
    }
}

void VrmModel::UpdateMorphWeights() {
    if (mesh_.morphs.empty()) return;
    std::vector<float> weights(mesh_.morphs.size(), 0.0f);

    // 表情驱动的权重。
    for (std::size_t i = 0; i < mesh_.expressions.size() && i < expressionWeights_.size(); ++i) {
        float value = expressionWeights_[i];
        if (value <= 0.0f) continue;
        // 二元表情（VRM 1.0 的 isBinary，例如 blink）只有开 / 关两种状态，
        // 直接线性插值会让眼皮半睁半闭，看起来很假，所以量化成 0 / 1。
        if (i < expressionBinary_.size() && expressionBinary_[i] != 0) {
            value = value >= 0.5f ? 1.0f : 0.0f;
        }
        for (const auto& binding : mesh_.expressions[i].bindings) {
            if (binding.first < 0 || static_cast<std::size_t>(binding.first) >= weights.size()) {
                continue;
            }
            const float weight = Clamp(value * binding.second, 0.0f, 1.0f);
            weights[static_cast<std::size_t>(binding.first)] =
                std::max(weights[static_cast<std::size_t>(binding.first)], weight);
        }
    }
    // 动画通道的权重优先。
    for (std::size_t i = 0; i < animMorphWeights_.size() && i < weights.size(); ++i) {
        if (animMorphWeights_[i] > 0.0f) weights[i] = animMorphWeights_[i];
    }

    if (weights != morphWeights_) {
        morphWeights_ = weights;
        morphDirty_ = true;
    }
    if (!morphDirty_) return;

    // 顶点数据需要重新上传（渲染层的 SkinnedMeshRenderer 只接受静态顶点缓冲）。
    mesh_.vertices = baseVertices_;
    ApplyMorphs(mesh_, morphWeights_, mesh_.vertices);
    if (gpuReady_) {
        renderer_.Upload(mesh_);
        uploadedMorphWeights_ = morphWeights_;
        morphDirty_ = false;
    }
}

void VrmModel::UpdateSkinningMatrices() {
    skinningMatrices_.clear();
    ComputeSkinningMatrices(mesh_, skinningMatrices_);
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void VrmModel::Render(const RenderContext& ctx) {
    if (!loaded_) return;
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

void VrmModel::EnsureGpuResources() {
    if (gpuReady_) return;
    gpuReady_ = true;

    renderer_.Upload(mesh_);

    textures_.clear();
    textures_.reserve(textureBytes_.size());
    for (const std::vector<unsigned char>& bytes : textureBytes_) {
        Texture texture;
        if (!bytes.empty()) {
            DecodedImage image;
            if (DecodeImage(bytes.data(), bytes.size(), image)) {
                texture.Create(image.pixels.data(), image.width, image.height, image.channels,
                               true, true);
            }
        }
        // 解码失败时保留无效纹理，渲染层会回退到纯色。
        textures_.push_back(std::move(texture));
    }
    renderer_.SetTextures(std::move(textures_));
}

Mat4 VrmModel::ComputeViewProjection(int viewportWidth, int viewportHeight,
                                      float renderScale) const {
    const float aspect = static_cast<float>(viewportWidth) / static_cast<float>(viewportHeight);

    // 包围盒决定取景范围：模型高度决定纵向范围，宽度按宽高比换算。
    const float modelWidth = std::max(mesh_.boundsMax.x - mesh_.boundsMin.x, kEpsilon);
    const float height = std::max(modelHeight_, kEpsilon);
    float halfHeight = std::max(height * 0.5f, (modelWidth * 0.5f) / std::max(aspect, kEpsilon));
    halfHeight *= kFramingMargin;
    // 渲染缩放：视体越小，模型在窗口里显示得越大。
    halfHeight /= std::max(renderScale, 0.01f);

    // 相机放在模型正前方（+Z）。glTF / VRM 1.0 的模型正面朝 +Z，
    // VRM 0.x 在载入时已经整体旋转 180°，因此这里对两种版本都成立。
    const float distance = std::max(height, modelWidth) * 4.0f;
    const Vec3 eye(center_.x, center_.y, center_.z + distance);
    const Mat4 view = LookAt(eye, center_, Vec3(0.0f, 1.0f, 0.0f));

    const float nearZ = std::max(distance - halfHeight * 2.0f - height, 0.01f);
    const float farZ = distance + halfHeight * 2.0f + height;
    const Mat4 proj = Ortho(-halfHeight * aspect, halfHeight * aspect, -halfHeight, halfHeight,
                            nearZ, farZ);
    return proj * view;
}

// ---------------------------------------------------------------------------
// 其它接口
// ---------------------------------------------------------------------------

void VrmModel::OnResize(int width, int height) {
    viewportWidth_ = width;
    viewportHeight_ = height;
}

void VrmModel::SetExpressionWeight(const std::string& name, float weight) {
    for (std::size_t i = 0; i < mesh_.expressions.size() && i < expressionWeights_.size(); ++i) {
        if (mesh_.expressions[i].name == name) {
            expressionWeights_[i] = Clamp(weight, 0.0f, 1.0f);
            return;
        }
    }
}

std::string VrmModel::DisplayName() const {
    if (!metaName_.empty()) return metaName_;
    if (!path_.empty()) return BaseName(path_);
    return "VRM Model";
}

// ---------------------------------------------------------------------------
// 动画
// ---------------------------------------------------------------------------

void VrmModel::BuildAnimations() {
    animChannels_.clear();
    animDuration_ = 0.0f;
    hasAnimation_ = false;

    const std::vector<GltfAnimation>& animations = doc_->Animations();
    for (const GltfAnimation& animation : animations) {
        for (const GltfAnimationChannel& channel : animation.channels) {
            if (channel.sampler < 0 ||
                static_cast<std::size_t>(channel.sampler) >= animation.samplers.size()) {
                continue;
            }
            const GltfAnimationSampler& sampler =
                animation.samplers[static_cast<std::size_t>(channel.sampler)];

            std::vector<float> times;
            std::vector<float> values;
            std::string readError;
            if (!doc_->ReadAccessorFloats(sampler.input, times, readError)) continue;
            if (!doc_->ReadAccessorFloats(sampler.output, values, readError)) continue;
            if (times.empty() || values.empty()) continue;

            AnimChannel anim;
            anim.node = channel.node;
            anim.bone = (anim.node >= 0 &&
                         static_cast<std::size_t>(anim.node) < boneOfNode_.size())
                            ? boneOfNode_[static_cast<std::size_t>(anim.node)]
                            : -1;
            if (sampler.interpolation == "STEP") {
                anim.interpolation = 1;
            } else if (sampler.interpolation == "CUBICSPLINE") {
                anim.interpolation = 2;
            } else {
                anim.interpolation = 0;
            }

            // 通道类型决定分量数。
            if (channel.path == "translation") {
                anim.path = 0;
                anim.components = 3;
            } else if (channel.path == "rotation") {
                anim.path = 1;
                anim.components = 4;
            } else if (channel.path == "scale") {
                anim.path = 2;
                anim.components = 3;
            } else if (channel.path == "weights") {
                anim.path = 3;
                anim.components = static_cast<int>(values.size() / times.size());
                if (anim.components <= 0) continue;
            } else {
                continue;  // 未知的通道类型直接忽略
            }

            // 时间戳必须是递增的；这里排序并归一化到从 0 开始。
            std::vector<std::pair<float, float>> sorted;
            sorted.reserve(times.size());
            for (std::size_t i = 0; i < times.size(); ++i) sorted.emplace_back(times[i], 0.0f);
            std::stable_sort(sorted.begin(), sorted.end(),
                             [](const std::pair<float, float>& a,
                                const std::pair<float, float>& b) { return a.first < b.first; });
            const float timeOffset = sorted.front().first;
            anim.times.clear();
            anim.times.reserve(sorted.size());
            for (const auto& entry : sorted) anim.times.push_back(entry.first - timeOffset);

            // 输出数据按关键帧顺序排列，CUBICSPLINE 每个关键帧有 3 组值。
            const std::size_t comps = static_cast<std::size_t>(anim.components);
            const std::size_t stride = (anim.interpolation == 2) ? comps * 3 : comps;
            if (values.size() < anim.times.size() * stride) {
                // 数据量不足，丢弃这条通道（不猜、不越界读）。
                continue;
            }
            anim.values.assign(values.begin(), values.begin() + anim.times.size() * stride);

            animDuration_ = std::max(animDuration_, anim.times.back());
            animChannels_.push_back(std::move(anim));
        }
    }
    hasAnimation_ = !animChannels_.empty();
}

void VrmModel::SampleChannel(const AnimChannel& channel, float time, float* out) const {
    const std::size_t comps = static_cast<std::size_t>(channel.components);
    const std::size_t keyCount = channel.times.size();
    if (keyCount == 0 || comps == 0) {
        for (std::size_t i = 0; i < comps; ++i) out[i] = 0.0f;
        return;
    }
    const std::size_t stride = (channel.interpolation == 2) ? comps * 3 : comps;

    if (keyCount == 1) {
        const std::size_t base = (channel.interpolation == 2) ? comps : 0;
        for (std::size_t i = 0; i < comps; ++i) out[i] = channel.values[base + i];
        return;
    }

    // 找到当前时间所在的关键帧区间。
    std::size_t key = 0;
    while (key + 1 < keyCount && channel.times[key + 1] <= time) ++key;
    const float t0 = channel.times[key];
    const float t1 = (key + 1 < keyCount) ? channel.times[key + 1] : t0;
    const float delta = t1 - t0;
    const float u = (delta > kEpsilon) ? Clamp((time - t0) / delta, 0.0f, 1.0f) : 0.0f;

    const std::size_t base0 = key * stride;
    const std::size_t base1 = (key + 1 < keyCount) ? (key + 1) * stride : base0;

    if (channel.interpolation == 1) {
        // STEP：保持前一个关键帧的值。
        for (std::size_t i = 0; i < comps; ++i) out[i] = channel.values[base0 + i];
        return;
    }
    if (channel.interpolation == 2) {
        // CUBICSPLINE：Hermite 插值，切线已经按规范乘过时间差。
        const float u2 = u * u;
        const float u3 = u2 * u;
        const float h00 = 2.0f * u3 - 3.0f * u2 + 1.0f;
        const float h10 = u3 - 2.0f * u2 + u;
        const float h01 = -2.0f * u3 + 3.0f * u2;
        const float h11 = u3 - u2;
        for (std::size_t i = 0; i < comps; ++i) {
            const float p0 = channel.values[base0 + comps + i];
            const float m0 = channel.values[base0 + 2 * comps + i];  // 当前关键帧的出切线
            const float p1 = channel.values[base1 + comps + i];
            const float m1 = channel.values[base1 + i];                // 下一关键帧的入切线
            out[i] = h00 * p0 + h10 * delta * m0 + h01 * p1 + h11 * delta * m1;
        }
        return;
    }

    // LINEAR：旋转用四元数插值，其余用线性插值。
    if (channel.path == 1 && comps == 4) {
        const Vec4 a(channel.values[base0], channel.values[base0 + 1], channel.values[base0 + 2],
                     channel.values[base0 + 3]);
        const Vec4 b(channel.values[base1], channel.values[base1 + 1], channel.values[base1 + 2],
                     channel.values[base1 + 3]);
        const Vec4 result = NlerpQuat(a, b, u);
        out[0] = result.x;
        out[1] = result.y;
        out[2] = result.z;
        out[3] = result.w;
        return;
    }
    for (std::size_t i = 0; i < comps; ++i) {
        out[i] = channel.values[base0 + i] +
                 (channel.values[base1 + i] - channel.values[base0 + i]) * u;
    }
}

}  // namespace ppet::vrm
