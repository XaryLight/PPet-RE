// PPet - C++ 移植版
// Cubism 后端抽象层：Live2D 后端与「Cubism Core」之间的唯一接口。
//
// 为什么要有这一层：
//   1. Live2DModel.cpp 里不出现任何 csm* 调用，所有与 Cubism SDK 的耦合都集中在
//      CubismCoreAdapter.cpp 一个文件里，SDK 升级时只需要改那一处。
//   2. 本头文件不暴露任何 Cubism 类型（csmModel / csmVector2 等），
//      因此 Live2DModel.hpp 可以在没有安装 SDK 的机器上编译。
//   3. 可以用桩（stub）替换真实 SDK：桩实现与真实 SDK 提供完全相同的一组
//      csm* 函数，于是「适配层代码」本身也能被编译和运行验证。
//
// 本层用到的 Cubism Core API（Cubism SDK for Native，Core 层，4.x/5.x）：
//   csmGetVersion()                      -> Cubism Core 版本
//   csmReviveMocInPlace()                -> 在对齐内存上「复活」moc 结构
//   csmGetSizeofModel()                  -> model 结构所需字节数
//   csmInitializeModelInPlace()          -> 在对齐内存上初始化 model
//   csmUpdateModel()                     -> 依据当前参数重算顶点
//   csmReadCanvasInfo()                  -> 画布尺寸 / 中心 / 单位像素数
//   csmGetParameterCount()               -> 参数数量
//   csmGetParameterIds()                 -> 参数名数组
//   csmGetParameterValues()              -> 参数值数组（可写）
//   csmGetParameterDefaultValues()       -> 参数默认值
//   csmGetParameterMinimumValues()       -> 参数下界
//   csmGetParameterMaximumValues()       -> 参数上界
//   csmGetDrawableCount()                -> 可绘制部件数量
//   csmGetDrawableVertexCounts()         -> 每个部件的顶点数
//   csmGetDrawableVertexPositions()      -> 顶点坐标（**锯齿数组**，按部件独立）
//   csmGetDrawableVertexUvs()            -> 顶点纹理坐标（**锯齿数组**）
//   csmGetDrawableIndexCounts()          -> 每个部件的索引数
//   csmGetDrawableIndices()              -> 三角形索引（**锯齿数组**，索引空间独立）
//   csmGetDrawableTextureIndices()       -> 每个部件使用的纹理序号
//   csmGetDrawableOpacities()            -> 每个部件的不透明度
//   csmGetDrawableRenderOrders()         -> 每个部件的渲染顺序
//   csmGetDrawableConstantFlags()        -> 每个部件的常量标志（混合 / 双面 / 反转遮罩）
//   csmGetDrawableDynamicFlags()         -> 每个部件的动态标志（可见性等）
//   csmResetDrawableDynamicFlags()       -> 复位动态标志
//
// 重要说明（与官方 Core API Reference r12/r13 一致）：
//   - Core 层**不提供**绘制函数（csmDrawModel / csmSetupEyeBlink / csmSetupMotion /
//     csmGetModelMatrix 都属于 Framework 层），因此本层只负责「数据」，
//     渲染、眨眼与动作播放由本项目自己实现。
//   - 顶点 / UV / 索引是**锯齿数组**：第 d 个部件拥有自己的顶点数组与索引数组，
//     索引值相对该部件自己的顶点空间，不同部件之间不能混用。
#pragma once

#include <string>
#include <vector>

namespace ppet::live2d::cubism {

/// 后端是否可用。
/// 返回 false 表示既没有编译 Cubism SDK，也没有编译桩实现。
bool Available();

/// Cubism Core 版本字符串（例如 "4.3.0"）。后端不可用时返回空串。
std::string Version();

/// 二维向量（对应 Cubism 的 csmVector2）。
struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

/// 画布信息（对应 csmReadCanvasInfo 的输出）。
///
/// Cubism 顶点坐标的单位是「模型单位」，需要结合 PixelsPerUnit 与画布中心
/// 换算到屏幕坐标。公式见官方 Core API Reference：
///   X = (localX / PixelsPerUnit) - (origin.X * size.X / PixelsPerUnit)
///   Y = (origin.Y * size.Y / PixelsPerUnit) - (localY / PixelsPerUnit)
struct CanvasInfo {
    Vec2 sizeInPixels{0.0f, 0.0f};  ///< 画布尺寸（像素）
    Vec2 originInPixels{0.0f, 0.0f}; ///< 画布中心（像素）
    float pixelsPerUnit = 1.0f;      ///< 单位像素数
};

/// 一个可绘制部件（Drawable）在一帧内的几何数据。
///
/// 指针指向适配层内部（真实 SDK 时为 Cubism Core 内部）的缓冲区，
/// 在下一次 UpdateModel() 之前有效；Live2DModel 每帧都会重新取用，因此不需要拷贝。
///
/// 注意：positions / uvs / indices 都是**按部件独立**的数组，
/// indices 中的值是相对于本部件顶点数组的下标。
struct Drawable {
    const float* positions = nullptr;        ///< 顶点坐标，x,y 交错
    const float* uvs = nullptr;              ///< 纹理坐标，u,v 交错
    const unsigned short* indices = nullptr; ///< 三角形索引
    int vertexCount = 0;                     ///< 顶点数
    int indexCount = 0;                      ///< 索引数
    int textureIndex = 0;                    ///< 使用的纹理序号（对应 model3.json 的 Textures 数组）
    float opacity = 1.0f;                    ///< 不透明度
    int renderOrder = 0;                     ///< 渲染顺序（越小越先绘制）
    bool doubleSided = false;                ///< 是否双面（双面时不做背面剔除）
    bool additiveBlend = false;              ///< 是否加法混合
    bool multiplicativeBlend = false;        ///< 是否乘法混合
    bool visible = true;                     ///< 是否可见
};

/// 模型句柄（不透明类型，实现细节全部藏在适配层里）。
class Model;

/// 创建模型实例。返回 nullptr 表示内存不足。
Model* CreateModel();

/// 销毁模型实例。
void DestroyModel(Model* model);

/// 打开 .moc3 文件。
/// @param model   模型实例
/// @param mocPath .moc3 文件路径
/// @param error   失败时写入中文原因
/// @return 成功返回 true。
bool OpenModel(Model* model, const std::string& mocPath, std::string& error);

/// 依据当前参数重算顶点（相当于 csmUpdateModel）。
void UpdateModel(Model* model);

/// 读取画布信息。
/// @return 成功返回 true；模型无效返回 false。
bool ReadCanvasInfo(const Model* model, CanvasInfo& out);

/// 参数数量。
int ParameterCount(const Model* model);

/// 第 index 个参数的名称。索引越界返回空串。
std::string ParameterId(const Model* model, int index);

/// 按名称查找参数索引。不存在返回 -1。
///
/// 这是视线追踪的关键：很多模型没有眼球参数或身体参数，
/// 找不到就必须跳过，而不是写一个不存在的参数。
int FindParameter(const Model* model, const std::string& id);

/// 读取参数当前值。
float ParameterValue(const Model* model, int index);

/// 写入参数值。
void SetParameterValue(Model* model, int index, float value);

/// 参数的取值范围（min/max）。
/// @param outMin 接收下界
/// @param outMax 接收上界
/// @return 参数存在且范围已知返回 true。
bool ParameterRange(const Model* model, int index, float& outMin, float& outMax);

/// 可绘制部件数量。
int DrawableCount(const Model* model);

/// 取第 index 个部件的几何数据。
/// @return 索引越界返回 false。
bool GetDrawable(const Model* model, int index, Drawable& out);

}  // namespace ppet::live2d::cubism
