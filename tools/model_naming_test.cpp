// PPet - C++ 移植版
// 模型显示名单测。重点回归 Windows 上 `list` 因非法字节路径抛
// filesystem_error 的问题：显示名推导必须纯字节处理、绝不碰编码转换。
#include <src/tools/core/ModelNaming.hpp>

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) ++g_failures;
}

void CheckEq(const std::string& actual, const std::string& expected, const char* what) {
    if (actual != expected) {
        std::printf("[FAIL] %s（实际 \"%s\"，期望 \"%s\"）\n", what, actual.c_str(),
                    expected.c_str());
        ++g_failures;
    } else {
        std::printf("[ OK ] %s\n", what);
    }
}

}  // namespace

int main() {
    using namespace ppet::model_naming;
    std::printf("== 模型显示名单测 ==\n");

    // --- 默认名：文件名去后缀 ---
    CheckEq(FileStem("/home/user/models/测试模型.pmx"), "测试模型", "中文路径 + PMX 后缀");
    CheckEq(FileStem("C:\\Models\\nengdai_2.model3.json"), "nengdai_2",
            "Windows 路径 + 复合后缀 .model3.json");
    CheckEq(FileStem("https://cdn.example.com/a/b/model.vrm?token=xyz"), "model",
            "URL 查询参数被去掉");
    CheckEq(FileStem("model.PMX"), "model", "后缀大小写不敏感");
    CheckEq(FileStem("无后缀"), "无后缀", "无后缀保持原名");
    CheckEq(FileStem("/a/b/.hidden"), ".hidden", "点开头不算后缀");
    CheckEq(FileStem("dir/"), "dir", "尾部斜杠被去掉");

    // --- 关键回归：非法 UTF-8 / 任意字节不得抛异常 ---
    {
        std::string bad = "/tmp/";
        bad.push_back(static_cast<char>(0xFF));
        bad.push_back(static_cast<char>(0xFE));
        bad += "/model.pmx";
        bool threw = false;
        std::string result;
        try {
            result = FileStem(bad);
        } catch (...) {
            threw = true;
        }
        Check(!threw, "非法字节路径：不抛异常（Windows __char_to_wide 回归）");
        CheckEq(result, "model", "非法字节路径：仍能取出文件名");

        // 整个列表推导也不得抛异常。
        const std::vector<std::string> paths = {bad, "/ok/中文.vrm", bad};
        bool listThrew = false;
        std::vector<std::string> names;
        try {
            names = ResolveNames(paths, {});
        } catch (...) {
            listThrew = true;
        }
        Check(!listThrew, "含非法字节的模型列表：ResolveNames 不抛异常");
        Check(names.size() == 3, "含非法字节的模型列表：名字数量对齐");
        CheckEq(names[0], "model", "非法字节项默认名");
        CheckEq(names[2], "model (2)", "重名自动标注序号");
    }

    // --- 重名序号 ---
    CheckEq(UniqueName("abc", {}), "abc", "无重名时保持原名");
    CheckEq(UniqueName("abc", {"abc"}), "abc (2)", "第一次重名 -> (2)");
    CheckEq(UniqueName("abc", {"abc", "abc (2)", "abc (3)"}), "abc (4)",
            "连续重名找到下一个空位");

    // --- 列表对齐 ---
    {
        const std::vector<std::string> paths = {"a/one.pmx", "b/two.vrm", "c/three.zip"};
        const auto names = ResolveNames(paths, {});
        CheckEq(names[0], "one", "补齐：第 1 项默认名");
        CheckEq(names[2], "three", "补齐：第 3 项默认名");

        const auto renamed = ResolveNames(paths, {"自定义", "", "自定义"});
        CheckEq(renamed[0], "自定义", "沿用用户改名");
        CheckEq(renamed[1], "two", "空名字回退到文件名");
        CheckEq(renamed[2], "自定义 (2)", "用户改名重复也自动加序号");

        CheckEq(NameAt(paths, renamed, 99), "模型 100", "越界下标兜底");
    }

    std::printf("== 结果：%s（失败 %d 项）==\n", g_failures == 0 ? "全部通过" : "存在失败",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
