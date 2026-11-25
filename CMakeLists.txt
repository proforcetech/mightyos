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

# Sql: the ROM library cache (Ubuntu: libqt6sql6-sqlite provides the
# QSQLITE driver at run time). Network: SteamGridDB artwork.
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Qml Quick Concurrent Sql Network)
include(GNUInstallDirs)
# Where the session's labwc config is installed (--check-session checks it).
set(MIGHTYOS_INSTALLED_LABWC_DIR "${CMAKE_INSTALL_FULL_DATADIR}/mightyos/labwc")
# Development entry points that feed input from outside the session
# (--fake-cec for mightyos and mightyos-inputtest). ON while MightyOS is in
# development; packaged builds (M6) must set it OFF.
option(MIGHTYOS_DEV_TOOLS "Build development-only input options (--fake-cec)" ON)

# Gamepads (PLAN D6). Ubuntu: libsdl3-dev.
find_package(SDL3 3.2 REQUIRED CONFIG COMPONENTS SDL3-shared)

# Optional: the SteamGridDB API key in the Secret Service (PLAN D11).
# Ubuntu: libsecret-1-dev. Without it the key is a 0600 file.
option(MIGHTYOS_WITH_LIBSECRET "Store API keys with libsecret when available" ON)
if(MIGHTYOS_WITH_LIBSECRET)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBSECRET QUIET IMPORTED_TARGET libsecret-1)
    endif()
endif()
if(LIBSECRET_FOUND)
    message(STATUS "libsecret ${LIBSECRET_VERSION}: API keys go to the Secret Service")
else()
    message(STATUS "libsecret not found: API keys are stored in a 0600 file")
endif()
# Optional: the Guide overlay as a wlr-layer-shell window over running apps
# (PLAN M6). Ubuntu: liblayershellqtinterface-dev (pulls layer-shell-qt).
# Without it the Guide menu is drawn in MightyOS's own window, which a
# fullscreen app covers.
option(MIGHTYOS_WITH_LAYERSHELL "Guide overlay via LayerShellQt (wlr-layer-shell)" ON)
if(MIGHTYOS_WITH_LAYERSHELL)
    find_package(LayerShellQt 6 CONFIG QUIET)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(WAYLAND_CLIENT QUIET IMPORTED_TARGET wayland-client)
    endif()
endif()
if(LayerShellQt_FOUND AND WAYLAND_CLIENT_FOUND)
    set(MIGHTYOS_HAVE_LAYERSHELL ON)
    message(STATUS "LayerShellQt ${LayerShellQt_VERSION}: Guide overlay on the wlr-layer-shell overlay layer")
else()
    set(MIGHTYOS_HAVE_LAYERSHELL OFF)
    message(STATUS "LayerShellQt not found (liblayershellqtinterface-dev): in-window Guide menu only")
endif()
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
        src/keyvalues.h src/keyvalues.cpp
        src/systemregistry.h src/systemregistry.cpp
        src/romscanner.h src/romscanner.cpp
        src/librarycache.h src/librarycache.cpp
        src/steamlibrary.h src/steamlibrary.cpp
        src/steamtracker.h src/steamtracker.cpp
        src/emulatorlaunch.h src/emulatorlaunch.cpp
        src/apikeystore.h src/apikeystore.cpp
        src/steamgriddb.h src/steamgriddb.cpp
        src/artworkscraper.h src/artworkscraper.cpp
        src/definitionlayers.h src/definitionlayers.cpp
        src/webapps.h src/webapps.cpp
        src/moonlight.h src/moonlight.cpp
        src/medialibrary.h src/medialibrary.cpp
        src/processhardening.h src/processhardening.cpp
        src/signalwatcher.h src/signalwatcher.cpp
        src/sessionstate.h src/sessionstate.cpp
        src/session.h src/session.cpp
        src/guideoverlayhost.h src/guideoverlayhost.cpp
)
# Mock library (systems + games) for tests and `--mock-library` dev runs.
qt_add_resources(mightyos_core "mightyos_mockdata"
    PREFIX /mightyos/mock
    BASE data/mock
    FILES data/mock/library.json)
# Built-in system definitions (the lowest systems.d layer); the same files
# are installed to share/mightyos/systems.d.
file(GLOB MIGHTYOS_SYSTEM_DEFS CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
     "${CMAKE_CURRENT_SOURCE_DIR}/data/systems.d/*.json")
qt_add_resources(mightyos_core "mightyos_systems"
    PREFIX /mightyos
    BASE data
    FILES ${MIGHTYOS_SYSTEM_DEFS})
# Built-in web-app tiles (streaming services), installed to
# share/mightyos/webapps.d like systems.d.
file(GLOB MIGHTYOS_WEBAPP_DEFS CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
     "${CMAKE_CURRENT_SOURCE_DIR}/data/webapps.d/*.json")
qt_add_resources(mightyos_core "mightyos_webapps"
    PREFIX /mightyos
    BASE data
    FILES ${MIGHTYOS_WEBAPP_DEFS})
target_include_directories(mightyos_core PUBLIC src)
target_link_libraries(mightyos_core PUBLIC Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Concurrent
                                           Qt6::Sql Qt6::Network)
target_link_libraries(mightyos_core PRIVATE SDL3::SDL3)
if(LIBSECRET_FOUND)
    target_link_libraries(mightyos_core PRIVATE PkgConfig::LIBSECRET)
    target_compile_definitions(mightyos_core PRIVATE MIGHTYOS_HAVE_LIBSECRET)
endif()
if(MIGHTYOS_HAVE_LAYERSHELL)
    target_link_libraries(mightyos_core PRIVATE LayerShellQt::Interface PkgConfig::WAYLAND_CLIENT)
    target_compile_definitions(mightyos_core PRIVATE MIGHTYOS_HAVE_LAYERSHELL)
endif()
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
        qml/GuideMenu.qml
        qml/GuideOverlay.qml
        qml/NavScreen.qml
        qml/NavStack.qml
        qml/screens/HomeScreen.qml
        qml/screens/SystemListScreen.qml
        qml/screens/GameGridScreen.qml
        qml/screens/CategoryScreen.qml
        qml/screens/AppListScreen.qml
        qml/screens/ArtworkScreen.qml
        qml/screens/MediaScreen.qml
        qml/screens/MoonlightScreen.qml
        qml/screens/MediaSettingsScreen.qml
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
    MIGHTYOS_INSTALLED_LABWC_DIR="${MIGHTYOS_INSTALLED_LABWC_DIR}"
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
# Session (PLAN M6, D5): labwc as the kiosk compositor, mightyos-session as
# the login session. The script and rc.xml are generated per target:
#  - build/session/: the build tree's binary and config (testing on a box;
#    MIGHTYOS_LABWC_DIR may point it at another config dir),
#  - build/session/install/: the installed paths, no overrides,
#  - build/session/test/: fake labwc / mightyos for tst_session.
# Paths end up inside single-quoted shell strings and labwc Execute
# commands: only a conservative character set is accepted.
# ---------------------------------------------------------------------------
set(MIGHTYOS_SESSION_LABWC "/usr/bin/labwc")
function(mightyos_session_path_ok out path)
    if(path MATCHES "^/[A-Za-z0-9/._+-]*$")
        set(${out} TRUE PARENT_SCOPE)
    else()
        set(${out} FALSE PARENT_SCOPE)
    endif()
endfunction()
function(mightyos_configure_session dir self bin labwc labwcdir delay dev)
    set(MIGHTYOS_SESSION_SELF "${self}")
    set(MIGHTYOS_SESSION_MIGHTYOS "${bin}")
    set(MIGHTYOS_SESSION_LABWC "${labwc}")
    set(MIGHTYOS_SESSION_LABWC_DIR "${labwcdir}")
    set(MIGHTYOS_SESSION_RESTART_DELAY "${delay}")
    if(dev)
        set(MIGHTYOS_SESSION_DEV_OVERRIDES "# Build tree only: test another labwc config dir.\nLABWC_DIR=\${MIGHTYOS_LABWC_DIR:-$LABWC_DIR}")
    else()
        set(MIGHTYOS_SESSION_DEV_OVERRIDES "")
    endif()
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/session/mightyos-session.in ${dir}/mightyos-session @ONLY
                   FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
                                    WORLD_READ WORLD_EXECUTE)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/session/labwc/rc.xml.in ${dir}/labwc/rc.xml @ONLY)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/session/labwc/menu.xml ${dir}/labwc/menu.xml COPYONLY)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/session/labwc/environment ${dir}/labwc/environment COPYONLY)
endfunction()

foreach(_p "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_FULL_DATADIR}")
    mightyos_session_path_ok(_ok "${_p}")
    if(NOT _ok)
        message(FATAL_ERROR "Install path '${_p}' contains characters the session files can't quote "
                            "(only A-Z a-z 0-9 / . _ + - are allowed)")
    endif()
endforeach()
mightyos_configure_session(${CMAKE_BINARY_DIR}/session/install
    "${CMAKE_INSTALL_FULL_BINDIR}/mightyos-session" "${CMAKE_INSTALL_FULL_BINDIR}/mightyos"
    "/usr/bin/labwc" "${MIGHTYOS_INSTALLED_LABWC_DIR}" 1 FALSE)
mightyos_session_path_ok(_build_ok "${CMAKE_BINARY_DIR}")
if(_build_ok)
    mightyos_configure_session(${CMAKE_BINARY_DIR}/session
        "${CMAKE_BINARY_DIR}/session/mightyos-session" "${CMAKE_BINARY_DIR}/mightyos"
        "/usr/bin/labwc" "${CMAKE_BINARY_DIR}/session/labwc" 1 TRUE)
    mightyos_configure_session(${CMAKE_BINARY_DIR}/session/test
        "${CMAKE_BINARY_DIR}/session/test/mightyos-session" "${CMAKE_BINARY_DIR}/fakebin/mightyos"
        "${CMAKE_BINARY_DIR}/fakebin/labwc" "${CMAKE_BINARY_DIR}/session/test/labwc" 0 FALSE)
else()
    message(WARNING "The build dir '${CMAKE_BINARY_DIR}' has characters the session script can't quote: "
                    "no build-tree mightyos-session")
endif()
set(MIGHTYOS_SESSION_SCRIPT "${CMAKE_INSTALL_FULL_BINDIR}/mightyos-session")
configure_file(session/mightyos.desktop.in ${CMAKE_BINARY_DIR}/session/install/mightyos.desktop @ONLY)
configure_file(session/system/apport/mightyos.in ${CMAKE_BINARY_DIR}/session/install/apport/mightyos @ONLY)

# ---------------------------------------------------------------------------
# Install: binaries, session files, the default systems.d / webapps.d.
# System-wide hardening files are only *shipped* (share/mightyos/system);
# copying them into /etc is a deliberate admin step (README "Session").
# ---------------------------------------------------------------------------
install(TARGETS mightyos RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS ${CMAKE_BINARY_DIR}/session/install/mightyos-session DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_BINARY_DIR}/session/install/mightyos.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/wayland-sessions)
install(FILES ${CMAKE_BINARY_DIR}/session/install/labwc/rc.xml session/labwc/menu.xml session/labwc/environment
        DESTINATION ${CMAKE_INSTALL_DATADIR}/mightyos/labwc)
install(DIRECTORY session/system/ DESTINATION ${CMAKE_INSTALL_DATADIR}/mightyos/system
        PATTERN "*.in" EXCLUDE)
# Copy to /etc/apport/report-ignore/ (README): Apport ignores this binary.
install(FILES ${CMAKE_BINARY_DIR}/session/install/apport/mightyos
        DESTINATION ${CMAKE_INSTALL_DATADIR}/mightyos/system/apport/report-ignore)
# D16: the CEC uaccess rule. Packagers (prefix /usr) may install it where
# udev reads it; by default it is only shipped under share/mightyos/system.
option(MIGHTYOS_INSTALL_UDEV_RULE "Install 70-mightyos-cec.rules to <prefix>/lib/udev/rules.d" OFF)
if(MIGHTYOS_INSTALL_UDEV_RULE)
    install(FILES session/system/udev/70-mightyos-cec.rules DESTINATION lib/udev/rules.d)
endif()
install(DIRECTORY data/systems.d data/webapps.d DESTINATION ${CMAKE_INSTALL_DATADIR}/mightyos
        FILES_MATCHING PATTERN "*.json")
# Optional managed browser policy (PLAN section 4): shipped, never put into
# /etc by MightyOS; copying it is an install-time step for the admin.
install(DIRECTORY data/browser-policies DESTINATION ${CMAKE_INSTALL_DATADIR}/mightyos
        FILES_MATCHING PATTERN "*.json")


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
        "XDG_CURRENT_DESKTOP=MightyOS"
        "MIGHTYOS_MOCK_LIBRARY=1")
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

    # Stand-ins for systemd-run / systemctl (scope launching without a
    # user manager, and deterministic where one exists).
    # Fakes for the session script test (labwc, mightyos) and a `flatpak`
    # that just runs its command (terminate tests), in build/fakebin.
    foreach(_fake systemd-run systemctl flatpak labwc mightyos)
        qt_add_executable(fake-${_fake} tests/fakesystemd.cpp)
        target_link_libraries(fake-${_fake} PRIVATE Qt6::Core)
        target_compile_definitions(fake-${_fake} PRIVATE FAKE_TOOL="${_fake}")
        mightyos_set_warnings(fake-${_fake})
        if(_fake MATCHES "^(flatpak|labwc|mightyos)$")
            set_target_properties(fake-${_fake} PROPERTIES OUTPUT_NAME ${_fake}
                                  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/fakebin)
        endif()
    endforeach()

    mightyos_add_test(tst_launcher)
    add_dependencies(tst_launcher mightyos-testapp fake-systemd-run fake-systemctl fake-flatpak)
    target_compile_definitions(tst_launcher PRIVATE
        MIGHTYOS_TESTAPP_PATH="$<TARGET_FILE:mightyos-testapp>"
        MIGHTYOS_FAKE_SYSTEMD_RUN="$<TARGET_FILE:fake-systemd-run>"
        MIGHTYOS_FAKE_SYSTEMCTL="$<TARGET_FILE:fake-systemctl>"
        MIGHTYOS_FAKE_FLATPAK="$<TARGET_FILE:fake-flatpak>")

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
    mightyos_add_test(tst_keyvalues)
    mightyos_add_test(tst_systems)
    mightyos_add_test(tst_romscanner)
    mightyos_add_test(tst_steam)
    mightyos_add_test(tst_library)
    mightyos_add_test(tst_scraper LIBS Qt6::Gui)
    mightyos_add_test(tst_webapps)
    mightyos_add_test(tst_moonlight)
    mightyos_add_test(tst_media)
    mightyos_add_test(tst_session LIBS Qt6::Quick)
    add_dependencies(tst_session fake-labwc fake-mightyos mightyos)
    target_compile_definitions(tst_session PRIVATE
        MIGHTYOS_BIN="$<TARGET_FILE:mightyos>"
        MIGHTYOS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        MIGHTYOS_BUILD_DIR="${CMAKE_BINARY_DIR}"
        MIGHTYOS_INSTALLED_BIN="${CMAKE_INSTALL_FULL_BINDIR}/mightyos")

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
