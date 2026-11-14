// PPet - 控制台 action <idx> 索引解析单元测试
//
// 验证 ppet::console::ResolveMotionTarget：既支持按名字，也支持按数字索引
// 调试动画；索引与 `action list` 展示的 Idx 列一一对应。

#include <src/tools/app/ConsoleCommand.hpp>

#include <cstdio>
#include <string>
#include <vector>

namespace {
int g_fail = 0;
int g_pass = 0;

void Check(bool cond, const char* expr, const std::string& detail) {
    if (cond) {
        ++g_pass;
    } else {
        ++g_fail;
        std::printf("  [FAIL] %s  ->  %s\n", expr, detail.c_str());
    }
}
}  // namespace

int main() {
    using ppet::console::ResolveMotionTarget;
    const std::vector<std::string> names = {"idle", "idle(2)", "wave", "motion#3"};

    // 1) 索引 0 -> 第一个动作名
    {
        auto r = ResolveMotionTarget("0", names);
        Check(r.second && r.first == "idle", "idx 0 命中", r.first);
        Check(r.second && r.first != "0", "idx 0 已解析为名字", r.first);
    }
    // 2) 索引 2 -> wave
    {
        auto r = ResolveMotionTarget("2", names);
        Check(r.second && r.first == "wave", "idx 2 命中 wave", r.first);
    }
    // 3) 越界索引 -> ok=false（调用方应提示范围）
    {
        auto r = ResolveMotionTarget("9", names);
        Check(!r.second, "idx 9 越界返回 false", "应返回 false");
    }
    // 4) 负数含 '-' -> 不是纯数字，原样返回交给 PlayMotion 处理
    {
        auto r = ResolveMotionTarget("-1", names);
        Check(r.second && r.first == "-1", "-1 原样返回", r.first);
    }
    // 5) 普通名字 -> 原样返回
    {
        auto r = ResolveMotionTarget("idle", names);
        Check(r.second && r.first == "idle", "名字 idle 原样", r.first);
    }
    // 6) 含 '#' 的非纯数字 -> 原样返回
    {
        auto r = ResolveMotionTarget("motion#3", names);
        Check(r.second && r.first == "motion#3", "motion#3 原样", r.first);
    }
    // 7) 空 token -> 原样空
    {
        auto r = ResolveMotionTarget("", names);
        Check(r.second && r.first.empty(), "空 token 原样空", "应返回空且 ok=true");
    }
    // 8) 前导零 "02" -> 命中 idx 2
    {
        auto r = ResolveMotionTarget("02", names);
        Check(r.second && r.first == "wave", "idx 02 命中 wave", r.first);
    }
    // 9) 空列表下任意索引都越界
    {
        const std::vector<std::string> empty;
        auto r = ResolveMotionTarget("0", empty);
        Check(!r.second, "空列表 idx 0 越界", "应返回 false");
    }

    std::printf("[console_motion_idx] %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
