cmake_minimum_required(VERSION 3.16)
project(Qt6Test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 自动处理 Qt 的元对象和资源系统
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# 1. 寻找 Qt 6 的核心与 QML 相关模块
find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick)

# 2. 定义可执行目标（使用 Qt 专属的包装函数）
qt_add_executable(Qt6Test
    main.cpp
)

# 3. 注册 QML 模块
# 这会自动把 main.qml 打包进资源系统（虚拟路径为 qrc:/MyTestApp/main.qml）
qt_add_qml_module(Qt6Test
    URI MyTestApp
    VERSION 1.0
    QML_FILES main.qml
)

# 4. 链接 Qt 模块
target_link_libraries(Qt6Test PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
)
