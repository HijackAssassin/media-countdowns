cmake_minimum_required(VERSION 3.21)
# The version lives in src/notifierversion.h, parsed here so the two can never
# disagree - the same arrangement the main app uses.
# V5.4.26 — same as the main app's CMakeLists: file(READ) creates no
# dependency, so a build configured before a version bump never reconfigures
# and stamps the OLD number into the .exe's version resource.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/src/notifierversion.h")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/notifierversion.h" _notifier_version_header)
string(REGEX MATCH "MC_NOTIFIER_VERSION[ \t]+\"([0-9]+)\\.([0-9]+)\\.([0-9]+)\""
       _notifier_version_match "${_notifier_version_header}")
if(NOT _notifier_version_match)
    message(FATAL_ERROR "Could not read MC_NOTIFIER_VERSION from src/notifierversion.h")
endif()
set(MC_NOTIFIER_VERSION "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
message(STATUS "Media Countdowns Notifier version ${MC_NOTIFIER_VERSION}")

project(MediaCountdownsNotifier VERSION ${MC_NOTIFIER_VERSION} LANGUAGES CXX RC)

# windres hands every include directory to the preprocessor unquoted, so the
# space in "Media Countdowns Notifier" splits the path in two and the compile
# fails. The icon resource includes no headers, so it is built without them.
# (Same fix as the main app's CMakeLists.)
set(CMAKE_RC_COMPILE_OBJECT "<CMAKE_RC_COMPILER> -O coff <DEFINES> <SOURCE> <OBJECT>")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network)
qt_standard_project_setup()

set(TRAY_SOURCES
    src/main.cpp
    src/trayapp.cpp     src/trayapp.h
    src/jsonmanager.cpp src/jsonmanager.h
    src/tiledata.h
    src/loopschedule.h
    src/notifierversion.h
    # The same icon as the main app — they are one product, and a tray program
    # with a default Qt icon looks like something else running on the machine.
    appicon.rc
    resources.qrc      # the same icon again, this time reachable at runtime
    # V5.4.26 — the version resource. This program has no window to put a
    # number in, so before this the version existed only in CMake: nothing
    # included notifierversion.h and the .exe carried no version at all, which
    # is precisely the "can't tie a bug report to a build" problem V5.4.25 was
    # meant to solve. Properties → Details now names it, and so does the tray
    # tooltip.
    ${CMAKE_CURRENT_BINARY_DIR}/version.rc
)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/version.rc.in
               ${CMAKE_CURRENT_BINARY_DIR}/version.rc @ONLY)

qt_add_executable(MediaCountdownsNotifier WIN32
    ${TRAY_SOURCES}
)

target_include_directories(MediaCountdownsNotifier PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(MediaCountdownsNotifier PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
)

# Windows Shell/COM APIs needed for startup shortcut creation (IShellLink)
if(WIN32)
    target_link_libraries(MediaCountdownsNotifier PRIVATE Ole32 Shell32)
endif()

install(TARGETS MediaCountdownsNotifier RUNTIME DESTINATION bin)
