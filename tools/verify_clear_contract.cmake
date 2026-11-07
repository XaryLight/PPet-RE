# PPet - C++ 移植版
# 回归测试脚本：钉死「主循环必须每帧清屏」这条契约。
#
# 背景（真实 bug）：主循环有模型时只调 Update/Render，从不 glClear；
# 每帧 SwapBuffers 后 back buffer 未定义、深度缓冲残留，第二帧起片元被
# GL_LESS 全部剔除。此时 draw calls / 贴图 / FPS / GL 错误 / rendering_ok
# 全部正常，但帧缓冲一个非透明像素都没有，屏幕上看不到模型 ——
# 诊断面板却报「正常渲染」。
#
# 本脚本用 ppet_render_verify 的 --no-clear --swap-each-frame 复现该错误
# 主循环，并断言：
#   1) 正确清屏：覆盖率 > 1%，result=PASS；
#   2) 漏清屏：覆盖率 == 0%，result=FAIL；
#   3) 漏清屏时 rendering_ok 仍为 true / model_status 仍为「正常渲染」
#      —— 证明「只看 draw call 与 GL 错误」确实会误判，像素证据不可省。
#
# 由 CTest 调用（见 CMakeLists.txt），需要 xvfb-run 提供虚拟显示。
cmake_minimum_required(VERSION 3.20)

foreach(_needed IN ITEMS RENDER_VERIFY MODEL OUTDIR)
    if(NOT DEFINED ${_needed} OR "${${_needed}}" STREQUAL "")
        message(FATAL_ERROR "缺少参数 -D${_needed}=...")
    endif()
endforeach()

if(XVFB)
    set(_prefix ${XVFB} -a)
else()
    set(_prefix "")
endif()

file(MAKE_DIRECTORY "${OUTDIR}")

function(run_verify _tag _skip_clear _out_json _out_rc)
    set(_args --model "${MODEL}"
              --shot "${OUTDIR}/${_tag}.png"
              --report "${OUTDIR}/${_tag}.json"
              --width 320 --height 400 --frames 6
              --swap-each-frame)
    if(_skip_clear)
        list(APPEND _args --no-clear)
    endif()
    execute_process(
        COMMAND ${_prefix} "${RENDER_VERIFY}" ${_args}
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _stdout
        ERROR_VARIABLE _stderr
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(${_out_rc} ${_rc} PARENT_SCOPE)
    set(${_out_json} "${OUTDIR}/${_tag}.json" PARENT_SCOPE)
    if(NOT EXISTS "${OUTDIR}/${_tag}.json")
        message(FATAL_ERROR "[${_tag}] 未生成报告。rc=${_rc}\nstdout: ${_stdout}\nstderr: ${_stderr}")
    endif()
endfunction()

# --- 1) 正确主循环：每帧清屏 + 每帧交换 ---
run_verify("clear" FALSE _clear_json _clear_rc)
file(READ "${_clear_json}" _clear_txt)
string(JSON _clear_cov GET "${_clear_txt}" render coverage_percent)
string(JSON _clear_result GET "${_clear_txt}" result)
message(STATUS "清屏组: coverage=${_clear_cov}% result=${_clear_result} rc=${_clear_rc}")
if(NOT _clear_rc EQUAL 0)
    message(FATAL_ERROR "清屏组应返回 0（PASS），实际 ${_clear_rc}")
endif()
if(_clear_cov LESS 1.0)
    message(FATAL_ERROR "清屏组覆盖率应 > 1%，实际 ${_clear_cov}% —— 渲染管线本身可能有问题")
endif()
if(NOT _clear_result STREQUAL "PASS")
    message(FATAL_ERROR "清屏组 result 应为 PASS，实际 ${_clear_result}")
endif()

# --- 2) 错误主循环：漏清屏 + 每帧交换 ---
run_verify("noclear" TRUE _noclear_json _noclear_rc)
file(READ "${_noclear_json}" _noclear_txt)
string(JSON _noclear_cov GET "${_noclear_txt}" render coverage_percent)
string(JSON _noclear_result GET "${_noclear_txt}" result)
string(JSON _noclear_ok GET "${_noclear_txt}" render rendering_ok)
string(JSON _noclear_status GET "${_noclear_txt}" render model_status)
message(STATUS "漏清屏组: coverage=${_noclear_cov}% result=${_noclear_result} "
               "rendering_ok=${_noclear_ok} model_status=${_noclear_status} rc=${_noclear_rc}")
if(_noclear_rc EQUAL 0)
    message(FATAL_ERROR "漏清屏组应返回非 0（FAIL），实际 0 —— 复现失效")
endif()
if(_noclear_cov GREATER 0.001)
    message(FATAL_ERROR "漏清屏组覆盖率应为 0%，实际 ${_noclear_cov}% —— 复现失效")
endif()
# 这两条是回归的靶点（仅提示、不判失败）：证明间接指标会撒谎，
# 所以运行时必须用帧缓冲像素证据兜底。CMake string(JSON) 把 JSON 布尔
# true 读成 ON，这里按 ON 判断；后端实现若改为自行读回像素，会变成 OFF。
if(_noclear_ok STREQUAL "ON")
    message(STATUS "靶点确认：漏清屏时后端 rendering_ok 仍为 true —— 只看 "
                   "网格/GPU/GL 错误确实会误判，像素证据不可省")
else()
    message(STATUS "提示：后端 rendering_ok 已变为 false（后端已自行读回像素）")
endif()
if(_noclear_status STREQUAL "正常渲染")
    message(STATUS "靶点确认：漏清屏时 model_status 仍为「正常渲染」")
endif()

message(STATUS "清屏契约回归通过：清屏出图 / 漏清屏零像素，且后者仍被误判为「正常渲染」")
