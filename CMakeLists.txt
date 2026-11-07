# PPet - C++ 移植版
# 构建配置。
#
# 说明：
#   - 本 CMakeLists 聚焦「模型后端」这一层，渲染层与窗口层的实现由主协调者提供，
#     因此对尚未存在的源文件做了存在性判断，缺失时用占位实现顶上，
#     保证任何阶段都能 configure / build 成功。
cmake_minimum_required(VERSION 3.16)
project(PPet LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 在一串候选路径里挑第一个真实存在的文件，结果写回 OUT_VAR（找不到则写空串）。
#
# 为什么不用 find_library()/find_file()：
#   这两个命令受 CMAKE_FIND_ROOT_PATH 影响。交叉编译时若设置了
#   CMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY，搜索路径会被前缀到 sysroot 下，
#   于是「SDK 文件明明躺在源码目录里，find_library 却说找不到」。
#   Cubism SDK 恰恰是用户自己放进源码目录的，必须绕开这层改写。
function(ppet_pick_first_existing OUT_VAR)
    foreach(_p IN LISTS ARGN)
        if(_p AND EXISTS "${_p}")
            set(${OUT_VAR} "${_p}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${OUT_VAR} "" PARENT_SCOPE)
endfunction()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
endif()

# ---------------------------------------------------------------------------
# Live2D Cubism 后端（可选）
#   - PPET_WITH_CUBISM      ：使用真实 Cubism SDK for Native（需自行下载）
#   - PPET_WITH_CUBISM_STUB ：使用仓库内置的 Cubism Core 桩（无需 SDK，仅供
#                             无 SDK 环境下验证 UI / 构建链路，不能加载真实模型）
#   两者都不开时，加载 Live2D 模型会给出「请启用 Cubism SDK」的友好提示。
#
# 自动识别：只要在 src/csdk/（或 third_party/Cubism/Core/）下检测到
# Live2DCubismCore.h，就自动开启真实 SDK 后端，无需手工传 CMake 选项。
# ---------------------------------------------------------------------------
# 区分「用户显式写了 -DPPET_WITH_CUBISM=ON/OFF」与「option() 的默认值 OFF」。
#
# 背景：option() 与 -D 传参在 CMakeCache.txt 里长得一模一样（都是 FOO:BOOL=OFF），
# 所以之前无法判断用户到底有没有明确关闭，只能一律 FORCE 覆盖 —— 结果就是
# 用户明明在 CLion 的 CMake options 里写了 -DPPET_WITH_CUBISM=OFF，
# 仍然被自动检测改成 ON，程序照样去链接 Cubism。
#
# 办法：在调用 option() 之前先问缓存里有没有这个变量。
#   - 命令行 -D 会在处理 CMakeLists.txt 之前就写入缓存 → 此时 CACHE{} 已定义；
#   - 首次 configure 且用户没传 -D → 缓存里没有 → option() 才刚创建默认值。
#
# 注意：这一段必须写在 option() 之前。option() 自己也会创建缓存项，
# 放在它后面判断就永远是「显式指定」了。
if(DEFINED CACHE{PPET_WITH_CUBISM})
    set(_ppet_cubism_explicit TRUE)
else()
    set(_ppet_cubism_explicit FALSE)
endif()

option(PPET_WITH_CUBISM "启用 Live2D 后端：需自行下载 Cubism SDK for Native 并放入 src/csdk" OFF)
option(PPET_WITH_CUBISM_STUB "使用内置 Cubism Core 桩（无需 SDK，仅供测试/验证构建链路）" OFF)
option(PPET_CUBISM_AUTODETECT "自动检测本机 Cubism SDK 并启用 Live2D 后端（显式设置 PPET_WITH_CUBISM 时以显式值为准）" ON)
set(PPET_CUBISM_ROOT "" CACHE PATH
    "Cubism SDK Core 目录所在位置（可指向 Core 目录本身或其上层）")

# 自动检测：SDK 已解压到 src/csdk/ 时自动启用，避免用户漏配 CMake 选项。
# 用户的显式设置（-DPPET_WITH_CUBISM=OFF/ON）优先级更高，不会被这里覆盖。
if(NOT PPET_WITH_CUBISM AND NOT PPET_WITH_CUBISM_STUB
   AND PPET_CUBISM_AUTODETECT AND NOT _ppet_cubism_explicit)
    foreach(_auto_root
            ${PPET_CUBISM_ROOT}
            ${PPET_CUBISM_ROOT}/Core
            ${CMAKE_CURRENT_SOURCE_DIR}/src/csdk/Core
            ${CMAKE_CURRENT_SOURCE_DIR}/src/csdk
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/Cubism/Core)
        if(_auto_root AND EXISTS "${_auto_root}/include/Live2DCubismCore.h")
            set(PPET_WITH_CUBISM ON CACHE BOOL "启用 Live2D 后端：需自行下载 Cubism SDK for Native 并放入 src/csdk" FORCE)
            message(STATUS "检测到 Cubism SDK（${_auto_root}），自动启用 Live2D 后端")
            break()
        endif()
    endforeach()
elseif(NOT PPET_WITH_CUBISM AND _ppet_cubism_explicit)
    message(STATUS
        "Live2D 后端：PPET_WITH_CUBISM 被显式设为关闭（命令行 -D 或既有构建缓存），"
        "已跳过 SDK 自动检测。想开启请传 -DPPET_WITH_CUBISM=ON")
endif()

# 统一开启严格警告，尽早暴露问题。
add_compile_options(-Wall -Wextra)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# OpenGL 库要先找出来：渲染层（Renderer.cpp）要直接调用 GL 入口点，
# 所以它所在的 ppet_model 目标得自己链接，不能依赖平台层间接传递。
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# 跨平台依赖解析：GLFW / GLEW / X11
# 原则：
#   - 优先使用已安装的包（vcpkg / 系统包管理器 / pkg-config）。
#   - GLFW 缺失时自动用 FetchContent 从源码构建，保证仓库“开箱即编”，
#     不要求各平台手工安装。
#   - X11 是 Linux 专属，绝不在 Windows/macOS 上进入链接命令，
#     否则会出现 “unable to find library -lX11”。
# ---------------------------------------------------------------------------

# GLFW：窗口与输入。找不到系统包时回退到源码构建。
set(PPET_GLFW_TARGET "")
find_package(glfw3 QUIET)
if(TARGET glfw)
    set(PPET_GLFW_TARGET glfw)
elseif(TARGET glfw3)
    set(PPET_GLFW_TARGET glfw3)
else()
    find_library(PPET_GLFW_LIB NAMES glfw glfw3)
    if(PPET_GLFW_LIB)
        set(PPET_GLFW_TARGET ${PPET_GLFW_LIB})
        # find_library 只给出库文件路径，不含头文件目录。
        # 这里补一次头文件搜索，否则 #include <GLFW/glfw3.h> 可能找不到。
        find_path(PPET_GLFW_INCLUDE_DIR NAMES GLFW/glfw3.h)
        if(PPET_GLFW_INCLUDE_DIR)
            include_directories(${PPET_GLFW_INCLUDE_DIR})
            message(STATUS "GLFW 头文件目录：${PPET_GLFW_INCLUDE_DIR}")
        endif()
    else()
        include(FetchContent)
        FetchContent_Declare(glfw
            GIT_REPOSITORY https://github.com/glfw/glfw.git
            GIT_TAG 3.3.10
            GIT_SHALLOW ON)
        set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(glfw)
        set(PPET_GLFW_TARGET glfw)
    endif()
endif()

# GLEW：OpenGL 扩展入口点加载。
# 注意：不使用 find_package(GLEW)。部分 MinGW / CMake 组合（如实测 CMake 4.2 +
# llvm-mingw）下，FindGLEW.cmake 在无法创建 GLEW::GLEW 导入目标时会直接报错
# （get_target_property() called with non-existent target "GLEW::GLEW"）。
# GLEW 在三大平台都以裸库名 “GLEW” 可用（Windows 为 -lGLEW，Linux 为 -lGLEW），
# 直接按库名链接即可，与原仓库行为一致，且对 find_package 的版本/环境差异免疫。
set(PPET_GLEW_TARGET GLEW)

# 工具链只提供「静态」libGLEW.a（无 glew32.dll / 导入库）。若不定义 GLEW_STATIC，
# <GL/glew.h> 会把 __glewXxx 声明为 __declspec(dllimport)，要求从 DLL 导入，
# 从而导致 Windows 链接期 “undefined symbol: __declspec(dllimport) __glewXxx”。
# 因此必须全局声明静态链接。该宏在 Linux 上基本无副作用（dllimport 修饰仅 Windows 生效）。
add_compile_definitions(GLEW_STATIC)

# X11：仅 Linux 需要（点击穿透、托盘透明抠图等）。
set(PPET_X11_LIBRARIES "")
if(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
    set(PPET_X11_LIBRARIES ${X11_LIBRARIES})
endif()

# ---------------------------------------------------------------------------
# 模型后端静态库
# ---------------------------------------------------------------------------
set(PPET_MODEL_SOURCES
    src/core/FileIO.cpp
    src/model/SkinnedMesh.cpp
    src/model/pmx/PmxParser.cpp
    src/model/pmx/PmxModel.cpp
    src/model/vrm/GltfParser.cpp
    src/model/vrm/VrmImage.cpp
    src/model/vrm/VrmModel.cpp
)

# ---------------------------------------------------------------------------
# zlib（vendored）
# ---------------------------------------------------------------------------
# 用于 ZIP 模型包的解压（inflate）。源码随仓库提供（third_party/zlib），
# 这样 Windows 上无需用户自行安装 zlib 依赖。
add_library(ppet_zlib STATIC
    third_party/zlib/adler32.c
    third_party/zlib/compress.c
    third_party/zlib/crc32.c
    third_party/zlib/deflate.c
    third_party/zlib/gzclose.c
    third_party/zlib/gzlib.c
    third_party/zlib/gzread.c
    third_party/zlib/gzwrite.c
    third_party/zlib/infback.c
    third_party/zlib/inffast.c
    third_party/zlib/inflate.c
    third_party/zlib/inftrees.c
    third_party/zlib/trees.c
    third_party/zlib/uncompr.c
    third_party/zlib/zutil.c
)
target_include_directories(ppet_zlib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/zlib)
# 第三方代码不参与本项目的严格告警，避免大量无关警告淹没真正的编译错误。
if(MSVC)
    target_compile_options(ppet_zlib PRIVATE /w)
else()
    target_compile_options(ppet_zlib PRIVATE -w)
endif()

# 渲染层实现（主协调者提供）。缺失时用测试桩顶上，保证库仍可链接。
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/render/Renderer.cpp)
    list(APPEND PPET_MODEL_SOURCES
        src/render/Renderer.cpp
        src/render/Camera.cpp
        src/render/RenderUtils.cpp
    )
else()
    message(STATUS "src/render/Renderer.cpp 尚未提供，使用测试桩 RendererStub.cpp")
    list(APPEND PPET_MODEL_SOURCES tools/test_stubs/RendererStub.cpp)
endif()

# Live2D Cubism 后端：开启真实 SDK 或桩时，把 live2d 源编入模型库。
if(PPET_WITH_CUBISM OR PPET_WITH_CUBISM_STUB)
    list(APPEND PPET_MODEL_SOURCES
        src/model/live2d/Live2DModel.cpp
        src/model/live2d/Live2DMotion.cpp
        src/model/live2d/Live2DManifest.cpp
        src/model/live2d/CubismCoreAdapter.cpp
    )
endif()
if(PPET_WITH_CUBISM_STUB)
    # 桩实现提供与真实 SDK 同名同签名的 csm* 函数。
    list(APPEND PPET_MODEL_SOURCES
        tools/test_stubs/CubismStub/CubismStubCore.cpp)
endif()

add_library(ppet_model STATIC ${PPET_MODEL_SOURCES})
target_include_directories(ppet_model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
# 渲染层直接调用 gl* 入口点，链接关系对外 PUBLIC，
# 这样测试目标只要链 ppet_model 就自动拿到 GL。
target_link_libraries(ppet_model PUBLIC ${PPET_GLEW_TARGET} ${OPENGL_LIBRARIES})

# Live2D 后端的头文件目录 / 库 / 编译宏，按所选模式配置。
if(PPET_WITH_CUBISM AND PPET_WITH_CUBISM_STUB)
    message(FATAL_ERROR "PPET_WITH_CUBISM 与 PPET_WITH_CUBISM_STUB 不能同时开启")
endif()

if(PPET_WITH_CUBISM)
    # 真实 Cubism SDK for Native 的 Core 目录（只含 include/ 与 lib/）。
    #
    # 支持多个放置位置，按优先级查找，第一个命中的生效：
    #   1. src/csdk/Core            —— 推荐：把 SDK 的 Core 目录解压到这里
    #   2. src/csdk                 —— 直接把 Core 的内容解压到这个目录
    #   3. third_party/Cubism/Core  —— 旧位置，保持兼容
    #   4. 外部路径 -DPPET_CUBISM_ROOT=<dir> —— 指向 Core 目录或其上层
    #
    # 这样「把 SDK 解压到 src/ 下即可被自动识别」，无需手工配置头文件路径。
    set(_cubism_root_candidates
        ${CMAKE_CURRENT_SOURCE_DIR}/src/csdk/Core
        ${CMAKE_CURRENT_SOURCE_DIR}/src/csdk
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/Cubism/Core
    )
    if(PPET_CUBISM_ROOT)
        list(INSERT _cubism_root_candidates 0 ${PPET_CUBISM_ROOT})
        list(INSERT _cubism_root_candidates 1 ${PPET_CUBISM_ROOT}/Core)
    endif()

    set(CUBISM_CORE_INCLUDE_DIR "")
    set(CUBISM_CORE_ROOT "")
    foreach(_root IN LISTS _cubism_root_candidates)
        if(EXISTS "${_root}/include/Live2DCubismCore.h")
            set(CUBISM_CORE_INCLUDE_DIR "${_root}/include")
            set(CUBISM_CORE_ROOT "${_root}")
            break()
        endif()
    endforeach()

    if(NOT CUBISM_CORE_INCLUDE_DIR)
        message(FATAL_ERROR
            "未找到 Live2DCubismCore.h。请把 Cubism SDK for Native 解压到 "
            "src/csdk/（即 src/csdk/Core/include/Live2DCubismCore.h 存在），"
            "或用 -DPPET_CUBISM_ROOT=<Core 目录> 指定位置。")
    endif()

    if(WIN32)
        # Cubism SDK 5 for Windows 提供两套产物：
        #   静态库 lib/windows/x86_64/<VS版本号>/Live2DCubismCore_{MT,MD}.lib
        #   动态库 dll/windows/x86_64/Live2DCubismCore.dll + 同名 .lib 导入库
        #
        # 【为什么 MinGW 必须优先用 DLL】
        #   静态库是用 MSVC 编译的，内部引用了一批 MSVC CRT 专有符号
        #   （__security_cookie / __GSHandlerCheck / __imp___stdio_common_* 等），
        #   并通过 .drectve 里的 /DEFAULTLIB:"MSVCRT" 与 OLDNAMES 指令
        #   告诉链接器如何接 CRT。
        #   GNU ld（mingw-w64 / llvm-mingw 用的是 lld）无法完整解析这些指令，
        #   会出现两类错误：
        #     1) 缺少 __security_cookie 等符号 → undefined reference；
        #     2) SDK 的 lib 里同时存在 PlatformLogger.c 与 PlatformLogger_Win.c
        #        两套互斥实现（都定义 __local_stdio_printf_options），
        #        GNU ld 按需拉取静态库成员时会把两个都拉进来 → multiple definition。
        #   动态库方案把整个 Cubism Core 连同它的 MSVC 运行时依赖封进 DLL，
        #   链接时只需要一个干净的导入库，上述问题全部消失。
        #
        # MSVC 用户用静态库即可（MSVC 链接器能正确处理 .drectve）。
        #
        # DLL 导入库对两种工具链都可用，先把它找出来（存在与否后面再决定用不用）：
        #   dll/windows/x86_64/Live2DCubismCore.lib  ← 导入库
        #   dll/windows/x86_64/Live2DCubismCore.dll  ← 运行时真正的实现
        ppet_pick_first_existing(_cubism_import_library
            "${CUBISM_CORE_ROOT}/dll/windows/x86_64/Live2DCubismCore.lib"
            "${CUBISM_CORE_ROOT}/dll/windows/x86_64/liblive2dcubismcore.dll.a"
            "${CUBISM_CORE_ROOT}/dll/windows/Live2DCubismCore.lib"
            "${CUBISM_CORE_ROOT}/dll/Live2DCubismCore.lib")

        ppet_pick_first_existing(_cubism_runtime_dll
            "${CUBISM_CORE_ROOT}/dll/windows/x86_64/Live2DCubismCore.dll"
            "${CUBISM_CORE_ROOT}/dll/windows/Live2DCubismCore.dll"
            "${CUBISM_CORE_ROOT}/dll/Live2DCubismCore.dll")

        # 静态库子目录里的数字是 Visual Studio 版本号
        # （143=VS2022、142=VS2019、141=VS2017），取最高的即可。
        # 静态库文件名带 _MT / _MD 后缀：
        #   _MD = 动态 CRT（与 MSVC 运行库一致，最稳）
        #   _MT = 静态 CRT
        ppet_pick_first_existing(_cubism_static_library
            "${CUBISM_CORE_ROOT}/lib/windows/x86_64/143/Live2DCubismCore_MD.lib"
            "${CUBISM_CORE_ROOT}/lib/windows/x86_64/143/Live2DCubismCore_MT.lib"
            "${CUBISM_CORE_ROOT}/lib/windows/x86_64/143/Live2DCubismCore.lib"
            "${CUBISM_CORE_ROOT}/lib/windows/x86_64/142/Live2DCubismCore_MD.lib"
            "${CUBISM_CORE_ROOT}/lib/windows/x86_64/142/Live2DCubismCore_MT.lib"
            "${CUBISM_CORE_ROOT}/lib/windows/x86_64/142/Live2DCubismCore.lib"
            "${CUBISM_CORE_ROOT}/lib/windows/x86_64/141/Live2DCubismCore_MD.lib"
            "${CUBISM_CORE_ROOT}/lib/windows/x86_64/141/Live2DCubismCore_MT.lib"
            "${CUBISM_CORE_ROOT}/lib/windows/x86_64/141/Live2DCubismCore.lib"
            "${CUBISM_CORE_ROOT}/lib/windows/x86_64/Live2DCubismCore_MD.lib"
            "${CUBISM_CORE_ROOT}/lib/windows/x86_64/Live2DCubismCore_MT.lib"
            "${CUBISM_CORE_ROOT}/lib/windows/x86_64/Live2DCubismCore.lib")

        # 兼容 find_library 的老式命名（万一 SDK 目录结构有变化）。
        if(NOT _cubism_import_library)
            find_library(_cubism_import_library
                NAMES Live2DCubismCore
                PATHS ${CUBISM_CORE_ROOT}/dll
                PATH_SUFFIXES windows/x86_64 windows
                NO_DEFAULT_PATH)
        endif()
        if(NOT _cubism_static_library)
            find_library(_cubism_static_library
                NAMES Live2DCubismCore_MD Live2DCubismCore_MT Live2DCubismCore
                PATHS ${CUBISM_CORE_ROOT}/lib
                PATH_SUFFIXES windows/x86_64/143 windows/x86_64/142 windows/x86_64/141 windows/x86_64
                NO_DEFAULT_PATH)
        endif()

        if(MINGW)
            # MinGW / llvm-mingw：只能用 DLL 导入库。
            #
            # 这里绝对不能回退到 MSVC 静态库。用户的构建日志里出现过：
            #   ld.lld: error: could not open 'libLIBCMT.a': No such file or directory
            #   ld.lld: error: could not open 'libOLDNAMES.a': No such file or directory
            # 原因就是链接器拿到了 143/Live2DCubismCore_MT.lib，其 .drectve 里的
            # /DEFAULTLIB:"LIBCMT" / "OLDNAMES" 指令让 GNU ld 去解析 MSVC 专有库。
            # 与其让 lld 抛一堆天书，不如在这里直接说清楚怎么改。
            if(_cubism_import_library)
                set(CUBISM_CORE_LIBRARY "${_cubism_import_library}")
                if(NOT _cubism_runtime_dll)
                    message(FATAL_ERROR
                        "找到了 Cubism 导入库 ${_cubism_import_library}，"
                        "但没找到配套的 Live2DCubismCore.dll。\n"
                        "程序启动时 Windows 会因为找不到 DLL 直接退出，"
                        "退出码 0xC0000135（STATUS_DLL_NOT_FOUND）。\n"
                        "请把 ${CUBISM_CORE_ROOT}/dll/windows/x86_64/Live2DCubismCore.dll "
                        "补齐，或用 -DPPET_CUBISM_ROOT 指向完整的 SDK 目录。")
                endif()
                set(CUBISM_CORE_DLL "${_cubism_runtime_dll}")
                message(STATUS "Live2D 后端：MinGW —— 使用 Cubism Core 动态库导入库（${CUBISM_CORE_LIBRARY}）")
            else()
                if(_cubism_static_library)
                    set(_cubism_hint
                        "检测到 MSVC 静态库 ${_cubism_static_library}，但 MinGW/llvm-mingw 无法链接它。")
                else()
                    set(_cubism_hint "在 ${CUBISM_CORE_ROOT} 下既没找到 DLL 导入库，也没找到静态库。")
                endif()
                message(FATAL_ERROR
                    "${_cubism_hint}\n"
                    "MinGW/llvm-mingw 必须使用 Cubism SDK 的 DLL 导入库方案。\n"
                    "原因：SDK 的静态库（Live2DCubismCore_MT.lib / _MD.lib）由 MSVC 编译，"
                    "内部带有 /DEFAULTLIB:\"LIBCMT\"、/DEFAULTLIB:\"OLDNAMES\" 等链接指令，"
                    "GNU ld / lld 不认识，会报\n"
                    "    ld.lld: error: could not open 'libLIBCMT.a': No such file or directory\n"
                    "    ld.lld: error: could not open 'libOLDNAMES.a': No such file or directory\n"
                    "解决办法（任选其一）：\n"
                    "  1) 使用 SDK 自带的动态库：确认以下两个文件存在，"
                    "CMake 会自动把 DLL 拷到可执行文件旁边：\n"
                    "       ${CUBISM_CORE_ROOT}/dll/windows/x86_64/Live2DCubismCore.lib\n"
                    "       ${CUBISM_CORE_ROOT}/dll/windows/x86_64/Live2DCubismCore.dll\n"
                    "  2) 改用 MSVC 工具链构建（CLion 里换成 Visual Studio 的 generator）；\n"
                    "  3) 暂时不要 Live2D 后端：加 -DPPET_WITH_CUBISM=OFF"
                    "（显式关闭后不会再被自动检测覆盖）。")
            endif()
        else()
            # MSVC：优先静态库，链接器能正确处理 .drectve，且不需要额外拷 DLL。
            if(_cubism_static_library)
                set(CUBISM_CORE_LIBRARY "${_cubism_static_library}")
                message(STATUS "Live2D 后端：MSVC —— 使用 Cubism Core 静态库（${CUBISM_CORE_LIBRARY}）")
            elseif(_cubism_import_library)
                # 没找到静态库时退回 DLL 导入库（一样可用，只是运行时要带 DLL）。
                set(CUBISM_CORE_LIBRARY "${_cubism_import_library}")
                set(CUBISM_CORE_DLL "${_cubism_runtime_dll}")
                message(STATUS "Live2D 后端：MSVC —— 未找到静态库，改用 DLL 导入库（${CUBISM_CORE_LIBRARY}）")
            endif()
        endif()
    elseif(APPLE)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)")
            set(_cubism_arch arm64)
        else()
            set(_cubism_arch x86_64)
        endif()
        ppet_pick_first_existing(_cubism_static_library
            "${CUBISM_CORE_ROOT}/lib/macos/${_cubism_arch}/libLive2DCubismCore.a"
            "${CUBISM_CORE_ROOT}/lib/macos/libLive2DCubismCore.a")
    else()
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)")
            set(_cubism_arch aarch64)
        else()
            set(_cubism_arch x86_64)
        endif()
        ppet_pick_first_existing(_cubism_static_library
            "${CUBISM_CORE_ROOT}/lib/linux/${_cubism_arch}/libLive2DCubismCore.a"
            "${CUBISM_CORE_ROOT}/lib/linux/libLive2DCubismCore.a")
    endif()
    if(NOT CUBISM_CORE_LIBRARY AND NOT CUBISM_CORE_DLL)
        # 兜底：SDK 目录结构非标准时，回到 find_library 传统查找。
        if(WIN32)
            set(_cubism_lib_names Live2DCubismCore_MD Live2DCubismCore_MT Live2DCubismCore)
            set(_cubism_lib_subdirs windows/x86_64/143 windows/x86_64/142 windows/x86_64/141 windows/x86_64)
        elseif(APPLE)
            set(_cubism_lib_names Live2DCubismCore)
            set(_cubism_lib_subdirs macos)
        else()
            set(_cubism_lib_names Live2DCubismCore)
            set(_cubism_lib_subdirs linux/x86_64)
        endif()
        find_library(CUBISM_CORE_LIBRARY
            NAMES ${_cubism_lib_names}
            PATHS ${CUBISM_CORE_ROOT}/lib
            PATH_SUFFIXES ${_cubism_lib_subdirs}
            NO_DEFAULT_PATH)
    endif()
    if(NOT CUBISM_CORE_LIBRARY)
        message(FATAL_ERROR
            "未找到 Cubism Core 库（libLive2DCubismCore.a / Live2DCubismCore_MT.lib / "
            "Live2DCubismCore.lib）。请确认 ${CUBISM_CORE_ROOT}/lib 下的平台子目录"
            "与你的工具链匹配。")
    endif()

    target_include_directories(ppet_model PUBLIC ${CUBISM_CORE_INCLUDE_DIR})
    target_link_libraries(ppet_model PUBLIC ${CUBISM_CORE_LIBRARY})
    target_compile_definitions(ppet_model PUBLIC PPET_WITH_CUBISM)

    # Cubism Core 4.x 与 5.x/6.x 的绘制顺序 API 名字不同：
    #   旧版：csmGetDrawableRenderOrders(model)
    #   新版：csmGetRenderOrders(model)
    # 直接写死任何一个都会在另一种 SDK 上编译失败（undeclared identifier）。
    # 这里读头文件内容判断实际提供的是哪个，再定义对应的宏。
    file(READ "${CUBISM_CORE_INCLUDE_DIR}/Live2DCubismCore.h" _cubism_core_header)
    if(_cubism_core_header MATCHES "csmGetRenderOrders")
        target_compile_definitions(ppet_model PUBLIC PPET_CUBISM_HAS_RENDER_ORDERS)
        message(STATUS "Live2D 后端：检测到 Cubism Core 绘制顺序 API = csmGetRenderOrders")
    elseif(_cubism_core_header MATCHES "csmGetDrawableRenderOrders")
        target_compile_definitions(ppet_model PUBLIC PPET_CUBISM_HAS_DRAWABLE_RENDER_ORDERS)
        message(STATUS "Live2D 后端：检测到 Cubism Core 绘制顺序 API = csmGetDrawableRenderOrders（Core 4.x）")
    else()
        message(FATAL_ERROR
            "Cubism Core 头文件中既没有 csmGetRenderOrders 也没有 csmGetDrawableRenderOrders，"
            "无法确定绘制顺序 API。请使用 Cubism SDK for Native 4.x 及以上版本。")
    endif()

    message(STATUS "Live2D 后端：启用真实 Cubism SDK（${CUBISM_CORE_LIBRARY}）")
elseif(PPET_WITH_CUBISM_STUB)
    target_include_directories(ppet_model PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_stubs/CubismStub)
    target_compile_definitions(ppet_model PUBLIC PPET_WITH_CUBISM_STUB)
    # 桩同时提供新旧两个绘制顺序 API，与真实 SDK 6.x 一致，
    # 因此按新版宏处理。
    target_compile_definitions(ppet_model PUBLIC PPET_CUBISM_HAS_RENDER_ORDERS)
    message(STATUS "Live2D 后端：使用内置 Cubism Core 桩（无需 SDK）")
endif()

# ---------------------------------------------------------------------------
# 平台层静态库（窗口 / 托盘 / 开机启动 / 外部链接 / 文件对话框）
# ---------------------------------------------------------------------------
set(PPET_PLATFORM_SOURCES
    src/platform/Window.cpp
    src/platform/Tray.cpp
    src/platform/AutoStart.cpp
    src/platform/OpenExternal.cpp
    src/platform/FileDialog.cpp
    src/platform/GlobalCursorTracker.cpp
)

# MSVC CRT 兼容层：历史遗留的安全网。
#
# Cubism SDK for Native 只提供 MSVC 编译的 .lib，这些库引用了
# __security_cookie / __security_check_cookie / __GSHandlerCheck /
# __isa_available 等 MSVC UCRT 专有符号，MinGW 的 CRT 不提供，
# 直接链接会得到 undefined reference（程序无法启动）。
#
# 现在的策略是：MinGW 一律走 DLL 导入库（见上面的 if(MINGW) 分支），
# 根本不会去链接 MSVC 静态库，所以正常情况下这个文件不会被编译。
# 保留它是为了在有人手工把静态库塞进链接命令时仍有一层兜底。
#
# 注意：
#   - 用动态库（DLL 导入库）时不需要本文件，DLL 内部已解决全部依赖；
#   - 用 MSVC 编译时也不需要，文件内部有 #error 保护避免与 MSVC CRT 冲突。
if(MINGW AND PPET_WITH_CUBISM AND NOT CUBISM_CORE_DLL)
    list(APPEND PPET_PLATFORM_SOURCES src/platform/MsvcCrtCompat.cpp)
    message(STATUS "Live2D 后端：启用 MSVC CRT 兼容层（MinGW + Cubism 静态库）")
endif()

add_library(ppet_platform STATIC ${PPET_PLATFORM_SOURCES})
target_include_directories(ppet_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(ppet_platform PUBLIC ${PPET_GLFW_TARGET} ${PPET_GLEW_TARGET} ${OPENGL_LIBRARIES} ${PPET_X11_LIBRARIES})

if(UNIX AND NOT APPLE)
    # XShape：Linux 下的点击穿透（输入区域）与托盘图标透明抠图。
    target_link_libraries(ppet_platform PRIVATE Xext)
    target_link_libraries(ppet_platform PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)

    # Xft（可选）：用于绘制托盘菜单文字。核心 X 字体无法显示中文，
    # 缺少 Xft 时托盘菜单会退化为「仅 ASCII 可读」。
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        # 托盘菜单直接调用了 fontconfig 的 API（FcFontMatch / FcDefaultSubstitute 等），
        # 因此 fontconfig 必须单独链接，不能依赖 xft 的间接依赖。
        pkg_check_modules(PPET_XFT QUIET xft)
        pkg_check_modules(PPET_FONTCONFIG QUIET fontconfig)
        pkg_check_modules(PPET_FREETYPE QUIET freetype2)
    endif()
    if(PPET_XFT_FOUND AND PPET_FONTCONFIG_FOUND)
        message(STATUS "找到 Xft：Linux 托盘菜单将支持中文显示")
        target_compile_definitions(ppet_platform PUBLIC PPET_HAS_XFT)
        target_include_directories(ppet_platform PRIVATE ${PPET_XFT_INCLUDE_DIRS}
                                   ${PPET_FONTCONFIG_INCLUDE_DIRS}
                                   ${PPET_FREETYPE_INCLUDE_DIRS})
        target_link_libraries(ppet_platform PRIVATE ${PPET_XFT_LIBRARIES}
                              ${PPET_FONTCONFIG_LIBRARIES} ${PPET_FREETYPE_LIBRARIES})
        target_link_directories(ppet_platform PRIVATE ${PPET_XFT_LIBRARY_DIRS}
                                ${PPET_FONTCONFIG_LIBRARY_DIRS}
                                ${PPET_FREETYPE_LIBRARY_DIRS})
    else()
        message(STATUS "未找到 Xft：Linux 托盘菜单文字将退化为核心 X 字体（不支持中文）")
    endif()
endif()

if(WIN32)
    # dwmapi   ：真透明（DwmExtendFrameIntoClientArea）
    # shell32  ：托盘图标与 ShellExecute
    # comdlg32 ：文件选择对话框
    # advapi32 ：开机启动注册表
    target_link_libraries(ppet_platform PRIVATE dwmapi shell32 comdlg32 advapi32)
endif()

if(APPLE)
    # 窗口层与托盘层使用 Cocoa，需要以 Objective-C++ 编译。
    set_source_files_properties(src/platform/Window.cpp src/platform/Tray.cpp
                                PROPERTIES LANGUAGE OBJCXX)
    target_link_libraries(ppet_platform PRIVATE "-framework Cocoa"
                          "-framework ApplicationServices")
endif()

# ---------------------------------------------------------------------------
# Qt 6（设置界面）
# ---------------------------------------------------------------------------
# 设置界面改用 Qt 6 Widgets 实现（独立 QDialog 窗口），取代原先的 Dear ImGui。
# 这样原生控件 / 中文 / 高 DPI 天然支持，且不再需要第二个 GL 上下文。
#
# 提供 Qt6 安装路径的方式（任选其一）：
#   - 把 Qt6 的 cmake 目录加进 CMAKE_PREFIX_PATH，例如：
#       Windows (llvm-mingw): -DCMAKE_PREFIX_PATH="C:/Qt/6.7.2/llvm-mingw_64/lib/cmake"
#       macOS              : -DCMAKE_PREFIX_PATH="~/Qt/6.7.2/macos/lib/cmake"
#   - Linux 通常由发行包（qt6-base-dev）提供，无需手动设置。
find_package(Qt6 REQUIRED COMPONENTS Widgets)

# ---------------------------------------------------------------------------
# Compiler/：文案翻译器
#
# 把 Compiler/i18n/*.json 里的界面文案「翻译」成 C++ 内嵌表
# （constexpr 表 + X 宏），生成到构建目录下的 core/I18nStrings.inl。
#
# 这样中英文文案只有一份来源（JSON），不再在 src/core/I18n.cpp 里手抄两遍；
# 生成器还会在构建期校验「文案键 ↔ struct Strings 成员」一一对应，
# 拼错键、少翻译都会立刻报错，而不是等到界面上出现空白。
# ---------------------------------------------------------------------------
if(CMAKE_CROSSCOMPILING)
    # 生成器必须跑在宿主机上。交叉编译时借 wine 运行；没有 wine 就明确报错，
    # 而不是让构建走到一半才失败。
    find_program(PPET_WINE_EXECUTABLE wine)
    if(PPET_WINE_EXECUTABLE)
        set(CMAKE_CROSSCOMPILING_EMULATOR ${PPET_WINE_EXECUTABLE})
    else()
        message(FATAL_ERROR
            "交叉编译时无法运行文案翻译器（它必须跑在宿主机上）。\n"
            "请安装 wine 并把它加进 PATH，或在宿主机上先跑一次生成器：\n"
            "    cmake --build <build> --target ppet_i18n_gen_run")
    endif()
endif()

add_executable(ppet_i18n_gen
    Compiler/i18n_gen/i18n_gen.cpp
    Compiler/i18n_gen/main.cpp
)
target_include_directories(ppet_i18n_gen PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Compiler/i18n_gen
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

set(PPET_I18N_ZH ${CMAKE_CURRENT_SOURCE_DIR}/Compiler/i18n/zh-CN.json CACHE FILEPATH
    "中文文案源文件（.json 或 .csv）")
set(PPET_I18N_EN ${CMAKE_CURRENT_SOURCE_DIR}/Compiler/i18n/en-US.json CACHE FILEPATH
    "英文文案源文件（.json 或 .csv）")
set(PPET_I18N_STRUCT_HEADER ${CMAKE_CURRENT_SOURCE_DIR}/src/core/I18n.hpp CACHE FILEPATH
    "struct Strings 所在的头文件（用于校验文案键与成员一一对应）")
set(PPET_I18N_GEN_DIR ${CMAKE_BINARY_DIR}/generated)
set(PPET_I18N_INL ${PPET_I18N_GEN_DIR}/core/I18nStrings.inl)

add_custom_command(
    OUTPUT ${PPET_I18N_INL}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PPET_I18N_GEN_DIR}/core
    COMMAND ppet_i18n_gen
            --out ${PPET_I18N_INL}
            --struct ${PPET_I18N_STRUCT_HEADER}
            --zh ${PPET_I18N_ZH}
            --en ${PPET_I18N_EN}
    DEPENDS ppet_i18n_gen ${PPET_I18N_ZH} ${PPET_I18N_EN} ${PPET_I18N_STRUCT_HEADER}
    COMMENT "翻译界面文案：Compiler/i18n/*.json -> core/I18nStrings.inl"
    VERBATIM)
add_custom_target(ppet_i18n_gen_run DEPENDS ${PPET_I18N_INL})
message(STATUS "文案翻译器：${PPET_I18N_ZH} + ${PPET_I18N_EN} -> ${PPET_I18N_INL}")

# ---------------------------------------------------------------------------
# 主程序
# ---------------------------------------------------------------------------
set(PPET_CORE_SOURCES
    src/core/Config.cpp
    src/core/I18n.cpp
    src/core/Log.cpp
    src/core/ModelPackage.cpp
    src/core/ZipArchive.cpp
    src/model/Model.cpp
    src/app/App.cpp
    src/ui/SettingsPanel.cpp
    src/ui/SettingsPanel.hpp
)

add_executable(ppet ${PPET_CORE_SOURCES} src/main.cpp)
target_link_libraries(ppet PRIVATE ppet_model ppet_platform ppet_zlib Qt6::Widgets)
set_target_properties(ppet PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)


# MinGW + Cubism 动态库方案：把 Live2DCubismCore.dll 拷到可执行文件旁边，
# 否则程序启动时会因找不到 DLL 而失败。
if(CUBISM_CORE_DLL AND EXISTS "${CUBISM_CORE_DLL}")
    add_custom_command(TARGET ppet POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CUBISM_CORE_DLL}" "$<TARGET_FILE_DIR:ppet>"
        COMMENT "拷贝 Cubism Core 动态库到可执行文件目录")
    message(STATUS "Live2D 后端：将自动拷贝 ${CUBISM_CORE_DLL} 到输出目录")
endif()

# ---------------------------------------------------------------------------
# MinGW / llvm-mingw 运行时 DLL 部署
#
# 现象：程序双击就闪退，CLion 显示
#   "进程已结束，退出代码为 3221225781 (0xc0000135)"
# 0xC0000135 = STATUS_DLL_NOT_FOUND，含义是「Windows 加载器找不到某个依赖 DLL」。
# 这跟 Cubism 无关时，通常是 MinGW 运行库没跟 exe 放在一起：
#   libgcc_s_seh-1.dll / libstdc++-6.dll / libwinpthread-1.dll
#
# 这些 DLL 一般就在编译器（clang++.exe / x86_64-w64-mingw32-g++.exe）旁边，
# 所以构建后顺手把它们拷到 exe 所在目录，省得用户自己找。
# ---------------------------------------------------------------------------
if(MINGW)
    set(_ppet_mingw_runtime_dlls "")
    get_filename_component(_ppet_cc_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(_ppet_c_dir "${CMAKE_C_COMPILER}" DIRECTORY)
    foreach(_rt_name libgcc_s_seh-1.dll libgcc_s_dw2-1.dll libstdc++-6.dll libwinpthread-1.dll)
        string(MAKE_C_IDENTIFIER "_ppet_rt_${_rt_name}" _rt_var)
        find_file(${_rt_var}
            NAMES ${_rt_name}
            PATHS ${_ppet_cc_dir} ${_ppet_c_dir}
            NO_DEFAULT_PATH)
        if(${_rt_var})
            list(APPEND _ppet_mingw_runtime_dlls "${${_rt_var}}")
        endif()
    endforeach()
    if(_ppet_mingw_runtime_dlls)
        foreach(_rt_dll IN LISTS _ppet_mingw_runtime_dlls)
            add_custom_command(TARGET ppet POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${_rt_dll}" "$<TARGET_FILE_DIR:ppet>"
                COMMENT "拷贝 MinGW 运行时 DLL（${_rt_dll}）到可执行文件目录")
        endforeach()
        message(STATUS "MinGW：运行时 DLL 将随程序一起部署（${_ppet_mingw_runtime_dlls}）")
    else()
        message(WARNING
            "没能在编译器目录找到 MinGW 运行时 DLL（libgcc_s_seh-1.dll / libstdc++-6.dll / "
            "libwinpthread-1.dll）。若程序启动时报 0xC0000135（STATUS_DLL_NOT_FOUND），"
            "请把这些 DLL 从工具链 bin 目录拷到 exe 旁边，或改用静态链接。")
    endif()
endif()

# ---------------------------------------------------------------------------
# 平台层冒烟测试
# ---------------------------------------------------------------------------
add_executable(ppet_platform_smoke tools/platform_smoke.cpp)
target_link_libraries(ppet_platform_smoke PRIVATE ppet_platform)

# 有 xvfb-run 时用它提供虚拟显示（无人值守环境下也能跑完整的图形路径）；
# 否则直接运行，测试程序在没有显示时会以退出码 2 表示「跳过图形检查」。
find_program(PPET_XVFB_RUN xvfb-run)
if(PPET_XVFB_RUN)
    add_test(NAME platform_smoke
             COMMAND ${PPET_XVFB_RUN} -a $<TARGET_FILE:ppet_platform_smoke>
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
else()
    add_test(NAME platform_smoke
             COMMAND ppet_platform_smoke
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()
set_tests_properties(platform_smoke PROPERTIES SKIP_RETURN_CODE 2)

# ---------------------------------------------------------------------------
# 渲染层冒烟测试
# ---------------------------------------------------------------------------
# 真实创建 GLFW 上下文 + GLEW，编译着色器、绘制网格，
# 再用 glReadPixels 把像素读回来做断言。
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/render_smoke.cpp)
    add_executable(ppet_render_smoke
        tools/render_smoke.cpp
        src/core/FileIO.cpp
        src/render/Renderer.cpp
        src/render/Camera.cpp
        src/render/RenderUtils.cpp
    )
    target_include_directories(ppet_render_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(ppet_render_smoke PRIVATE ${PPET_GLEW_TARGET} ${PPET_GLFW_TARGET} ${OPENGL_LIBRARIES})
endif()

# 端到端集成测试：真实模型文件 -> 模型后端 -> 渲染层 -> glReadPixels。
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/render_integration.cpp)
    add_executable(ppet_render_integration
        tools/render_integration.cpp
        src/core/FileIO.cpp
        src/model/SkinnedMesh.cpp
        src/model/pmx/PmxParser.cpp
        src/model/pmx/PmxModel.cpp
        src/model/vrm/GltfParser.cpp
        src/model/vrm/VrmImage.cpp
        src/model/vrm/VrmModel.cpp
        src/render/Renderer.cpp
        src/render/Camera.cpp
        src/render/RenderUtils.cpp
    )
    target_include_directories(ppet_render_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(ppet_render_integration PRIVATE ${PPET_GLEW_TARGET} ${PPET_GLFW_TARGET} ${OPENGL_LIBRARIES})
endif()

if(UNIX AND NOT APPLE)
    # 最小化的「系统托盘宿主」模拟器，用于在无人值守环境中验证 Linux 托盘实现。
    add_executable(ppet_fake_tray tools/x11_fake_tray.cpp)
    target_link_libraries(ppet_fake_tray PRIVATE X11)
endif()

# ---------------------------------------------------------------------------
# 设置界面冒烟测试（Qt 6）
# ---------------------------------------------------------------------------
# 设置界面是独立 Qt 窗口，验证：QApplication 可用、对话框创建成功、
# Show/Hide/Toggle 可见性切换、PumpEvents/Refresh 不崩溃。
# 用 offscreen 平台插件运行，无需真实显示；没有该插件时跳过（退出码 2）。
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/settings_panel_smoke.cpp)
    add_executable(ppet_settings_panel_smoke
        tools/settings_panel_smoke.cpp
        src/ui/SettingsPanel.cpp
        src/core/I18n.cpp
        src/core/Log.cpp
    )
    target_include_directories(ppet_settings_panel_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(ppet_settings_panel_smoke PRIVATE Qt6::Widgets)
endif()

# 设置面板截图工具：用真实 Qt 控件渲染并抓取 PNG，用于 README 配图。
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/settings_panel_screenshot.cpp)
    add_executable(ppet_settings_panel_screenshot
        tools/settings_panel_screenshot.cpp
        src/ui/SettingsPanel.cpp
        src/core/I18n.cpp
        src/core/Log.cpp
    )
    target_include_directories(ppet_settings_panel_screenshot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(ppet_settings_panel_screenshot PRIVATE Qt6::Widgets)
endif()

# Live2D 渲染验证工具：仅真实 Cubism SDK 模式下构建（桩无法加载真实模型）。
# 用隐藏 GLFW 窗口 + GLEW 渲染一个兼容的 Live2D 模型，读回像素并合成 PNG。
if(PPET_WITH_CUBISM AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/live2d_verify.cpp)
    add_executable(ppet_live2d_verify tools/live2d_verify.cpp)
    # 验证工具自己创建 GLFW 窗口 + GLEW 上下文，因此需要直接链接 GL/GLFW。
    target_link_libraries(ppet_live2d_verify PRIVATE
        ppet_model
        ${PPET_GLEW_TARGET}
        ${PPET_GLFW_TARGET}
        ${OPENGL_LIBRARIES})
    # stb_image_write 用于把渲染结果导出成 PNG 做人工确认。
    target_include_directories(ppet_live2d_verify PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb)
    # 第三方头文件的告警不参与本项目的严格告警。
    if(NOT MSVC)
        set_source_files_properties(tools/live2d_verify.cpp
                                    PROPERTIES COMPILE_OPTIONS "-w")
    endif()
endif()

# ---------------------------------------------------------------------------
# 测试
# ---------------------------------------------------------------------------
enable_testing()

add_executable(ppet_pmx_test tools/pmx_test.cpp)
target_link_libraries(ppet_pmx_test PRIVATE ppet_model)

# ZIP 模型包导入测试：解压、定位模型、幂等性与非法输入。
add_executable(ppet_zip_test
    tools/zip_test.cpp
    src/core/FileIO.cpp
    src/core/Log.cpp
    src/core/ModelPackage.cpp
    src/core/ZipArchive.cpp
)
target_link_libraries(ppet_zip_test PRIVATE ppet_zlib)

# 中文路径处理测试：验证 UTF-8 路径在文件读写、目录遍历、解压定位各层都不被
# 系统 ANSI 代码页破坏（Windows 上尤其关键）。
add_executable(ppet_unicode_path_test
    tools/unicode_path_test.cpp
    src/core/FileIO.cpp
    src/core/Log.cpp
    src/core/ModelPackage.cpp
    src/core/ZipArchive.cpp
)
target_link_libraries(ppet_unicode_path_test PRIVATE ppet_zlib)

add_executable(ppet_pmx_fuzz tools/pmx_fuzz.cpp src/model/pmx/PmxParser.cpp)
target_link_libraries(ppet_pmx_fuzz PRIVATE ppet_model)

add_executable(ppet_pmx_dump tools/pmx_dump.cpp)
target_link_libraries(ppet_pmx_dump PRIVATE ppet_model)

add_executable(ppet_vrm_test tools/vrm_test.cpp)
target_link_libraries(ppet_vrm_test PRIVATE ppet_model)

add_executable(ppet_vrm_fuzz tools/vrm_fuzz.cpp)
target_link_libraries(ppet_vrm_fuzz PRIVATE ppet_model)

add_executable(ppet_vrm_dump tools/vrm_dump.cpp)
target_link_libraries(ppet_vrm_dump PRIVATE ppet_model)

# 测试素材由脚本生成，测试前先跑一次确保数据存在。
add_custom_target(ppet_testdata
    COMMAND ${CMAKE_COMMAND} -E echo "生成 PMX 测试素材"
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_test_pmx.py
    COMMAND ${CMAKE_COMMAND} -E echo "生成 VRM 测试素材"
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_test_vrm.py
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

add_dependencies(ppet_pmx_test ppet_testdata)
add_dependencies(ppet_pmx_fuzz ppet_testdata)
add_dependencies(ppet_vrm_test ppet_testdata)
add_dependencies(ppet_vrm_fuzz ppet_testdata)

add_test(NAME pmx_unit
         COMMAND ppet_pmx_test
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

add_test(NAME vrm_unit
         COMMAND ppet_vrm_test
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# ---------------------------------------------------------------------------
# 文案翻译器（Compiler/i18n_gen）的单元测试
# ---------------------------------------------------------------------------
add_executable(ppet_i18n_gen_test
    Compiler/i18n_gen/i18n_gen.cpp
    Compiler/i18n_gen/i18n_gen_test.cpp
)
target_include_directories(ppet_i18n_gen_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Compiler/i18n_gen
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
add_test(NAME i18n_gen_unit
         COMMAND ppet_i18n_gen_test
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# 端到端检查：真实的文案源文件能不能被翻译器正确处理，
# 并且与构建时生成的 .inl 内容一致（防止有人改了 JSON 却忘了重建）。
add_test(NAME i18n_gen_project
         COMMAND ppet_i18n_gen
                 --out ${PPET_I18N_INL}
                 --struct ${PPET_I18N_STRUCT_HEADER}
                 --zh ${PPET_I18N_ZH}
                 --en ${PPET_I18N_EN}
                 --check
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# 渲染层冒烟测试必须跑在真实 GL 上下文里，所以优先用 xvfb-run 提供虚拟显示；
# 没有 xvfb 时直接运行，测试程序在没有显示时会以退出码 2 表示「跳过图形检查」。
if(TARGET ppet_render_smoke)
    if(PPET_XVFB_RUN)
        add_test(NAME render_smoke
                 COMMAND ${PPET_XVFB_RUN} -a $<TARGET_FILE:ppet_render_smoke>
                 WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    else()
        add_test(NAME render_smoke
                 COMMAND ppet_render_smoke
                 WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endif()
    set_tests_properties(render_smoke PROPERTIES SKIP_RETURN_CODE 2)
endif()

# 端到端集成测试同样需要真实的 GL 上下文与测试素材。
if(TARGET ppet_render_integration)
    if(PPET_XVFB_RUN)
        add_test(NAME render_integration
                 COMMAND ${PPET_XVFB_RUN} -a $<TARGET_FILE:ppet_render_integration>
                 WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    else()
        add_test(NAME render_integration
                 COMMAND ppet_render_integration
                 WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endif()
    set_tests_properties(render_integration PROPERTIES SKIP_RETURN_CODE 2)
endif()

# 设置界面冒烟测试（Qt）：用 offscreen 平台插件运行，无需真实显示。
if(TARGET ppet_settings_panel_smoke)
    add_test(NAME settings_panel_smoke
             COMMAND $<TARGET_FILE:ppet_settings_panel_smoke>
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    # QT_QPA_PLATFORM=offscreen：不依赖 X / 显示器；
    # SKIP_RETURN_CODE 2：offscreen 插件不可用或 QApplication 初始化失败时跳过。
    set_tests_properties(settings_panel_smoke PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        SKIP_RETURN_CODE 2)
endif()

# ZIP 模型包导入测试：需要一个测试用 zip。
# 测试包由 tools/gen_test_zip.py 生成（含一个最小 PMX 与一张贴图），
# 生成后再运行导入测试。
set(PPET_TEST_ZIP ${CMAKE_CURRENT_BINARY_DIR}/testdata/model_package.zip)
if(TARGET ppet_zip_test)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND AND
       EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_test_zip.py)
        add_test(NAME zip_package_test
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_test_zip.py
                         ${PPET_TEST_ZIP})
        set_tests_properties(zip_package_test PROPERTIES
            FIXTURES_SETUP ppet_zip_fixture)

        add_test(NAME zip_import_test
                 COMMAND $<TARGET_FILE:ppet_zip_test> ${PPET_TEST_ZIP}
                 WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(zip_import_test PROPERTIES
            FIXTURES_REQUIRED ppet_zip_fixture
            SKIP_RETURN_CODE 2)
    endif()
endif()

# 中文路径测试：无需外部素材，可直接运行。
if(TARGET ppet_unicode_path_test)
    add_test(NAME unicode_path_test
             COMMAND $<TARGET_FILE:ppet_unicode_path_test>
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# ---------------------------------------------------------------------------
# 文案翻译器的消费方登记
#
# src/core/I18n.cpp 会 include 生成出来的 core/I18nStrings.inl，
# 因此凡是编译了 I18n.cpp 的目标，都需要：
#   1) 依赖 ppet_i18n_gen_run —— 保证 .inl 已经生成；
#   2) 把生成目录加进头文件搜索路径。
#
# 这里统一扫描全部目标来登记，避免以后新增目标时漏掉（漏了就是
# "fatal error: core/I18nStrings.inl: No such file or directory"）。
# ---------------------------------------------------------------------------
get_property(_ppet_all_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(_ppet_t IN LISTS _ppet_all_targets)
    get_target_property(_ppet_t_sources ${_ppet_t} SOURCES)
    if(_ppet_t_sources AND "src/core/I18n.cpp" IN_LIST _ppet_t_sources)
        add_dependencies(${_ppet_t} ppet_i18n_gen_run)
        target_include_directories(${_ppet_t} PRIVATE ${PPET_I18N_GEN_DIR})
        message(STATUS "文案翻译器：目标 ${_ppet_t} 依赖生成的内嵌表")
    endif()
endforeach()
