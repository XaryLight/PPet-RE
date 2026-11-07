// PPet - C++ 移植版
// Live2D 物理（physics3.json）解析与摆锤求解。
//
// 【为什么需要它】
//   Live2D 的「头发摇曳」不是动作动画，而是**物理模拟**：
//   模型把若干参数（如 ParamHairFront / Param6）声明为「物理输出」，
//   运行时由摆锤链根据头部/身体的倾斜与位移实时算出这些参数的值。
//   没有物理层，头发就是死的；用户反馈的「Live2D 头发摇曳不明显」正是缺这一层。
//
// 【算法来源】
//   本模块严格复刻 Cubism 官方运行时的摆锤算法（CubismPhysics）：
//     · 输入归一化：把输入参数按其自身 min/max 线性映射到
//       Normalization 声明的区间（中点对应 Default）；
//     · 摆锤链：每组物理是一串粒子，第 0 个是根部（被输入推动），
//       后续粒子用 Verlet 风格积分 + 距离约束（Radius）挂在父粒子下；
//     · 重力：Gravity 方向绕根部旋转（由 Angle 类输入给出），
//       每个粒子按 Acceleration 缩放；
//     · 空气阻力：相邻两帧重力方向之差除以 AirResistance 后旋转粒子连线；
//     · 输出：取粒子相对父粒子的位移，按 Type（X/Y/Angle）换算成参数值，
//       乘 Scale、考虑 Reflect，最后按 Weight 与参数原值混合。
//
//   参考实现：Live2D Cubism SDK for Unity 的 Framework/Physics（Live2D 开放软件许可），
//   以及 Cubism Native Framework 5 的 CubismPhysics::Evaluate。
//
// 【physics3.json 的两种格式】
//   1) Cubism 3+ 标准格式（官方规范 Live2D/CubismSpecs · FileFormats/physics3.json.md）：
//      Input / Output / Vertices 都是数组，顶层有 Meta。
//   2) Mobill / Cubism2 变体：Input / Output 也是数组，但 Vertices 用
//      「绝对位置 + Radius」描述，且缺少部分 Meta 字段。
//   本模块两种都能解析（对数组/对象两种写法都做兼容），
//   因为用户手上的模型正是 Mobill 变体，而 FULASKA 用的模型是标准格式。
//
// 【纯逻辑约束】
//   本文件不碰 Cubism / GL / Qt / 文件 IO，只做数值计算，
//   因此可以脱离 SDK 做单元测试（tools/live2d_physics_test.cpp）。
#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace ppet::live2d {

/// 物理输入 / 输出的作用类型。
enum class PhysicsType {
    X,      ///< 水平位移
    Y,      ///< 垂直位移
    Angle,  ///< 角度（绕根部旋转重力方向）
};

/// 一个物理输入：某个模型参数如何推动摆锤。
struct PhysicsInput {
    std::string parameter;  ///< 输入参数名（如 "ParamAngleX"）
    float weight = 100.0f;  ///< 影响力（0~100）
    PhysicsType type = PhysicsType::X;
    bool reflect = false;   ///< 是否反向
};

/// 一个物理输出：摆锤的哪个量写进哪个模型参数。
struct PhysicsOutput {
    std::string parameter;   ///< 输出参数名（如 "ParamHairFront"）
    int vertexIndex = 0;     ///< 读第几个粒子（0 起点；1 表示第一个子粒子）
    float scale = 1.0f;      ///< 缩放
    float weight = 100.0f;   ///< 影响力（0~100）
    PhysicsType type = PhysicsType::Angle;
    bool reflect = false;    ///< 是否反向
};

/// 摆锤链上的一个粒子（官方称 Vertex）。
///
/// 注意：官方运行时**只读 Radius**，Position 被忽略 —— 粒子之间的实际距离
/// 由 Radius 决定（这与 Cubism Editor 里「摆锤长度」是同一个值）。
/// 因此本结构保留 position 仅用于兼容旧数据。
struct PhysicsVertex {
    float positionX = 0.0f;      ///< 初始位置 X（运行时忽略）
    float positionY = 0.0f;      ///< 初始位置 Y（运行时忽略）
    float mobility = 1.0f;       ///< 摇摆度：越大越容易晃
    float delay = 1.0f;          ///< 反应速度：越大越跟得上
    float acceleration = 1.0f;   ///< 收敛速度：越大越快回到下垂
    float radius = 0.0f;         ///< 与父粒子的距离（摆锤长度）
};

/// 一个物理设置（= physics3.json 里的一个 PhysicsSetting）。
struct PhysicsSetting {
    std::string id;
    std::vector<PhysicsInput> inputs;
    std::vector<PhysicsOutput> outputs;
    std::vector<PhysicsVertex> vertices;

    // 归一化区间（官方称 Normalization）。
    float positionMin = -10.0f;
    float positionDefault = 0.0f;
    float positionMax = 10.0f;
    float angleMin = -10.0f;
    float angleDefault = 0.0f;
    float angleMax = 10.0f;
};

/// 一整份物理文件。
struct PhysicsFile {
    std::vector<PhysicsSetting> settings;
    /// 物理帧率（0 表示不限速，跟随渲染帧率）。
    float fps = 0.0f;
    /// 重力（Meta.EffectiveForces.Gravity）。
    float gravityX = 0.0f;
    float gravityY = -1.0f;
    /// 风（Meta.EffectiveForces.Wind）。
    float windX = 0.0f;
    float windY = 0.0f;
};

/// 解析 physics3.json 文本。
///
/// @param text  文件内容
/// @param out   解析结果
/// @param error 失败时写入中文原因
/// @return 成功返回 true。
bool ParsePhysicsJson(const std::string& text, PhysicsFile& out, std::string& error);

/// 读取并解析 physics3.json 文件。
bool LoadPhysicsFile(const std::string& path, PhysicsFile& out, std::string& error);

/// Live2D 物理求解器。
///
/// 用法：
///   Live2DPhysics phys;
///   phys.Load(file);
///   phys.Stabilize(params);            // 初始化摆锤到静止姿态
///   phys.Evaluate(dt, params);         // 每帧推进，把输出写进 params
///
/// 其中 params 是「参数名 -> 当前值」的映射，物理会：
///   1) 读取 Input 里声明的参数；
///   2) 计算摆锤；
///   3) 把 Output 的结果写回同名参数。
///
/// 关于「覆盖还是叠加」：官方 Cubism 的物理是**覆盖**输出参数
/// （UpdateOutputParameterValue 里 weight>=1 时直接 parameter.Value = value）。
/// 但那会抹掉动作已经写入的同名字段；本项目采用更稳妥的做法：
///   把物理输出**叠加**到参数当前值上，再按 Weight 混合，
///   这样「动作驱动的姿势」与「物理驱动的头发」可以共存。
class Live2DPhysics {
public:
    /// 载入物理设置（会先清空旧状态）。
    bool Load(const PhysicsFile& file);

    /// 是否已载入有效的物理设置。
    bool Empty() const { return settings_.empty(); }

    /// 物理设置数量。
    std::size_t SettingCount() const { return settings_.size(); }

    /// 读取重力（用于调试面板显示）。
    void Gravity(float& x, float& y) const { x = gravityX_; y = gravityY_; }

    /// 登记模型参数的取值范围（min/max）。
    ///
    /// 物理的输入归一化需要知道参数自身的范围：官方 Cubism 从 moc3 里读，
    /// 本项目则由 Live2DModel 在加载后用 Cubism 的参数范围接口注入。
    /// 没有登记时按 [-1, 1] 处理。
    void SetParameterRange(const std::string& parameter, float min, float max);

    /// 把摆锤置于「静止下垂」姿态，并把该姿态对应的输出值写进参数。
    ///
    /// 官方运行时在加载后会做一次这样的「安定化」，否则第一帧摆锤会从
    /// 水平位置猛然落下（表现为模型开头抖一下）。
    ///
    /// @param params 参数表（会被写入输出参数）
    void Stabilize(std::map<std::string, float>& params);

    /// 推进一帧物理。
    ///
    /// @param dt     时间步长（秒）
    /// @param params 参数表（输入参数被读取，输出参数被写入）
    void Evaluate(float dt, std::map<std::string, float>& params);

    /// 复位到初始状态（下次 Evaluate 前需重新 Stabilize）。
    void Reset();

private:
    /// 一个摆锤链的运行时状态。
    struct Strand {
        struct Particle {
            float x = 0.0f;
            float y = 0.0f;
            float lastX = 0.0f;
            float lastY = 0.0f;
            float lastGravityX = 0.0f;
            float lastGravityY = 1.0f;
            float velocityX = 0.0f;
            float velocityY = 0.0f;
            float mobility = 1.0f;
            float delay = 1.0f;
            float acceleration = 1.0f;
            float radius = 0.0f;
        };
        std::vector<Particle> particles;
        std::vector<float> currentOutputs;   ///< 本步的输出原始值
        std::vector<float> previousOutputs;  ///< 上一步的输出原始值（用于插值）
    };

    /// 计算某个物理设置的输入：产生根部平移与重力旋转角。
    void ComputeInputs(const PhysicsSetting& setting, const std::map<std::string, float>& params,
                        float& outTranslationX, float& outTranslationY, float& outAngle);

    /// 推进一条摆锤链。
    void UpdateStrand(Strand& strand, const PhysicsSetting& setting, float translationX,
                      float translationY, float angle, float dt);

    /// 计算一条摆锤链的输出原始值（写进 strand.currentOutputs）。
    void WriteOutputs(Strand& strand, const PhysicsSetting& setting);

    std::vector<PhysicsSetting> settings_;
    std::vector<Strand> strands_;
    float gravityX_ = 0.0f;
    float gravityY_ = -1.0f;
    float windX_ = 0.0f;
    float windY_ = 0.0f;
    float fps_ = 0.0f;

    /// 模型参数的取值范围（归一化基准）。
    std::map<std::string, std::pair<float, float>> paramRanges_;

    /// 物理自己的计时器（fps_ > 0 时用于固定步长）。
    float accumulator_ = 0.0f;
    /// 上一次的输入缓存（用于帧间插值，对齐官方的固定步长 + 插值行为）。
    std::map<std::string, float> inputCache_;
    bool hasInputCache_ = false;
    /// 上一帧的输出值（用于输出插值）。
    std::map<std::string, float> outputCache_;
};

}  // namespace ppet::live2d
