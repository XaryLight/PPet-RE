# PPet - C++ 移植版
# 构建配置。
#
# 说明：
#   - 本 CMakeLists 聚焦「模型后端」这一层，渲染层与窗口层的实现由主协调者提供，
#     因此对尚未存在的源文件做了存在性判断，缺失时用占位实现顶上，
#     保证任何阶段都能 configure / build 成功。
cmake_minimum_required(VERSION 3.16)
project(PPet LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
endif()

# 统一开启严格警告，尽早暴露问题。
add_compile_options(-Wall -Wextra)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# OpenGL 库要先找出来：渲染层（Renderer.cpp）要直接调用 GL 入口点，
# 所以它所在的 ppet_model 目标得自己链接，不能依赖平台层间接传递。
find_package(OpenGL REQUIRED)
find_package(X11 REQUIRED)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# 模型后端静态库
# ---------------------------------------------------------------------------
set(PPET_MODEL_SOURCES
    src/model/SkinnedMesh.cpp
    src/model/pmx/PmxParser.cpp
    src/model/pmx/PmxModel.cpp
    src/model/vrm/GltfParser.cpp
    src/model/vrm/VrmImage.cpp
    src/model/vrm/VrmModel.cpp
)

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

add_library(ppet_model STATIC ${PPET_MODEL_SOURCES})
target_include_directories(ppet_model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
# 渲染层直接调用 gl* 入口点，链接关系对外 PUBLIC，
# 这样测试目标只要链 ppet_model 就自动拿到 GL。
target_link_libraries(ppet_model PUBLIC GLEW ${OPENGL_LIBRARIES})

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

add_library(ppet_platform STATIC ${PPET_PLATFORM_SOURCES})
target_include_directories(ppet_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(ppet_platform PUBLIC glfw GLEW ${OPENGL_LIBRARIES} X11)

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
# 主程序
# ---------------------------------------------------------------------------
set(PPET_CORE_SOURCES
    src/core/Config.cpp
    src/core/I18n.cpp
    src/model/Model.cpp
    src/app/App.cpp
)

add_executable(ppet ${PPET_CORE_SOURCES} src/main.cpp)
target_link_libraries(ppet PRIVATE ppet_model ppet_platform)
set_target_properties(ppet PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

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
        src/render/Renderer.cpp
        src/render/Camera.cpp
        src/render/RenderUtils.cpp
    )
    target_include_directories(ppet_render_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(ppet_render_smoke PRIVATE GLEW glfw ${OPENGL_LIBRARIES})
endif()

# 端到端集成测试：真实模型文件 -> 模型后端 -> 渲染层 -> glReadPixels。
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/render_integration.cpp)
    add_executable(ppet_render_integration
        tools/render_integration.cpp
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
    target_link_libraries(ppet_render_integration PRIVATE GLEW glfw ${OPENGL_LIBRARIES})
endif()

if(UNIX AND NOT APPLE)
    # 最小化的「系统托盘宿主」模拟器，用于在无人值守环境中验证 Linux 托盘实现。
    add_executable(ppet_fake_tray tools/x11_fake_tray.cpp)
    target_link_libraries(ppet_fake_tray PRIVATE X11)
endif()

# ---------------------------------------------------------------------------
# 测试
# ---------------------------------------------------------------------------
enable_testing()

add_executable(ppet_pmx_test tools/pmx_test.cpp)
target_link_libraries(ppet_pmx_test PRIVATE ppet_model)

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
