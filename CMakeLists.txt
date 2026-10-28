# CommandCapture - capture every command a script or executable spawns.
#
# Copyright (c) 2026 maijun
#
# SPDX-License-Identifier: MIT
#
# Released under the MIT License. See the LICENSE file at the repository
# root for the full text.

# ============================================================================
# CommandCapture - 模块化构建
#
# 三个库 + 一个入口：
#
#   ccap_core      纯跨平台，无平台依赖（事件模型、JSON、命令行切分、静态分析）
#   ccap_capture   需要 OS 能力（进程捕获）+ C ABI
#   ccap_compdb    编译命令识别/解析/导出 + 编译器适配器
#   ccap           CLI 入口
#
# 依赖方向是单向的：
#   ccap_compdb --> ccap_core
#   ccap_capture --> ccap_core
#   ccap         --> 上面全部
#
# ccap_compdb **不依赖** ccap_capture —— 它只吃"一串命令行 + 工作目录"，
# 不关心这些命令是怎么来的。这样才能吃 CMake / ninja 产生的编译数据库。
# ============================================================================

cmake_minimum_required(VERSION 3.20)
# C 和 C++ 都要 —— tests/test_capi.c 是 C 程序，
# 用来验证 C ABI 真的能被外部程序（而不是 C++）使用。
project(CommandCapture C CXX)

# 生成 compile_commands.json。
#
# 用途：clang-tidy 需要它才能拿到正确的编译参数（include 路径、宏定义、
# 目标平台）。没有它的话，clang-tidy 只能用启发式猜 include 路径，
# 报出一堆假的"头文件找不到"。
#
# 代价：几乎为零（只是多写一个 json 文件）。
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CCAP_MODULES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/modules")

# ─────────────────────────────────────────────────────────────────────────────
# ccap_core
#
# 纯算法与纯数据。**不能**引用任何平台 API。
# 这一点是模块划分的核心约束 —— 违反它，core 就没法在别的平台上复用。
# ─────────────────────────────────────────────────────────────────────────────
add_library(ccap_core STATIC
  modules/ccap_core/core/event.cpp
  modules/ccap_core/core/json.cpp
  modules/ccap_core/core/json_parser.cpp
  modules/ccap_core/core/command_line.cpp
  modules/ccap_core/core/normalize.cpp
  modules/ccap_core/core/script_scanner.cpp
  modules/ccap_core/core/powershell_scanner.cpp
  modules/ccap_core/core/tree.cpp
  modules/ccap_core/core/filter.cpp
)
target_include_directories(ccap_core PUBLIC
  ${CCAP_MODULES_DIR}/ccap_core
)
if(MSVC)
  target_compile_options(ccap_core PRIVATE /utf-8)
endif()
# 静态库要能链进共享库（so/dll），必须用 -fPIC 编译。
#
# 不加的话，Linux 上链接 libccap_capture.so 会报：
#   "relocation R_X86_64_PC32 ... can not be used when making a shared object"
set_target_properties(ccap_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ─────────────────────────────────────────────────────────────────────────────
# ccap_capture
#
# 静态库：给内部链接用。
# 动态库（so/dll）：给外部程序用（C ABI）。
# ─────────────────────────────────────────────────────────────────────────────
set(CCAP_CAPTURE_SOURCES
  modules/ccap_capture/capture/platform.cpp
  modules/ccap_capture/capture/capi.cpp
)

if(WIN32)
  list(APPEND CCAP_CAPTURE_SOURCES
    modules/ccap_capture/capture/windows_debugger.cpp
    modules/ccap_capture/capture/peb_reader.cpp
  )
else()
  list(APPEND CCAP_CAPTURE_SOURCES
    modules/ccap_capture/capture/linux_preload.cpp
  )
endif()

add_library(ccap_capture STATIC ${CCAP_CAPTURE_SOURCES})
target_link_libraries(ccap_capture PUBLIC ccap_core)
# 静态库要能链进共享库（so/dll），必须用 -fPIC 编译。
#
# 不加的话，Linux 上链接 libccap_capture.so 会报：
#   "relocation R_X86_64_PC32 ... can not be used when making a shared object"
set_target_properties(ccap_capture PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(ccap_capture
  PUBLIC  ${CCAP_MODULES_DIR}/ccap_capture/include
          ${CCAP_MODULES_DIR}/ccap_capture
)
if(MSVC)
  target_compile_options(ccap_capture PRIVATE /utf-8)
  target_compile_definitions(ccap_capture PRIVATE
    CCAP_OS_WINDOWS=1
    _CRT_SECURE_NO_WARNINGS
    UNICODE
    _UNICODE
  )
  target_link_libraries(ccap_capture PUBLIC dbghelp)
else()
  target_compile_definitions(ccap_capture PRIVATE CCAP_OS_LINUX=1)
  find_package(Threads REQUIRED)
  target_link_libraries(ccap_capture PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
endif()

# 动态库版本 —— C ABI
add_library(ccap_capture_shared SHARED ${CCAP_CAPTURE_SOURCES})
target_link_libraries(ccap_capture_shared PUBLIC ccap_core)
target_include_directories(ccap_capture_shared
  PUBLIC  ${CCAP_MODULES_DIR}/ccap_capture/include
          ${CCAP_MODULES_DIR}/ccap_capture
)
target_compile_definitions(ccap_capture_shared PRIVATE CCAP_CAPTURE_BUILD_DLL=1)
if(MSVC)
  target_compile_options(ccap_capture_shared PRIVATE /utf-8)
  target_compile_definitions(ccap_capture_shared PRIVATE
    CCAP_OS_WINDOWS=1
    _CRT_SECURE_NO_WARNINGS
    UNICODE
    _UNICODE
  )
  target_link_libraries(ccap_capture_shared PUBLIC dbghelp)
  # 让 .dll 和 .lib 都带上 C API 导出
  #
  # 注意：输出目录必须和静态库分开 ——
  # MSVC 的 import lib 叫 <name>.lib，会**覆盖**同名的静态库。
  # （曾经踩过：ccap_capture.lib 是 3KB 的导入库，不是静态库，
  #   于是链接 ccap 时找不到 run_capture。）
  set_target_properties(ccap_capture_shared PROPERTIES
    OUTPUT_NAME ccap_capture
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/shared"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/shared"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/shared"
  )
else()
  target_compile_definitions(ccap_capture_shared PRIVATE CCAP_OS_LINUX=1)
  find_package(Threads REQUIRED)
  target_link_libraries(ccap_capture_shared PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
  # 输出目录必须和静态库分开 ——
  # 静态库也叫 libccap_capture.a，放一起会互相覆盖
  # （Windows 上是 import lib 覆盖静态库，这里是共享库覆盖静态库）。
  set_target_properties(ccap_capture_shared PROPERTIES
    OUTPUT_NAME ccap_capture
    PREFIX "lib"
    CXX_VISIBILITY_PRESET default
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/shared"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/shared"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/shared"
  )
endif()

# ─────────────────────────────────────────────────────────────────────────────
# ccap_compdb
#
# 编译命令识别 / 解析 / 导出。
# **不依赖 ccap_capture** —— 它只吃命令行文本。
# ─────────────────────────────────────────────────────────────────────────────
add_library(ccap_compdb STATIC
  modules/ccap_compdb/compdb/compiler_table.cpp
  modules/ccap_compdb/compdb/alias_resolver.cpp
  modules/ccap_compdb/compdb/command_parser.cpp
  modules/ccap_compdb/compdb/compile_command.cpp
  modules/ccap_compdb/compdb/filter_rules.cpp
  modules/ccap_compdb/compdb/compdb_exporter.cpp
  modules/ccap_compdb/compdb/clang_translator.cpp
  modules/ccap_compdb/compdb/config.cpp
  modules/ccap_compdb/adapters/adapter_registry.cpp
  modules/ccap_compdb/adapters/gcc_adapter.cpp
  modules/ccap_compdb/adapters/clang_adapter.cpp
  modules/ccap_compdb/adapters/msvc_adapter.cpp
  modules/ccap_compdb/adapters/generic_adapter.cpp
)
target_link_libraries(ccap_compdb PUBLIC ccap_core)
set_target_properties(ccap_compdb PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(ccap_compdb
  PUBLIC  ${CCAP_MODULES_DIR}/ccap_compdb/include
  PRIVATE ${CCAP_MODULES_DIR}/ccap_compdb
)
if(MSVC)
  target_compile_options(ccap_compdb PRIVATE /utf-8)
endif()

# 动态库版本
add_library(ccap_compdb_shared SHARED
  modules/ccap_compdb/compdb/compiler_table.cpp
  modules/ccap_compdb/compdb/alias_resolver.cpp
  modules/ccap_compdb/compdb/command_parser.cpp
  modules/ccap_compdb/compdb/compile_command.cpp
  modules/ccap_compdb/compdb/filter_rules.cpp
  modules/ccap_compdb/compdb/compdb_exporter.cpp
  modules/ccap_compdb/compdb/clang_translator.cpp
  modules/ccap_compdb/compdb/config.cpp
  modules/ccap_compdb/adapters/adapter_registry.cpp
  modules/ccap_compdb/adapters/gcc_adapter.cpp
  modules/ccap_compdb/adapters/clang_adapter.cpp
  modules/ccap_compdb/adapters/msvc_adapter.cpp
  modules/ccap_compdb/adapters/generic_adapter.cpp
)
target_link_libraries(ccap_compdb_shared PUBLIC ccap_core)
target_include_directories(ccap_compdb_shared
  PUBLIC  ${CCAP_MODULES_DIR}/ccap_compdb/include
  PRIVATE ${CCAP_MODULES_DIR}/ccap_compdb
)
if(MSVC)
  target_compile_options(ccap_compdb_shared PRIVATE /utf-8)
endif()
set_target_properties(ccap_compdb_shared PROPERTIES
  OUTPUT_NAME ccap_compdb
  PREFIX "lib"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/shared"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/shared"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/shared"
)

# ─────────────────────────────────────────────────────────────────────────────
# ccap —— CLI 入口
# ─────────────────────────────────────────────────────────────────────────────
set(CCAP_SOURCES
  modules/ccap/app/main.cpp
  modules/ccap/app/cli.cpp
  modules/ccap/app/compdb_cmd.cpp
)

add_executable(ccap ${CCAP_SOURCES})
target_link_libraries(ccap PRIVATE ccap_capture ccap_compdb)
target_include_directories(ccap PRIVATE
  ${CCAP_MODULES_DIR}/ccap
  ${CCAP_MODULES_DIR}/ccap_compdb/include
)
if(MSVC)
  target_compile_options(ccap PRIVATE /utf-8)
endif()

# ─────────────────────────────────────────────────────────────────────────────
# LD_PRELOAD shim（Linux）
#
# 必须是**独立**的共享库 —— 它被注入到**目标进程**里，
# 如果链接了 ccap_core 就会把一堆代码带进每个被观测的进程。
# ─────────────────────────────────────────────────────────────────────────────
if(NOT WIN32)
  add_library(ccap_preload SHARED modules/ccap_capture/capture/linux_shim.cpp)
  target_compile_definitions(ccap_preload PRIVATE CCAP_OS_LINUX=1)
  target_link_libraries(ccap_preload PRIVATE ${CMAKE_DL_LIBS})
  set_target_properties(ccap_preload PROPERTIES
    PREFIX "" OUTPUT_NAME "ccap_shim")
endif()

install(TARGETS ccap RUNTIME DESTINATION bin)
install(TARGETS ccap_capture_shared ccap_compdb_shared
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin)

# ─────────────────────────────────────────────────────────────────────────────
# 单元测试
# ─────────────────────────────────────────────────────────────────────────────
enable_testing()

add_executable(test_cmdline tests/test_cmdline.cpp)
target_link_libraries(test_cmdline PRIVATE ccap_core)
target_include_directories(test_cmdline PRIVATE ${CCAP_MODULES_DIR}/ccap_core)
if(MSVC)
  target_compile_options(test_cmdline PRIVATE /utf-8)
endif()
add_test(NAME cmdline COMMAND test_cmdline)

# 编译器识别 / 别名 / 解析 的单元测试
#
# 直接链 **ccap_compdb 库**，不重复列出源文件。
#
# 之前是把模块的 .cpp 列表在这里抄一遍 —— 那样模块加一个文件就得改两处，
# 忘了改就是"符号重复定义"或者"链接不到"，很难查。
add_executable(test_compdb tests/test_compdb.cpp)
target_link_libraries(test_compdb PRIVATE ccap_compdb ccap_core)
target_include_directories(test_compdb PRIVATE
  ${CCAP_MODULES_DIR}/ccap_compdb
  ${CCAP_MODULES_DIR}/ccap_compdb/include
)
if(MSVC)
  target_compile_options(test_compdb PRIVATE /utf-8)
endif()
add_test(NAME compdb COMMAND test_compdb)

# ─────────────────────────────────────────────────────────────────────────────
# C ABI 验证程序
#
# 这个程序**故意只链接动态库**，不链接 ccap_capture 静态库 ——
# 只有这样才能真正验证：
#   1. 导出符号正确（dllexport / visibility 都对）
#   2. C ABI 自包含（不依赖 C++ 运行时的特定版本）
#   3. 外部程序能拿到事件回调
#
# 如果它链了静态库，即使导出符号有问题也测不出来。
# ─────────────────────────────────────────────────────────────────────────────
if(WIN32)
  add_executable(test_capi tests/test_capi.c)
  target_include_directories(test_capi PRIVATE
    ${CCAP_MODULES_DIR}/ccap_capture/include)
  # 只链接动态库的 import lib。
  #
  # 链 **target** 而不是硬编码路径：
  #   硬编码 "${CMAKE_BINARY_DIR}/shared/$<CONFIG>/ccap_capture.lib" 在
  #   Ninja 生成器下会失败 —— Ninja 没有 multi-config，$<CONFIG> 展开成空，
  #   路径变成 shared//ccap_capture.lib，找不到。
  #
  #   链 target 的话，CMake 自己知道 import lib 在哪、叫什么。
  target_link_libraries(test_capi PRIVATE ccap_capture_shared)
  # 运行时能找到 dll —— exe 放到 dll 旁边
  set_target_properties(test_capi PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/shared/$<CONFIG>")
else()
  add_executable(test_capi tests/test_capi.c)
  target_include_directories(test_capi PRIVATE
    ${CCAP_MODULES_DIR}/ccap_capture/include)
  target_link_libraries(test_capi PRIVATE ccap_capture_shared)
  set_target_properties(test_capi PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/shared"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/shared")
endif()

# /utf-8：源文件是 UTF-8，但 MSVC 默认用当前代码页（中文机器上是 936/GBK），
# 会报 C4819「该文件包含不能在当前代码页中表示的字符」。
#
# 这不是"警告可以忽略"的问题 —— 它意味着**字符串字面量的字节是按错误编码解释的**。
# 我们的注释和错误信息里有中文，不加 /utf-8 就是在赌运气。
if(MSVC)
  target_compile_options(test_capi PRIVATE /utf-8)
endif()

# 把 C ABI 验证纳入 CTest。
#
# 之前它只能手工跑 —— 意味着**没人会跑它**，CI 里也不会跑。
# 手工验证过的代码如果没有门禁，迟早会坏掉且没人知道。
#
# 目标按平台选：需要一个"必定创建进程、且必定被捕获"的目标。
#   Windows: cmd.exe /c echo  -> 走 DEBUG_PROCESS，产生 1 个 runtime 事件
#   Linux:   /bin/echo        -> 走 LD_PRELOAD，产生 1 个 runtime 事件
if(WIN32)
  add_test(NAME capi COMMAND test_capi ccap_capture.dll cmd.exe /c echo capi-test)
else()
  add_test(NAME capi COMMAND test_capi libccap_capture.so /bin/echo capi-test)
endif()

# ─────────────────────────────────────────────────────────────────────────────
# 命名规范门禁
#
# tests/check_naming.py 用 clang-tidy 的 readability-identifier-naming
# 检查命名规范（见 docs/NAMING-CONVENTION.md）。
#
# 为什么值得纳入 CTest：
#   之前它只能手工跑 —— 意味着**没人会跑它**，CI 里也不会跑。
#   手工验证过的检查如果没有门禁，迟早会坏掉且没人知道。
#
# 退出码约定（脚本自己的约定，不是猜的）：
#   0  = 通过
#   1  = 有命名违规，或编译错误导致检查不完整
#   2  = 工具链缺失（clang-tidy / 编译数据库找不到）—— **无法检查**
#
# 退出码 2 用 SKIP_RETURN_CODE 标成 "skipped" 而不是 "failed"：
#   理由 —— clang-tidy 装不装是**环境**决定的，不是代码决定的。
#   把它标成失败会让 CI 在没有 clang-tidy 的机器上永远红，而红的原因跟
#   代码质量无关；那只会让人养成忽略红灯的习惯。
#
#   但它也**不是**静默通过：CTest 会明确显示 "Skipped"，
#   在 CI 日志里一眼能看出"这次没检查命名"。
#   脚本自己还会打印 "ERROR: 找不到 clang-tidy，无法检查命名。"
#
# 需要 clang-tidy 的机器上，这条测试是**真的门禁**。
# ─────────────────────────────────────────────────────────────────────────────
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME naming
    COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_naming.py"
            --build-dir "${CMAKE_BINARY_DIR}")
  set_tests_properties(naming PROPERTIES
    SKIP_RETURN_CODE 2
    TIMEOUT 1800)

  # ───────────────────────────────────────────────────────────────────────────
  # 开源协议头门禁
  #
  # 为什么单独一条，而不是并进 naming：
  #   naming 需要 clang-tidy（不是每台机器都装），而协议头检查只需要
  #   Python。混在一起的话，"没装 clang-tidy" 会把协议头检查一起跳过 ——
  #   而协议头是**每台机器都能查**的东西，不该被工具链缺失连累。
  #
  # 退出码：
  #   0 = 通过
  #   1 = 有文件缺协议头
  #   2 = 找不到源文件（路径不对）—— 无法检查
  #
  # 门禁的价值：协议头只加在 95% 的文件上，比完全没有协议头更糟 ——
  # 看起来项目是有授权的，实际有一部分文件没有任何条款。
  # ───────────────────────────────────────────────────────────────────────────
  add_test(NAME license
    COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_license.py")
  set_tests_properties(license PROPERTIES
    SKIP_RETURN_CODE 2
    TIMEOUT 300)
else()
  message(STATUS "Python3 not found - the naming gate test will not be added")
  message(STATUS "Python3 not found - the license gate test will not be added")
endif()

# ─────────────────────────────────────────────────────────────────────────────
# 调试器检测探针（用于验证 --clear-being-debugged）
# ─────────────────────────────────────────────────────────────────────────────
if(WIN32)
  add_executable(dbg_flag_target tests/probe/DbgFlagTarget.cpp)
  target_compile_options(dbg_flag_target PRIVATE /utf-8)
endif()

# ─────────────────────────────────────────────────────────────────────────────
# 一次性调试探针（默认不构建）
#
# tests/probe/ 下的程序是**调试期用来定位那些坑的工具**，
# README「踩过的坑」一节里的每条结论都靠它们复现。
#
# 之前它们只是躺在目录里的 .cpp，既不进构建、也不能验证 ——
# 等于僵尸代码：文件在，但没人知道它还能不能编译。
#
# 现在用 CCAP_BUILD_PROBES=ON 显式构建，源码保持可编译状态。
#   cmake -S . -B build -DCCAP_BUILD_PROBES=ON
#
# 每个探针都是独立的可执行文件（互相之间没有依赖），
# 所以逐个 add_executable。
# ─────────────────────────────────────────────────────────────────────────────
option(CCAP_BUILD_PROBES "Build the one-off debugging probes in tests/probe/" OFF)

if(CCAP_BUILD_PROBES)
  # Windows 调试器 API 探针
  foreach(probe
      DbgFlag
      DbgFlagTarget
      DbgPeb
      DbgTree
      DbgTree2
      DbgTree3
      DbgTree4
      DbgTree5
      DbgTree6
      DbgTree7
      DbgTree8
      psdbg)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/probe/${probe}.cpp")
      add_executable(probe_${probe} tests/probe/${probe}.cpp)
      target_compile_options(probe_${probe} PRIVATE /utf-8)
      target_link_libraries(probe_${probe} PRIVATE dbghelp)
    endif()
  endforeach()

  # 32 位目标探针（验证 WOW64 双 PEB）
  #
  # **必须用 32 位工具链编译** —— 它用 __readfsdword 直接读 32 位 TEB，
  # 在 x64 下编译会报"C3861: 找不到标识符"。
  #
  # 用法：
  #   call vcvars32.bat
  #   cmake -G "Visual Studio 17 2022" -A Win32 -S . -B build32 -DCCAP_BUILD_PROBES=ON
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/probe/Target32.cpp")
    add_executable(probe_target32 EXCLUDE_FROM_ALL tests/probe/Target32.cpp)
    target_compile_options(probe_target32 PRIVATE /utf-8)
  endif()

  # Linux shim 探针
  if(NOT WIN32)
    foreach(probe repeat_exec shim_smoke)
      if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/probe/${probe}.c")
        add_executable(probe_${probe} tests/probe/${probe}.c)
      endif()
    endforeach()
  endif()
endif()
