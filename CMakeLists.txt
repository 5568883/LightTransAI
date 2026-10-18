# ============================================================
#  轻译 AI (LightTransAI) —— CMake 构建文件
#
#  说明：本机当前未安装 CMake（VS 也未勾选 CMake 组件），
#        因此此文件尚未经过实际构建验证。
#        当前可用构建方式是 build.ps1（直连 cl.exe，已完整验证通过）。
#        安装 CMake 后可用：
#            cmake -S . -B build -G "Visual Studio 18 2026" -A x64
#            cmake --build build --config Release
#
#  依赖：仅 nlohmann/json（已随仓库提供），无其它第三方库。
# ============================================================
cmake_minimum_required(VERSION 3.20)

project(LightTransAI
    VERSION 0.1.0
    DESCRIPTION "轻译 AI - 轻量级桌面翻译工具"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 默认静态链接 CRT，生成单文件可执行程序
if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# UTF-8 源文件与多字节字符集
if(MSVC)
    # /utf-8          源码与执行字符集均为 UTF-8。
    #                 本工程源码含中文注释与中文字符串字面量，
    #                 在系统代码页为 GBK 时若不加此选项会编译失败。
    # /Zc:__cplusplus 使 __cplusplus 报告真实标准版本。
    add_compile_options(
        /utf-8
        /Zc:__cplusplus
        /permissive-
        /W4
        /MP
    )
    # 注意：刻意不定义 WIN32_LEAN_AND_MEAN。
    # 它会让 windows.h 不再包含 unknwn.h，导致 UIAutomation.h 中
    # 的接口声明缺少 IUnknown 基类而编译失败。
    add_compile_definitions(UNICODE _UNICODE NOMINMAX)
endif()

# ---------------- 源文件 ----------------
set(LT_SOURCES
    src/main.cpp

    src/app/app_controller.cpp
    src/app/tray_manager.cpp
    src/app/settings_window.cpp

    src/config/config_manager.cpp
    src/config/crypto_dpapi.cpp

    src/input/hotkey_manager.cpp
    src/input/mouse_selection_hook.cpp
    src/input/text_extractor.cpp

    src/network/winhttp_client.cpp
    src/translation/deepseek_client.cpp

    src/ui/ui_common.cpp
    src/ui/float_button_window.cpp
    src/ui/popup_window.cpp

    src/utils/string_utils.cpp
    src/utils/logger.cpp
    src/utils/perf_monitor.cpp
)

set(LT_HEADERS
    src/app/app_messages.h
    src/app/app_controller.h
    src/app/tray_manager.h
    src/app/settings_window.h

    src/config/config_manager.h
    src/config/crypto_dpapi.h

    src/input/hotkey_manager.h
    src/input/mouse_selection_hook.h
    src/input/text_extractor.h

    src/network/winhttp_client.h
    src/translation/deepseek_client.h

    src/ui/ui_common.h
    src/ui/float_button_window.h
    src/ui/popup_window.h

    src/utils/string_utils.h
    src/utils/logger.h
    src/utils/perf_monitor.h
)

# ---------------- 可执行程序 ----------------
add_executable(LightTransAI WIN32 ${LT_SOURCES} ${LT_HEADERS})

target_include_directories(LightTransAI PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

# 资源文件（版本信息）
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/app.rc")
    target_sources(LightTransAI PRIVATE app.rc)
endif()

# 嵌入清单（Per-Monitor DPI Aware V2 + Common Controls v6）
if(MSVC)
    target_link_options(LightTransAI PRIVATE
        "/MANIFEST:EMBED"
        "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/app.manifest"
    )
endif()

# ---------------- 链接库 ----------------
# 全部为 Windows 系统内置组件，无第三方依赖
target_link_libraries(LightTransAI PRIVATE
    user32          # 窗口、消息、钩子、热键
    gdi32           # 基础绘图与位图
    shell32         # 托盘图标、Shell 功能
    ole32           # COM（UIAutomation / WIC / Direct2D）
    oleaut32        # BSTR 等自动化类型
    advapi32        # 注册表与安全
    psapi           # GetProcessMemoryInfo
    winhttp         # HTTPS 请求
    crypt32         # DPAPI
    d2d1            # Direct2D
    dwrite          # DirectWrite
    windowscodecs   # WIC 图像编码
    dwmapi          # DWM 圆角与投影
    uiautomationcore # UIAutomation 取词
    comctl32        # 通用控件
    shlwapi         # Shell 轻量工具
    shcore          # SetProcessDpiAwareness 回退
)

# ---------------- 单元测试 ----------------
enable_testing()

add_executable(LightTransAI_tests
    tests/test_all.cpp

    src/config/crypto_dpapi.cpp
    src/config/config_manager.cpp
    src/utils/string_utils.cpp
    src/utils/logger.cpp
    src/utils/perf_monitor.cpp
)

target_include_directories(LightTransAI_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

target_link_libraries(LightTransAI_tests PRIVATE
    advapi32 crypt32 shell32 ole32 user32 psapi
)

add_test(NAME LightTransAI_UnitTests COMMAND LightTransAI_tests)

# ---------------- 输出信息 ----------------
message(STATUS "轻译 AI 配置完成")
message(STATUS "  C++ 标准: ${CMAKE_CXX_STANDARD}")
message(STATUS "  构建类型: ${CMAKE_BUILD_TYPE}")
