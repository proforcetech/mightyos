cmake_minimum_required(VERSION 3.21)

project(MightyOS
    VERSION 0.0.1
    DESCRIPTION "Console-style TV launcher for Ubuntu"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# qmlcachegen's AOT-compiled QML produces large C++ files (~0.5-1 GB of RAM
# per compiler process). Cap parallel compile jobs by memory so that a plain
# `cmake --build build` does not get OOM-killed on small dev VMs.
if(CMAKE_GENERATOR MATCHES "Ninja")
    cmake_host_system_information(RESULT _mem_mb QUERY TOTAL_PHYSICAL_MEMORY)
    cmake_host_system_information(RESULT _cores QUERY NUMBER_OF_LOGICAL_CORES)
    math(EXPR _jobs "${_mem_mb} / 1000")
    if(_jobs LESS 1)
        set(_jobs 1)
    elseif(_jobs GREATER _cores)
        set(_jobs ${_cores})
    endif()
    set(MIGHTYOS_COMPILE_JOBS ${_jobs} CACHE STRING "Max parallel compile jobs (Ninja)")
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS mightyos_compile=${MIGHTYOS_COMPILE_JOBS})
    set(CMAKE_JOB_POOL_COMPILE mightyos_compile)
endif()

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Qml Quick)
qt_standard_project_setup(REQUIRES 6.8)

# Common settings for our own targets (not Qt-generated helper targets).
function(mightyos_set_warnings target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    # With a shared Qt, QML plugins are loaded at run time and nothing needs
    # to be linked, so skip qmlimportscanner. (Ubuntu's Qt 6.10 packages ship
    # no Qt6QmlPlugins.cmake, which otherwise yields one bogus "plugin will
    # not be linked" warning per imported module.)
    if(QT6_IS_SHARED_LIBS_BUILD)
        set_target_properties(${target} PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
    endif()
    target_compile_definitions(${target} PRIVATE
        QT_NO_CAST_FROM_ASCII
        QT_NO_URL_CAST_FROM_STRING
        QT_NO_NARROWING_CONVERSIONS_IN_CONNECT)
endfunction()

# ---------------------------------------------------------------------------
# mightyos: the launcher itself
# ---------------------------------------------------------------------------
qt_add_executable(mightyos
    src/main.cpp
)

set_source_files_properties(qml/Theme.qml qml/InputMap.qml
    PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(mightyos
    URI MightyOS.UI
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/Theme.qml
        qml/InputMap.qml
        qml/Home.qml
        qml/Tile.qml
        qml/TileRow.qml
        qml/FocusRing.qml
        qml/Icon.qml
        qml/PromptBar.qml
        qml/Clock.qml
        qml/Toast.qml
        qml/LaunchOverlay.qml
    SOURCES
        src/launcher.h
        src/launcher.cpp
)

target_include_directories(mightyos PRIVATE src)
target_compile_definitions(mightyos PRIVATE
    MIGHTYOS_VERSION="${PROJECT_VERSION}")
target_link_libraries(mightyos PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick)
mightyos_set_warnings(mightyos)

# ---------------------------------------------------------------------------
# mightyos-testapp: tiny GUI program used as the M0 launch target.
# Dev/test only; replaced by real apps from AppRegistry in M2.
# ---------------------------------------------------------------------------
qt_add_executable(mightyos-testapp
    testapp/main.cpp
)

qt_add_qml_module(mightyos-testapp
    URI MightyOS.TestApp
    VERSION 1.0
    QML_FILES
        testapp/TestApp.qml
)

target_link_libraries(mightyos-testapp PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick)
mightyos_set_warnings(mightyos-testapp)


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
option(MIGHTYOS_BUILD_TESTS "Build unit tests" ON)
if(MIGHTYOS_BUILD_TESTS)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Test)
    enable_testing()

    qt_add_executable(tst_launcher
        tests/tst_launcher.cpp
        src/launcher.h
        src/launcher.cpp
    )
    target_include_directories(tst_launcher PRIVATE src)
    target_link_libraries(tst_launcher PRIVATE Qt6::Core Qt6::Qml Qt6::Test)
    mightyos_set_warnings(tst_launcher)
    add_dependencies(tst_launcher mightyos-testapp)
    target_compile_definitions(tst_launcher PRIVATE
        MIGHTYOS_TESTAPP_PATH="$<TARGET_FILE:mightyos-testapp>")

    add_test(NAME tst_launcher COMMAND tst_launcher)
    set_tests_properties(tst_launcher PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
