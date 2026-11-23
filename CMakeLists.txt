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

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Qml Quick Concurrent)
# Development entry points that feed input from outside the session
# (--fake-cec for mightyos and mightyos-inputtest). ON while MightyOS is in
# development; packaged builds (M6) must set it OFF.
option(MIGHTYOS_DEV_TOOLS "Build development-only input options (--fake-cec)" ON)

# Gamepads (PLAN D6). Ubuntu: libsdl3-dev.
find_package(SDL3 3.2 REQUIRED CONFIG COMPONENTS SDL3-shared)
qt_standard_project_setup(REQUIRES 6.8)
# Each QML module gets <dir>/MightyOS/<Name>, which qmllint and qmlcachegen
# use as their import path.
set(QT_QML_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/qml)

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
# mightyos_core: C++ services exposed to QML as module MightyOS.Core.
# A static library so the app and the tests link the same code.
# ---------------------------------------------------------------------------
qt_add_library(mightyos_core STATIC)
qt_add_qml_module(mightyos_core
    URI MightyOS.Core
    VERSION 1.0
    SOURCES
        src/launcher.h src/launcher.cpp
        src/inputrouter.h src/inputrouter.cpp
        src/navstate.h src/navstate.cpp
        src/soundfx.h src/soundfx.cpp
        src/ids.h src/ids.cpp
        src/jsonfile.h src/jsonfile.cpp
        src/tilelistmodel.h src/tilelistmodel.cpp
        src/itemresolver.h
        src/library.h src/library.cpp
        src/desktopentry.h src/desktopentry.cpp
        src/iconlocator.h src/iconlocator.cpp
        src/appscanner.h src/appscanner.cpp
        src/appregistry.h src/appregistry.cpp
        src/artworkprovider.h src/artworkprovider.cpp
        src/profileservice.h src/profileservice.cpp
        src/favorites.h src/favorites.cpp
        src/history.h src/history.cpp
        src/settings.h src/settings.cpp
        src/feedback.h src/feedback.cpp
        src/textsanitize.h src/textsanitize.cpp
        src/padinput.h src/padinput.cpp
        src/padhub.h src/padhub.cpp
        src/gamepadservice.h src/gamepadservice.cpp
        src/cecinput.h src/cecinput.cpp
        src/cecservice.h src/cecservice.cpp
)
# Mock library (systems + games) until the real library arrives in M4.
qt_add_resources(mightyos_core "mightyos_mockdata"
    PREFIX /mightyos/mock
    BASE data/mock
    FILES data/mock/library.json)
target_include_directories(mightyos_core PUBLIC src)
target_link_libraries(mightyos_core PUBLIC Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Concurrent)
target_link_libraries(mightyos_core PRIVATE SDL3::SDL3)
mightyos_set_warnings(mightyos_core)

# ---------------------------------------------------------------------------
# mightyos_ui: all QML (module MightyOS.UI), static so the Quick Test runner
# can load the same components as the app.
# ---------------------------------------------------------------------------
qt_add_library(mightyos_ui STATIC)

set_source_files_properties(qml/Theme.qml
    PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(mightyos_ui
    URI MightyOS.UI
    VERSION 1.0
    DEPENDENCIES MightyOS.Core
    QML_FILES
        qml/Main.qml
        qml/Theme.qml
        qml/Icons.js
        qml/Icon.qml
        qml/FocusRing.qml
        qml/PointerArea.qml
        qml/WheelStepper.qml
        qml/Tile.qml
        qml/TileRow.qml
        qml/TileGrid.qml
        qml/CategoryBar.qml
        qml/InfoHeader.qml
        qml/PromptBar.qml
        qml/ButtonGlyph.qml
        qml/Glyphs.js
        qml/Breadcrumb.qml
        qml/ContextMenu.qml
        qml/Dialog.qml
        qml/Toast.qml
        qml/Clock.qml
        qml/LaunchOverlay.qml
        qml/NavScreen.qml
        qml/NavStack.qml
        qml/screens/HomeScreen.qml
        qml/screens/SystemListScreen.qml
        qml/screens/GameGridScreen.qml
        qml/screens/CategoryScreen.qml
        qml/screens/AppListScreen.qml
        qml/ProfilePicker.qml
        qml/ProfileEditor.qml
        qml/ChoiceRow.qml
        qml/ProfileChip.qml
        qml/Catalog.js
)
target_link_libraries(mightyos_ui PRIVATE Qt6::Quick mightyos_core)
mightyos_set_warnings(mightyos_ui)

# ---------------------------------------------------------------------------
# mightyos: the launcher itself
# ---------------------------------------------------------------------------
qt_add_executable(mightyos
    src/main.cpp
)
target_compile_definitions(mightyos PRIVATE
    MIGHTYOS_VERSION="${PROJECT_VERSION}"
    $<$<BOOL:${MIGHTYOS_DEV_TOOLS}>:MIGHTYOS_DEV_TOOLS>)
target_link_libraries(mightyos PRIVATE
    Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick
    mightyos_core mightyos_coreplugin
    mightyos_ui mightyos_uiplugin)
mightyos_set_warnings(mightyos)

# ---------------------------------------------------------------------------
# mightyos-testapp: tiny GUI program, the launch target for mock games until
# the real library (M4) and for the tests. Dev/test only.
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
# mightyos-inputtest: CLI, no GUI. Prints controllers and the actions they
# produce (hands-on testing over SSH).
# ---------------------------------------------------------------------------
qt_add_executable(mightyos-inputtest
    tools/inputtest.cpp
)
target_link_libraries(mightyos-inputtest PRIVATE Qt6::Core mightyos_core)
target_compile_definitions(mightyos-inputtest PRIVATE $<$<BOOL:${MIGHTYOS_DEV_TOOLS}>:MIGHTYOS_DEV_TOOLS>)
mightyos_set_warnings(mightyos-inputtest)


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
option(MIGHTYOS_BUILD_TESTS "Build unit tests" ON)
if(MIGHTYOS_BUILD_TESTS)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Test QuickTest)
    enable_testing()

    # Tests never touch the real XDG dirs (~/.config, ~/.local/share,
    # ~/.local/state, ~/.cache), and every run starts from empty ones (the
    # fixture below runs first). XDG_DATA_DIRS points at an empty dir too,
    # so the system's .desktop files don't leak into the results.
    set(_test_xdg "${CMAKE_BINARY_DIR}/test-xdg")
    set(_test_env
        "QT_QPA_PLATFORM=offscreen"
        "XDG_STATE_HOME=${_test_xdg}/state"
        "XDG_CONFIG_HOME=${_test_xdg}/config"
        "XDG_DATA_HOME=${_test_xdg}/data"
        "XDG_CACHE_HOME=${_test_xdg}/cache"
        "XDG_DATA_DIRS=${_test_xdg}/system"
        "XDG_CURRENT_DESKTOP=MightyOS")
    add_test(NAME clear_test_state COMMAND ${CMAKE_COMMAND} -E rm -rf "${_test_xdg}")
    set_tests_properties(clear_test_state PROPERTIES FIXTURES_SETUP test_state)

    function(mightyos_add_test name)
        cmake_parse_arguments(ARG "" "" "LIBS" ${ARGN})
        qt_add_executable(${name} tests/${name}.cpp)
        target_link_libraries(${name} PRIVATE mightyos_core Qt6::Test ${ARG_LIBS})
        mightyos_set_warnings(${name})
        add_test(NAME ${name} COMMAND ${name})
        set_tests_properties(${name} PROPERTIES ENVIRONMENT "${_test_env}"
                                               FIXTURES_REQUIRED test_state)
    endfunction()

    mightyos_add_test(tst_launcher)
    add_dependencies(tst_launcher mightyos-testapp)
    target_compile_definitions(tst_launcher PRIVATE
        MIGHTYOS_TESTAPP_PATH="$<TARGET_FILE:mightyos-testapp>")

    mightyos_add_test(tst_inputrouter LIBS Qt6::Quick)
    mightyos_add_test(tst_navstate)
    mightyos_add_test(tst_tilelistmodel)
    mightyos_add_test(tst_desktopentry)
    mightyos_add_test(tst_appregistry)
    mightyos_add_test(tst_profiles)
    mightyos_add_test(tst_favorites)
    mightyos_add_test(tst_artwork LIBS Qt6::Quick)
    mightyos_add_test(tst_padinput)
    mightyos_add_test(tst_padhub)
    mightyos_add_test(tst_gamepadservice LIBS SDL3::SDL3)
    mightyos_add_test(tst_cec)
    mightyos_add_test(tst_settings)

    # Qt Quick Test: components, NavStack and the whole Main window
    # (tests/qml/tst_*.qml). tst_main launches the test app.
    qt_add_executable(tst_qml tests/tst_qml.cpp)
    add_dependencies(tst_qml mightyos-testapp)
    target_link_libraries(tst_qml PRIVATE
        Qt6::Quick Qt6::QuickTest
        mightyos_core mightyos_coreplugin
        mightyos_ui mightyos_uiplugin)
    target_compile_definitions(tst_qml PRIVATE
        QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/qml"
        MIGHTYOS_TESTAPP_PATH="$<TARGET_FILE:mightyos-testapp>")
    mightyos_set_warnings(tst_qml)
    add_test(NAME tst_qml COMMAND tst_qml)
    set_tests_properties(tst_qml PROPERTIES ENVIRONMENT "${_test_env}"
                                            FIXTURES_REQUIRED test_state)
endif()
