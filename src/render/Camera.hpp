// PPet - C++ 移植版
// 相机：视图 / 投影矩阵的构造与自动取景。
//
// 坐标约定（与整个渲染管线一致）：
//   - 右手系，Y 轴向上；
//   - 相机看向自己的 -Z 方向（OpenGL 约定）；
//   - 模型正面朝 +Z，因此相机放在模型 +Z 一侧向 -Z 方向看过去，
//     这样屏幕上看到的才是模型的正面。
#pragma once

#include "core/Math.hpp"
#include "model/SkinnedMesh.hpp"

namespace ppet {

/// 相机。view / projection / viewProjection 三者同步维护。
struct Camera {
    Mat4 view;            ///< 视图矩阵（世界 -> 相机空间）
    Mat4 projection;      ///< 投影矩阵（相机空间 -> 裁剪空间）
    Mat4 viewProjection;  ///< projection * view，绘制时直接传给渲染层
    Vec3 position;        ///< 相机世界坐标
    Vec3 target;          ///< 相机看向的世界坐标

    /// 按模型包围盒自动取景，构造正交相机。
    /// 桌宠是 2D 平面角色，正交投影不会产生近大远小的透视畸变，
    /// 也不会因为模型转向而改变体型，因此是这类场景的默认选择。
    /// @param mesh    模型（取其包围盒）
    /// @param aspect  视口宽高比（width / height）
    /// @param padding 四周留白比例（1.1 表示上下左右各留 10%）
    static Camera FitOrthographic(const SkinnedMesh& mesh, float aspect, float padding = 1.1f);

    /// 按模型包围盒自动取景，构造透视相机。
    /// @param fovY 垂直视场角，单位「度」（30 度是常用默认值）
    static Camera FitPerspective(const SkinnedMesh& mesh, float aspect, float fovY = 30.0f);

    /// 重新设置观察点，并同步刷新 view 与 viewProjection。
    /// projection 需事先设置好（Fit* 已自动完成）。
    void LookAt(const Vec3& eye, const Vec3& target, const Vec3& up = Vec3(0.0f, 1.0f, 0.0f));
};

}  // namespace ppet
