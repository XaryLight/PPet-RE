// PPet - C++ 移植版
// 分区坐标映射单元测试：
//   - NormalizedFromRegion（区域名 -> 归一化坐标）与 RegionFromPoint
//     （归一化坐标 -> 区域名）互为反查，任意合法区域往返不变；
//   - 未知区域回退窗口中心；
//   - 分区边界：头在顶部、腿在底部、左右按水平位置划分。
//
// 纯逻辑、零依赖，三种 CMake 配置均可编译运行。
#include <src/tools/model/Model.hpp>

#include <cstdio>
#include <string>
#include <vector>

int main() {
    int failures = 0;
    auto check = [&](bool cond, const char* msg) {
        if (!cond) { std::printf("FAIL: %s\n", msg); ++failures; }
    };

    using ppet::NormalizedFromRegion;
    using ppet::RegionFromPoint;

    // --- 正反映射一致性：roundtrip(region) == region ---
    const std::vector<std::string> regions = {"head", "body", "leg", "left", "right"};
    for (const std::string& r : regions) {
        const ppet::Vec2 p = NormalizedFromRegion(r);
        check(RegionFromPoint(p.x, p.y) == r,
              ("roundtrip(" + r + ")").c_str());
    }

    // --- 未知区域回退中心 -> body ---
    const ppet::Vec2 unknown = NormalizedFromRegion("does-not-exist");
    check(unknown.x == 0.5f && unknown.y == 0.5f, "未知区域 -> 窗口中心");
    check(RegionFromPoint(unknown.x, unknown.y) == "body", "中心点 -> body");

    // --- 分区边界（y 优先：头/腿；否则按 x 分左右） ---
    check(RegionFromPoint(0.10f, 0.10f) == "head", "左上仍判为 head");
    check(RegionFromPoint(0.90f, 0.90f) == "leg", "右下仍判为 leg");
    check(RegionFromPoint(0.10f, 0.50f) == "left", "左侧中部 -> left");
    check(RegionFromPoint(0.90f, 0.50f) == "right", "右侧中部 -> right");
    check(RegionFromPoint(0.50f, 0.50f) == "body", "正中 -> body");
    check(RegionFromPoint(0.0f, 0.0f) == "head", "角点 (0,0) -> head");
    check(RegionFromPoint(1.0f, 1.0f) == "leg", "角点 (1,1) -> leg");

    if (failures == 0) {
        std::printf("region_unit: PASS (all assertions)\n");
        return 0;
    }
    std::printf("region_unit: %d FAILED\n", failures);
    return 1;
}
