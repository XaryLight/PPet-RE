// PPet - C++ 移植版
// 动态插件加载器实现（报告 2.3.3）。
#include <src/tools/plugin/DynamicPluginLoader.hpp>

#include <src/tools/plugin/PluginABI.hpp>

#include <cstring>
#include <filesystem>
#include <memory>
#include <set>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace ppet::plugin {
namespace {

namespace fs = std::filesystem;

/// 平台库句柄与符号解析。统一封装，业务代码不碰 dlsym/GetProcAddress。
struct Library {
    void* handle = nullptr;

    static Library Open(const std::string& path) {
#if defined(_WIN32)
        // LoadLibraryA 接受窄字符路径；中文路径用 LoadLibraryW 更稳，但这里
        // 先走 A 版本，调用方传入的路径在 UTF-8 环境多为 ASCII（plugins/ 目录）。
        HMODULE h = LoadLibraryA(path.c_str());
        return Library{reinterpret_cast<void*>(h)};
#else
        return Library{dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL)};
#endif
    }

    void* Symbol(const char* name) const {
        if (!handle) return nullptr;
#if defined(_WIN32)
        return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(handle), name));
#else
        return dlsym(handle, name);
#endif
    }

    std::string LastError() const {
#if defined(_WIN32)
        return "Windows 错误码 " + std::to_string(GetLastError());
#else
        const char* e = dlerror();
        return e ? e : "未知动态库错误";
#endif
    }

    void Close() {
        if (!handle) return;
#if defined(_WIN32)
        FreeLibrary(reinterpret_cast<HMODULE>(handle));
#else
        dlclose(handle);
#endif
        handle = nullptr;
    }
};

/// 受支持的动态库扩展名（含平台惯例）。
bool IsSupportedLibrary(const fs::path& p) {
    const std::string ext = p.extension().string();
#if defined(_WIN32)
    return ext == ".dll";
#elif defined(__APPLE__)
    return ext == ".dylib" || ext == ".so";
#else
    return ext == ".so";
#endif
}

}  // namespace

PluginLoadResult LoadPluginFile(const std::string& path) {
    PluginLoadResult result;
    Library lib = Library::Open(path);
    if (!lib.handle) {
        result.error = "无法加载动态库 " + path + "：" + lib.LastError();
        return result;
    }

    // 取三个必需符号。用 memcpy 把 void* 转成函数指针，避免 -pedantic 下
    // reinterpret_cast<void*>(func_ptr) 的标准冲突（POSIX 允许，但只是扩展）。
    AbiVersionFn abiFn = nullptr;
    CreatePluginFn createFn = nullptr;
    DestroyPluginFn destroyFn = nullptr;
    void* symAbi = lib.Symbol(kSymbolAbiVersion);
    void* symCreate = lib.Symbol(kSymbolCreate);
    void* symDestroy = lib.Symbol(kSymbolDestroy);
    if (!symAbi || !symCreate || !symDestroy) {
        result.error = "动态库 " + path + " 缺少必需的插件导出符号";
        lib.Close();
        return result;
    }
    std::memcpy(&abiFn, &symAbi, sizeof(abiFn));
    std::memcpy(&createFn, &symCreate, sizeof(createFn));
    std::memcpy(&destroyFn, &symDestroy, sizeof(destroyFn));

    const int pluginAbi = abiFn();
    if (pluginAbi != kPluginAbiVersion) {
        result.error = "动态库 " + path + " 的 ABI 版本 " + std::to_string(pluginAbi) +
                       " 与宿主 " + std::to_string(kPluginAbiVersion) + " 不匹配";
        lib.Close();
        return result;
    }

    IPlugin* raw = createFn();
    if (!raw) {
        result.error = "动态库 " + path + " 的 ppet_plugin_create 返回空指针";
        lib.Close();
        return result;
    }

    // 包装成 shared_ptr：自定义 deleter 先销毁插件对象，再关闭库句柄。
    // 用 shared_ptr<void> 捕获库句柄，确保即使拷贝 shared_ptr 也只关闭一次。
    void* handle = lib.handle;
    auto libGuard = std::shared_ptr<void>(handle, [](void* h) {
        Library l{h};
        l.Close();
    });
    result.plugin = PluginPtr(raw, [destroyFn, libGuard](IPlugin* p) {
        destroyFn(p);
        // libGuard 在此 lambda 析构时释放 -> dlclose。
        (void)libGuard;
    });
    return result;
}

std::vector<std::string> LoadPluginsFromDirectory(PluginManager& manager,
                                                  const std::string& directory) {
    std::vector<std::string> loaded;
    std::error_code ec;
    if (directory.empty() || !fs::is_directory(fs::path(directory), ec)) return loaded;

    // 固定顺序遍历，保证加载次序可复现（同目录同名插件后加载者覆盖）。
    std::set<std::string> candidates;
    for (fs::directory_iterator it(fs::path(directory), ec), end;
         !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file() && IsSupportedLibrary(it->path())) {
            candidates.insert(it->path().string());
        }
    }
    for (const std::string& path : candidates) {
        const PluginLoadResult res = LoadPluginFile(path);
        if (res.plugin) {
            const std::string name = res.plugin->Name();
            manager.Register(res.plugin);
            loaded.push_back(name);
        }
    }
    return loaded;
}

const char* DefaultPluginDirectoryName() { return "plugins"; }

}  // namespace ppet::plugin
