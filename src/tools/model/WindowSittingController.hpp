// PPet - C++ 移植版
// 窗口坐姿控制器（缺陷 4）：桌面空间感知的「决策」部分。
//
// 【为什么是纯逻辑】
//   平台几何获取（WindowGeometry）与「是否坐 / 何时坐 / 坐哪」的判定分离：
//   本模块只消费调用方注入的窗口矩形与光标位置，输出坐姿决策与锚点，不碰
//   任何平台 API，因此可脱离后端单测。App 层负责每帧取 ForegroundWindow 并
//   桥接进来。
//
// 【语义】
//   用户把光标移到「前台窗口上边缘」附近并停留片刻 → 桌宠进入 Sitting，
//   坐到该窗口上边缘；窗口消失（关闭 / 最小化）立即站起，光标离开过久则
//   防抖后站起。坐姿锚点跟随窗口移动。
#pragma once

#include <src/tools/core/Math.hpp>

#include <algorithm>
#include <cmath>

namespace ppet::model {

/// 坐姿目标窗口的矩形（字段与 platform::WindowRect 一致，便于 App 桥接）。
/// 独立定义在此，避免纯逻辑模块反向依赖平台层。
struct SitTarget {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    bool valid = false;

    float TopY() const { return y; }
    float CenterX() const { return x + width * 0.5f; }
    bool Contains(float cx, float cy) const {
        return valid && cx >= x && cx <= x + width && cy >= y && cy <= y + height;
    }
    bool Empty() const { return !valid || width <= 0.0f || height <= 0.0f; }
};

/// 窗口坐姿控制器。线程不安全，仅在渲染主线程调用。
class WindowSittingController {
public:
    struct Config {
        /// 光标距窗口上边缘多少像素内视为「靠近」。
        float proximityPx = 48.0f;
        /// 连续靠近多久才坐（防抖，避免鼠标路过就触发）。
        float settleSeconds = 0.45f;
        /// 光标离开（非窗口丢失）多久才站起。
        float leaveSeconds = 0.70f;
    };

    WindowSittingController() : config_(Config{}) {}
    explicit WindowSittingController(Config config) : config_(config) {}

    void SetConfig(const Config& config) { config_ = config; }
    const Config& GetConfig() const { return config_; }

    /// 每帧调用：注入目标窗口矩形与光标位置，推进防抖并更新坐姿状态。
    void Update(const SitTarget& target, const Vec2& cursor, float dt) {
        justSat_ = false;
        justStood_ = false;
        if (!(dt > 0.0f)) return;

        const bool near = !target.Empty() && NearTopEdge(target, cursor);

        if (sitting_) {
            // 坐姿目标丢失（窗口关闭 / 最小化）：立即站起，触发「下落 / 站起」过渡。
            if (target.Empty()) {
                Stand();
                return;
            }
            // 坐姿锚点跟随窗口移动（窗口被拖动时角色跟着走）。
            anchor_ = target;
            if (near) {
                leaveTimer_ = 0.0f;
            } else {
                leaveTimer_ += dt;
                if (leaveTimer_ >= config_.leaveSeconds) Stand();
            }
        } else {
            if (near) {
                settleTimer_ += dt;
                if (settleTimer_ >= config_.settleSeconds) Sit(target);
            } else {
                settleTimer_ = 0.0f;
            }
        }
    }

    bool IsSitting() const { return sitting_; }
    /// 本帧是否刚刚坐下（上升沿，供 App 驱动状态机 Sit 事件）。
    bool JustSat() const { return justSat_; }
    /// 本帧是否刚刚站起（下降沿，供 App 驱动状态机 Stand 事件）。
    bool JustStood() const { return justStood_; }

    /// 当前坐姿锚点（目标窗口上边缘，屏幕坐标）。未坐时返回 invalid。
    const SitTarget& Anchor() const { return anchor_; }

    /// 坐姿锚点的屏幕 X（窗口上边缘中点）。未坐时返回 0。
    float AnchorCenterX() const { return anchor_.CenterX(); }
    /// 坐姿锚点的屏幕 Y（窗口上边缘）。未坐时返回 0。
    float AnchorTopY() const { return anchor_.TopY(); }

    /// 重置（模型切换 / 停止坐姿时调用）。
    void Reset() {
        sitting_ = false;
        justSat_ = false;
        justStood_ = false;
        settleTimer_ = 0.0f;
        leaveTimer_ = 0.0f;
        anchor_ = SitTarget{};
    }

private:
    /// 光标是否「靠近窗口上边缘」：水平方向落在窗口范围（左右各外扩 proximity），
    /// 垂直方向距上边缘不超过 proximity。
    bool NearTopEdge(const SitTarget& w, const Vec2& c) const {
        const float p = std::max(0.0f, config_.proximityPx);
        return c.x >= w.x - p && c.x <= w.x + w.width + p &&
               std::fabs(c.y - w.TopY()) <= p;
    }

    void Sit(const SitTarget& target) {
        sitting_ = true;
        justSat_ = true;
        anchor_ = target;
        settleTimer_ = 0.0f;
        leaveTimer_ = 0.0f;
    }

    void Stand() {
        sitting_ = false;
        justStood_ = true;
        anchor_ = SitTarget{};
        settleTimer_ = 0.0f;
        leaveTimer_ = 0.0f;
    }

    Config config_;
    bool sitting_ = false;
    bool justSat_ = false;
    bool justStood_ = false;
    float settleTimer_ = 0.0f;
    float leaveTimer_ = 0.0f;
    SitTarget anchor_;
};

}  // namespace ppet::model
