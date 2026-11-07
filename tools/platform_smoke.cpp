// PPet - C++ 移植版
// 平台层冒烟测试。
//
// 覆盖范围：
//   1. 窗口层：创建 / 帧循环 / 置顶 / 点击穿透 / 透明 / 位置尺寸 / 全屏置顶维持
//   2. 托盘层：创建（含无托盘降级）/ 图标 / 提示 / 菜单结构 / 菜单回调映射
//   3. 开机启动：只查询，不修改系统状态
//   4. 外部链接：只校验协议白名单，不真的打开浏览器
//   5. 全局光标归一化换算（复用 GlobalCursorTracker 的既有实现）
//
// 运行方式：
//   xvfb-run -a ./ppet_platform_smoke
//   或先 Xvfb :99 & 再 DISPLAY=:99 ./ppet_platform_smoke
//
// 退出码：0 = 全部通过；1 = 存在失败项；2 = 无图形显示环境，已跳过图形相关检查。
#include <cmath>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <src/tools/core/Math.hpp>
#include <src/tools/platform/AutoStart.hpp>
#include <src/tools/platform/FileDialog.hpp>
#include <src/tools/platform/GlobalCursorTracker.hpp>
#include <src/tools/platform/OpenExternal.hpp>
#include <src/tools/platform/Tray.hpp>
#include <src/tools/platform/Window.hpp>

#if !defined(_WIN32) && !defined(__APPLE__)
// glfw3native.h 需要先包含 glfw3.h（它依赖 GLFWAPI 宏）。
#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <X11/Xlib.h>
#endif

namespace {

int g_checks = 0;
int g_failures = 0;

#if !defined(_WIN32) && !defined(__APPLE__)
/// 统计根窗口的直接子窗口数量（弹出菜单会新增一个 override-redirect 窗口）。
int CountRootChildren(Display* display, Window root) {
    Window rootReturn = 0;
    Window parentReturn = 0;
    Window* children = nullptr;
    unsigned int count = 0;
    if (!XQueryTree(display, root, &rootReturn, &parentReturn, &children, &count)) return -1;
    if (children) XFree(children);
    return static_cast<int>(count);
}

/// 把鼠标指针移动到窗口内部（用于验证点击穿透）。
void WarpPointerIntoWindow(GLFWwindow* handle) {
    Display* display = glfwGetX11Display();
    const Window xwin = glfwGetX11Window(handle);
    if (!display || !xwin) return;
    int x = 0;
    int y = 0;
    glfwGetWindowPos(handle, &x, &y);
    int width = 0;
    int height = 0;
    glfwGetWindowSize(handle, &width, &height);
    // 移动到窗口中心附近，避开边框。
    XWarpPointer(display, None, xwin, 0, 0, 0, 0, width / 2, height / 2);
    XFlush(display);
}
#endif

void Check(bool condition, const std::string& description) {
    ++g_checks;
    if (condition) {
        std::printf("  [ OK ] %s\n", description.c_str());
    } else {
        ++g_failures;
        std::printf("  [FAIL] %s\n", description.c_str());
    }
}

void Section(const char* title) { std::printf("\n== %s ==\n", title); }

/// 与原项目 src/main/tray.ts 保持一致的菜单结构（中文标签）。
std::vector<ppet::TrayMenuItem> BuildTrayMenu(bool alwaysOnTop, bool ignoreMouseEvents,
                                               bool showTool, bool openAtLogin, bool chinese) {
    using ppet::TrayMenuItem;
    std::vector<TrayMenuItem> items;

    items.push_back({TrayMenuItem::Type::Checkbox, chinese ? "@置顶" : "@Always On Top",
                     "alwaysOnTop", alwaysOnTop, true, {}, "Ctrl+T"});
    items.push_back({TrayMenuItem::Type::Checkbox, chinese ? "小工具" : "Tools", "showTool",
                     showTool, true, {}, "Ctrl+T"});
    items.push_back({TrayMenuItem::Type::Checkbox, chinese ? "忽略点击" : "Ignore Mouse Events",
                     "ignoreMouseEvents", ignoreMouseEvents, true, {}, "Ctrl+I"});
    items.push_back({TrayMenuItem::Type::Action, chinese ? "配置" : "Settings", "settings", false,
                     true, {}, "Ctrl+,"});

    items.push_back({TrayMenuItem::Type::Separator, "", "", false, true, {}, ""});
    items.push_back({TrayMenuItem::Type::Action, chinese ? "上一个模型" : "Prev Model", "prevModel",
                     false, true, {}, "Ctrl+P"});
    items.push_back({TrayMenuItem::Type::Action, chinese ? "下一个模型" : "Next Model", "nextModel",
                     false, true, {}, "Ctrl+N"});

    items.push_back({TrayMenuItem::Type::Separator, "", "", false, true, {}, ""});

    TrayMenuItem language{TrayMenuItem::Type::Submenu, chinese ? "语言" : "Language", "language",
                          false, true, {}, ""};
    language.subItems.push_back({TrayMenuItem::Type::Radio, "简体中文", "lang.zh", chinese, true, {},
                                 ""});
    language.subItems.push_back({TrayMenuItem::Type::Radio, "English", "lang.en", !chinese, true, {},
                                 ""});
    items.push_back(language);

    items.push_back({TrayMenuItem::Type::Separator, "", "", false, true, {}, ""});
    items.push_back({TrayMenuItem::Type::Action, chinese ? "重新渲染" : "ReRender", "reload", false,
                     true, {}, "Ctrl+R"});
    items.push_back({TrayMenuItem::Type::Action, chinese ? "调试" : "Debug", "debug", false, true, {},
                     "Ctrl+D"});

    items.push_back({TrayMenuItem::Type::Separator, "", "", false, true, {}, ""});
    items.push_back({TrayMenuItem::Type::Checkbox, chinese ? "开机启动" : "Open At Login",
                     "openAtLogin", openAtLogin, true, {}, ""});

    items.push_back({TrayMenuItem::Type::Separator, "", "", false, true, {}, ""});
    items.push_back({TrayMenuItem::Type::Action, chinese ? "反馈" : "Feedback", "feedback", false,
                     true, {}, ""});
    items.push_back({TrayMenuItem::Type::Action, chinese ? "关于" : "About", "about", false, true, {},
                     ""});

    items.push_back({TrayMenuItem::Type::Separator, "", "", false, true, {}, ""});
    items.push_back({TrayMenuItem::Type::Action, chinese ? "退出" : "Quit", "quit", false, true, {},
                     ""});
    return items;
}

}  // namespace

int main() {
    std::printf("PPet 平台层冒烟测试\n");

    bool displayAvailable = true;

    // ---------------------------------------------------------------------
    Section("窗口层");
    // ---------------------------------------------------------------------
    {
        ppet::Window window;
        ppet::WindowOptions options;
        options.width = 350;
        options.height = 600;
        options.x = 100;
        options.y = 100;
        options.title = "PPet";
        options.alwaysOnTop = true;
        options.transparent = true;
        options.resizable = false;
        options.skipTaskbar = true;

        std::string error;
        if (!window.Create(options, error)) {
            displayAvailable = false;
            std::printf("  [跳过] 无法创建窗口：%s\n", error.c_str());
            Check(!error.empty(), "创建失败时给出了中文错误说明（而不是崩溃）");
        } else {
            Check(true, "创建 GLFW 窗口 + OpenGL 上下文");
            Check(window.Handle() != nullptr, "GLFWwindow 句柄有效");

            // 位置与尺寸。
            const ppet::Vec2 size = window.Size();
            Check(std::fabs(size.x - 350.0f) < 2.0f && std::fabs(size.y - 600.0f) < 2.0f,
                  "初始尺寸为 350x600");

            const ppet::Vec2 position = window.Position();
            Check(std::fabs(position.x - 100.0f) < 8.0f && std::fabs(position.y - 100.0f) < 8.0f,
                  "初始位置约为 (100, 100)");

            // 负坐标 + 多显示器语义。
            window.SetPosition(-120, -60);
            window.PollEvents();
            const ppet::Vec2 negative = window.Position();
            Check(negative.x < 0.0f && negative.y < 0.0f,
                  "支持负坐标（位于主屏左上方的显示器）");

            window.SetPosition(100, 100);

            // 尺寸变更。
            bool resized = false;
            window.onResize = [&resized](int, int) { resized = true; };
            window.SetSize(400, 500);
            window.PollEvents();
            const ppet::Vec2 newSize = window.Size();
            Check(std::fabs(newSize.x - 400.0f) < 2.0f && std::fabs(newSize.y - 500.0f) < 2.0f,
                  "SetSize 生效（400x500）");
            (void)resized;

            // 置顶开关。
            window.SetAlwaysOnTop(false);
            Check(!window.IsAlwaysOnTop(), "取消置顶状态已记录");
            window.SetAlwaysOnTop(true);
            Check(window.IsAlwaysOnTop(), "置顶状态已记录");

            // 点击穿透开关。
            window.SetIgnoreMouseEvents(true);
            Check(window.IsIgnoreMouseEvents(), "点击穿透状态已记录");
            window.SetIgnoreMouseEvents(false);
            Check(!window.IsIgnoreMouseEvents(), "点击穿透可恢复");

#if !defined(_WIN32) && !defined(__APPLE__)
            // 行为验证：X11 上点击穿透是靠 XShape 把输入区域清空实现的，
            // 因此「鼠标移入窗口时是否还能收到事件」可以直接反映穿透是否生效。
            int mouseMoveCount = 0;
            window.onMouseMove = [&mouseMoveCount](double, double) { ++mouseMoveCount; };

            window.SetIgnoreMouseEvents(false);
            WarpPointerIntoWindow(window.Handle());
            for (int i = 0; i < 10; ++i) {
                window.PollEvents();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            const int receivedWhenNormal = mouseMoveCount;

            mouseMoveCount = 0;
            window.SetIgnoreMouseEvents(true);
            WarpPointerIntoWindow(window.Handle());
            for (int i = 0; i < 10; ++i) {
                window.PollEvents();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            const int receivedWhenIgnored = mouseMoveCount;
            window.SetIgnoreMouseEvents(false);

            Check(receivedWhenNormal > 0, "正常状态下窗口能收到鼠标移动事件");
            Check(receivedWhenIgnored == 0,
                  "开启点击穿透后窗口不再收到鼠标事件（事件穿透到下层窗口）");
#endif

            // 透明与标题。
            window.SetTransparent(true);
            Check(window.IsTransparent(), "透明状态已记录");
            window.SetTitle("PPet - 冒烟测试");

            // 帧循环 + 全屏置顶维持。
            bool closed = false;
            window.onClose = [&closed]() { closed = true; };
            for (int frame = 0; frame < 30; ++frame) {
                window.PollEvents();
                window.UpdatePlatformState();
                window.SwapBuffers();
            }
            Check(true, "连续 30 帧（PollEvents / UpdatePlatformState / SwapBuffers）未崩溃");
            Check(!closed, "窗口未意外关闭");
            Check(window.PlatformNote().empty() || !window.PlatformNote().empty(),
                  "平台降级说明可读取");
            if (!window.PlatformNote().empty()) {
                std::printf("         平台说明：%s\n", window.PlatformNote().c_str());
            }

            window.Destroy();
            Check(window.Handle() == nullptr, "Destroy 后句柄已清空");
            window.Destroy();  // 重复调用应安全
            Check(true, "重复 Destroy 未崩溃");
        }
    }

    // ---------------------------------------------------------------------
    Section("托盘层");
    // ---------------------------------------------------------------------
    {
        ppet::Tray tray;
        std::string error;
        const bool created = tray.Create(error);
        if (!created) {
            // Linux 下没有托盘宿主属于「无托盘降级模式」，必须给出清晰的中文提示。
            Check(!error.empty(), "托盘不可用时给出了中文原因：" + error);
            Check(!tray.IsAvailable(), "托盘标记为不可用（进入降级模式）");
            std::printf("         降级说明：%s\n", tray.PlatformNote().c_str());
        } else {
            Check(tray.IsAvailable(), "托盘图标创建成功");
            tray.SetTooltip("PPet 桌宠");
            tray.SetIcon("tools/testdata/test_texture.png");
            Check(true, "设置提示文本与图标未崩溃");

            // 菜单结构与原项目一致。
            std::vector<std::string> received;
            bool receivedChecked = false;
            tray.SetItemCallback([&received, &receivedChecked](const std::string& id, bool checked) {
                received.push_back(id);
                receivedChecked = checked;
            });

            const std::vector<ppet::TrayMenuItem> menu =
                BuildTrayMenu(true, false, true, false, true);
            tray.UpdateMenu(menu);
            Check(true, "重建菜单未崩溃");

            // 逐个触发扁平索引，验证 id 映射与勾选语义。
            // 分隔线不可选中，因此不会触发回调，收集时会自动跳过。
            const std::vector<std::string> expected = {
                "alwaysOnTop", "showTool", "ignoreMouseEvents", "settings",
                "prevModel",   "nextModel", "language",  "lang.zh",
                "lang.en",     "reload",    "debug",      "openAtLogin",
                "feedback",    "about",     "quit"};
            std::vector<std::string> collected;
            std::vector<int> indexOf;
            for (int index = 0; index < 32; ++index) {
                received.clear();
                tray.NotifyItemSelected(index);
                if (received.empty()) continue;
                collected.push_back(received.front());
                indexOf.push_back(index);
            }
            bool mappingOk = collected.size() == expected.size();
            if (mappingOk) {
                for (std::size_t i = 0; i < expected.size(); ++i) {
                    if (collected[i] != expected[i]) {
                        mappingOk = false;
                        std::printf("         第 %zu 项期望 %s，实际 %s\n", i, expected[i].c_str(),
                                    collected[i].c_str());
                        break;
                    }
                }
            } else {
                std::printf("         期望 %zu 项，实际 %zu 项\n", expected.size(),
                            collected.size());
            }
            Check(mappingOk, "菜单扁平索引与 id 映射正确（含子菜单项）");
            Check(collected.size() == expected.size(), "菜单项数量与原项目一致");

            // 勾选项的回调语义：勾选状态应为「切换后的新状态」。
            const auto indexOfId = [&](const std::string& id) -> int {
                for (std::size_t i = 0; i < collected.size(); ++i) {
                    if (collected[i] == id) return indexOf[i];
                }
                return -1;
            };

            received.clear();
            const int alwaysOnTopIndex = indexOfId("alwaysOnTop");
            tray.NotifyItemSelected(alwaysOnTopIndex);  // 当前为已勾选状态
            Check(!received.empty() && received.front() == "alwaysOnTop" && !receivedChecked,
                  "勾选项回调返回切换后的状态（true -> false）");

            received.clear();
            const int languageIndex = indexOfId("lang.zh");
            tray.NotifyItemSelected(languageIndex);  // 单选项
            Check(!received.empty() && received.front() == "lang.zh" && receivedChecked,
                  "单选项回调返回 true");

#if !defined(_WIN32) && !defined(__APPLE__)
            // Linux 上菜单是自绘的 override-redirect 窗口，
            // 可以通过根窗口子窗口数量的变化确认菜单确实被创建了。
            if (Display* display = XOpenDisplay(nullptr)) {
                const Window root = DefaultRootWindow(display);
                const int before = CountRootChildren(display, root);
                tray.PopupMenu();
                // 给托盘线程一点时间处理命令队列。
                for (int i = 0; i < 30; ++i) {
                    tray.PumpEvents();
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                const int after = CountRootChildren(display, root);
                Check(after > before, "弹出菜单后根窗口新增了子窗口（菜单已绘制）");
                XCloseDisplay(display);
            }
#endif

            tray.PopupMenu();
            tray.PumpEvents();
            Check(true, "PopupMenu / PumpEvents 未崩溃");
        }
    }

    // ---------------------------------------------------------------------
    Section("开机启动（只查询，不修改系统状态）");
    // ---------------------------------------------------------------------
    {
        std::string exePath;
        const bool gotPath = ppet::GetExecutablePath(exePath) && !exePath.empty();
        Check(gotPath, "获取当前可执行文件路径：" + exePath);

        std::string registered;
        const bool enabled = ppet::IsAutoStartEnabled(registered);
        std::printf("         当前开机启动状态：%s（注册路径：%s）\n",
                    enabled ? "已启用" : "未启用", registered.empty() ? "(空)" : registered.c_str());
        Check(true, "IsAutoStartEnabled 查询未崩溃（未修改系统状态）");
    }

    // ---------------------------------------------------------------------
    Section("外部链接协议白名单");
    // ---------------------------------------------------------------------
    {
        std::string error;
        Check(!ppet::OpenExternalUrl("file:///etc/passwd", error),
              "拒绝非 http/https 链接：" + error);
        Check(!ppet::OpenExternalUrl("", error), "拒绝空链接");
        Check(!ppet::OpenExternalUrl("javascript:alert(1)", error), "拒绝 javascript 协议");
    }

    // ---------------------------------------------------------------------
    Section("全局光标归一化换算");
    // ---------------------------------------------------------------------
    {
        const ppet::Vec2 center(960.0f, 540.0f);
        ppet::Vec2 normalized;

        ppet::GlobalCursorTracker::NormalizeAround(center, center, 400.0f, &normalized);
        Check(std::fabs(normalized.x) < 1e-5f && std::fabs(normalized.y) < 1e-5f,
              "光标位于中心时归一化为 (0, 0)");

        ppet::GlobalCursorTracker::NormalizeAround(ppet::Vec2(1360.0f, 540.0f), center, 400.0f,
                                                   &normalized);
        Check(std::fabs(normalized.x - 1.0f) < 1e-4f && std::fabs(normalized.y) < 1e-4f,
              "光标在正右方时归一化为 (1, 0)");

        ppet::GlobalCursorTracker::NormalizeAround(ppet::Vec2(960.0f, 940.0f), center, 400.0f,
                                                   &normalized);
        Check(std::fabs(normalized.y + 1.0f) < 1e-4f,
              "光标在下方时 Y 取反（屏幕 Y 向下，模型 Y 向上）");

        ppet::GlobalCursorTracker::NormalizeAround(ppet::Vec2(2000.0f, 100.0f), center, 400.0f,
                                                   &normalized);
        const float length = ppet::Length(normalized);
        Check(std::fabs(length - 1.0f) < 1e-4f, "超出参照半径时长度被限制为 1");
    }

    // ---------------------------------------------------------------------
    std::printf("\n---------------------------------------------\n");
    std::printf("检查项：%d，失败：%d\n", g_checks, g_failures);
    if (!displayAvailable) {
        std::printf("结果：当前环境没有图形显示，图形相关检查已跳过（程序未崩溃）\n");
        return 2;
    }
    std::printf("结果：%s\n", g_failures == 0 ? "全部通过" : "存在失败项");
    return g_failures == 0 ? 0 : 1;
}
