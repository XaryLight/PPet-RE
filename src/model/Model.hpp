// PPet - C++ 移植版
// 模型抽象接口：Live2D / PMX / VRM 三种后端统一由此驱动。
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/Math.hpp"

namespace ppet {

enum class ModelType {
    Unknown,
    Live2D,  ///< .model3.json / .moc3
    Pmx,     ///< .pmx
    Vrm,     ///< .vrm
};

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
};

using ModelPtr = std::shared_ptr<IModel>;

/// 创建对应类型的模型实例。未找到后端时返回 nullptr。
ModelPtr CreateModel(ModelType type);

}  // namespace ppet
