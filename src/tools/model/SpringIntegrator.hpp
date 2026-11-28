// PPet - VRM 弹簧骨积分器（纯逻辑，零依赖，可单测）。
//
// 按 VRM 规范（VRMC_springBone-1.0, non-normative 参考实现）的 verlet 积分：
//   inertia   = (current - previous) * (1 - dragForce)
//   stiffness = dt * restDirection * stiffnessForce
//   external  = dt * gravityDir * gravityPower
//   next      = current + inertia + stiffness + external
// 再做骨长约束与碰撞球推出。
//
// 与旧实现（内联在 VrmModel::UpdateSpringBones）的关键差异：
//   * 惯性乘 (1 - dragForce)，不是 dragForce；
//   * 加入 stiffness 回正力，把骨骼拉回静止朝向——这是「自然摆动」的核心；
//   * 重力乘 dt（老代码误用 dt²）。
#pragma once

#include <src/tools/core/Math.hpp>

#include <algorithm>
#include <cmath>

namespace ppet {

struct SpringIntegratorParams {
    float stiffness = 1.0f;   ///< 回正强度（>=0）
    float dragForce = 0.5f;   ///< 阻尼，[0,1]，越大惯性衰减越快
    float gravityPower = 0.0f;
    float hitRadius = 0.0f;   ///< 关节碰撞半径
    Vec3 gravityDir{0.0f, -1.0f, 0.0f};
    Vec3 restDirection{0.0f, 1.0f, 0.0f};  ///< 静止朝向（单位向量）
    Vec3 anchor{0.0f, 0.0f, 0.0f};         ///< 父关节世界坐标
    float restLength = 0.0f;               ///< 骨长
};

/// 推进一帧 verlet 积分，返回新的骨骼末端世界坐标。
/// current/previous 均为世界坐标；dt 为帧间隔（秒）。
inline Vec3 SpringIntegrate(const Vec3& current, const Vec3& previous,
                            const SpringIntegratorParams& p, float dt) {
    if (!(dt > 0.0f)) return current;

    const float drag = std::clamp(p.dragForce, 0.0f, 1.0f);
    const Vec3 inertia = (current - previous) * (1.0f - drag);
    const Vec3 stiffnessForce = p.restDirection * (std::max(0.0f, p.stiffness) * dt);
    const Vec3 external = p.gravityDir * (p.gravityPower * dt);

    Vec3 next = current + inertia + stiffnessForce + external;

    // 骨长约束：末端到锚点距离保持 restLength（只约束上限，避免被拉长）。
    if (p.restLength > 1e-6f) {
        const Vec3 toAnchor = next - p.anchor;
        const float length = Length(toAnchor);
        if (length > p.restLength && length > 1e-6f) {
            next = p.anchor + toAnchor * (p.restLength / length);
        }
    }

    // 数值防御：任何一步产生非有限值都退回上一帧位置，避免模型「炸开」。
    // 注意 current 自身可能就是非有限的（上游污染），此时不能退回它，
    // 否则会把 NaN 继续传给蒙皮矩阵导致整只模型消失；改为返回零向量。
    if (!std::isfinite(next.x) || !std::isfinite(next.y) || !std::isfinite(next.z)) {
        if (std::isfinite(previous.x) && std::isfinite(previous.y) && std::isfinite(previous.z)) {
            return previous;
        }
        return Vec3(0.0f, 0.0f, 0.0f);
    }
    return next;
}

}  // namespace ppet
