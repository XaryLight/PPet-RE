// PPet - C++ 移植版
// 崩溃诊断单测：确认能区分 PPet 主程序与第三方 overlay 注入 DLL。
#include <src/tools/platform/CrashHandler.hpp>

#include <cstdio>
#include <string_view>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) ++g_failures;
}

}  // namespace

int main() {
    using ppet::crash::ClassifyOrigin;
    using ppet::crash::ExceptionName;
    using ppet::crash::Origin;
    std::printf("== 崩溃模块归类单测 ==\n");

    const char* exe = R"(C:\Apps\PPet\ppet.exe)";
    Check(ClassifyOrigin(R"(C:\Apps\PPet\ppet.exe)", exe) == Origin::kSelf,
          "同类：自身模块判为 kSelf");
    Check(ClassifyOrigin(R"(C:\APPS\PPET\PPET.EXE)", exe) == Origin::kSelf,
          "同类：Windows 路径与大小写不敏感");
    Check(ClassifyOrigin("ppet.exe", exe) == Origin::kSelf,
          "同类：只有文件名也能匹配");
    Check(ClassifyOrigin("C:/Apps/PPet/ppet.exe", exe) == Origin::kSelf,
          "同类：正斜杠路径");

    Check(ClassifyOrigin(R"(C:\Program Files\GamePP\GamePPOverlay.dll)", exe) ==
              Origin::kThirdParty,
          "第三方：GamePP overlay DLL 判为 kThirdParty");
    Check(ClassifyOrigin(R"(C:\Program Files\RivaTuner\RTSSHooks64.dll)", exe) ==
              Origin::kThirdParty,
          "第三方：RTSS 注入 DLL");
    Check(ClassifyOrigin(R"(C:\Windows\System32\nvd3dum.dll)", exe) ==
              Origin::kThirdParty,
          "第三方：显卡驱动 DLL 不算主程序");
    Check(ClassifyOrigin("ppet_helper.dll", exe) == Origin::kThirdParty,
          "第三方：同目录但不同文件名不误判");

    Check(ClassifyOrigin("", exe) == Origin::kUnknown,
          "未知：拿不到模块名时返回 kUnknown");
    Check(ClassifyOrigin(R"(C:\Apps\PPet\ppet.exe)", "") == Origin::kUnknown,
          "未知：主程序路径缺失时不误判");

    Check(ExceptionName(0xC0000005UL) == "EXCEPTION_ACCESS_VIOLATION",
          "异常码：0xC0000005 可读名称");
    Check(ExceptionName(0xC0000135UL) == "STATUS_DLL_NOT_FOUND",
          "异常码：0xC0000135 可读名称");
    Check(ExceptionName(0x12345678UL) == "UNKNOWN", "异常码：未知码回退 UNKNOWN");
    Check(ClassifyOrigin("a.dll", "b.exe") == Origin::kThirdParty,
          "第三方：文件名不同即为注入模块");

    std::printf("== 结果：%s（失败 %d 项）==\n", g_failures == 0 ? "全部通过" : "存在失败",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
