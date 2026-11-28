// PPet - C++ 移植版
// 分区触摸响应单测：验证 head/body/leg/left/right 的差异反馈映射，
// 以及与 Model.hpp::RegionFromPoint 的分区口径一致。
#include <src/tools/model/Model.hpp>
#include <src/tools/model/TouchResponse.hpp>

#include <cmath>
#include <cstdio>
#include <string>

namespace {

int failures = 0;
int checks = 0;

void Check(bool cond, const char* what) {
    ++checks;
    if (!cond) {
        ++failures;
        std::printf("  [FAIL] %s\n", what);
    }
}

using ppet::model::TouchResponse;
using ppet::model::ResponseForRegion;

bool HasExpr(const TouchResponse& r, const std::string& name) {
    for (const auto& kv : r.expression) {
        if (kv.first == name) return true;
    }
    return false;
}

float ExprWeight(const TouchResponse& r, const std::string& name) {
    for (const auto& kv : r.expression) {
        if (kv.first == name) return kv.second;
    }
    return 0.0f;
}

void TestRegionMapping() {
    std::printf("-- 分区 -> 差异化反馈 --\n");

    const TouchResponse head = ResponseForRegion("head");
    Check(std::string(head.motionId) == "nod", "摸头 -> 点头");
    Check(HasExpr(head, "happy"), "摸头 -> 开心表情");
    Check(head.influence > 0.0f && head.influence <= 1.0f, "摸头 influence 在 (0,1]");
    Check(head.holdSeconds > 0.0f, "摸头表情有保持时长");

    const TouchResponse left = ResponseForRegion("left");
    Check(std::string(left.motionId) == "waveLeft", "左侧 -> 左挥手");
    Check(HasExpr(left, "happy"), "左侧 -> 开心表情");

    const TouchResponse right = ResponseForRegion("right");
    Check(std::string(right.motionId) == "wave", "右侧 -> 右挥手");
    Check(HasExpr(right, "happy"), "右侧 -> 开心表情");

    const TouchResponse body = ResponseForRegion("body");
    Check(std::string(body.motionId) == "shake", "身体 -> 摇头");
    Check(HasExpr(body, "surprised"), "身体 -> 轻微惊讶");

    const TouchResponse leg = ResponseForRegion("leg");
    Check(std::string(leg.motionId) == "nod", "腿 -> 点头");
    Check(leg.expression.empty(), "腿 -> 不带表情偏移");
    Check(leg.influence == 0.0f, "腿 -> influence=0（不叠表情）");

    // 未知分区：安全降级为只点头，不崩溃。
    const TouchResponse unknown = ResponseForRegion("???");
    Check(std::string(unknown.motionId) == "nod", "未知分区 -> 点头兜底");
    Check(unknown.expression.empty(), "未知分区 -> 不叠表情");
}

void TestMotionIdsAreBuiltin() {
    std::printf("-- 动作 id 均为内置合法 id --\n");
    // 内置动作 id：nod / shake / wave / waveLeft（BoneMotion.hpp::Builtin）。
    const char* regions[] = {"head", "body", "leg", "left", "right", "???"};
    for (const char* region : regions) {
        const std::string id = ResponseForRegion(region).motionId;
        Check(id == "nod" || id == "shake" || id == "wave" || id == "waveLeft",
              "动作 id 属于内置集合");
    }
}

void TestWeightRanges() {
    std::printf("-- 表情权重/强度范围 --\n");
    const char* regions[] = {"head", "body", "leg", "left", "right"};
    for (const char* region : regions) {
        const TouchResponse r = ResponseForRegion(region);
        Check(r.influence >= 0.0f && r.influence <= 1.0f, "influence ∈ [0,1]");
        Check(r.holdSeconds >= 0.0f, "保持时长非负");
        for (const auto& kv : r.expression) {
            Check(kv.second >= 0.0f && kv.second <= 1.0f, "表情权重 ∈ [0,1]");
        }
    }
}

void TestRegionConsistency() {
    std::printf("-- 与 RegionFromPoint 分区口径一致 --\n");
    // NormalizedFromRegion(r) 再 RegionFromPoint 应得到 r（Model.hpp 契约）。
    const char* regions[] = {"head", "body", "leg", "left", "right"};
    for (const char* region : regions) {
        const ppet::Vec2 p = ppet::NormalizedFromRegion(region);
        Check(ppet::RegionFromPoint(p.x, p.y) == region, "分区反查一致");
        // 每个合法分区都有确定的反馈（动作 id 非空）。
        Check(ResponseForRegion(region).motionId[0] != '\0', "合法分区有动作反馈");
    }
}

}  // namespace

int main() {
    std::printf("=== 分区触摸响应单测 ===\n");
    TestRegionMapping();
    TestMotionIdsAreBuiltin();
    TestWeightRanges();
    TestRegionConsistency();
    std::printf("=== %d 项检查，失败 %d ===\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
