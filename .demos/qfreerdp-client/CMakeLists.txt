cmake_minimum_required(VERSION 3.16)
project(qf-client LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 自动处理 Qt 的元对象和资源系统
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# 1. 寻找 Qt 6 的核心与 QML 相关模块
find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick)
find_package(FreeRDP 3 REQUIRED)
find_package(WinPR 3 REQUIRED)
find_package(FreeRDP-Client 3 REQUIRED)

# 2. 定义可执行目标（使用 Qt 专属的包装函数）
qt_add_executable(qf-client
    mini-qf-client.cc
    qf_channel_client_handler.c
    ../../channels/cliprdr/client/cliprdr_main.c
    ../../channels/cliprdr/client/cliprdr_format.c
    ../../channels/cliprdr/cliprdr_common.c
)

# 3. 注册 QML 模块
# 这会自动把 main.qml 打包进资源系统（虚拟路径为 qrc:/MyTestApp/main.qml）
qt_add_qml_module(qf-client
    URI MyTestApp
    VERSION 1.0
    QML_FILES main.qml
    SOURCES rdp-view-item.h
)

# FreeRDP 3 installs headers under include/freerdp3. Some distro CMake
# targets expose only /usr/include, so add the package include dirs explicitly.
target_include_directories(qf-client PRIVATE
    ${FreeRDP_INCLUDE_DIR}
    ${FreeRDP-Client_INCLUDE_DIR}
    ${WinPR_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

# 4. 链接 Qt 模块
target_link_libraries(qf-client PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    freerdp
    freerdp-client
    winpr
)
