cmake_minimum_required(VERSION 3.21)

# V5.4.13 — the version is read out of src/appversion.h rather than typed here
# as well. This said 5.0.0 while the app said 5.4.12, which is the same drift
# the window title had: every place the number is written by hand is a place
# it can be forgotten. appversion.h is now the ONLY place to change it.
# V5.4.26 — and CMake is told to re-run when that header changes. file(READ)
# creates no dependency on its own, so bumping the version and rebuilding gave
# an .exe whose window title said the new number while the version RESOURCE
# stamped into it still said the old one — a build configured before the bump
# never reconfigures. Exactly the drift the note above is about.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/src/appversion.h")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/appversion.h" _appversion_header)
string(REGEX MATCH "MC_APP_VERSION[ \t]+\"([0-9]+)\\.([0-9]+)\\.([0-9]+)\""
       _appversion_match "${_appversion_header}")
if(NOT _appversion_match)
    message(FATAL_ERROR "Could not read MC_APP_VERSION from src/appversion.h")
endif()
set(MC_VERSION "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
message(STATUS "Media Countdowns version ${MC_VERSION} (from src/appversion.h)")

project(MediaCountdowns VERSION ${MC_VERSION} LANGUAGES CXX RC)

# windres is handed every include directory, and it forwards them to cc1
# unquoted — so "C:/Users/.../Media Countdowns/src" arrives as two arguments and
# the compile fails with "Countdowns/src: No such file or directory". The icon
# resource includes no headers, so it is compiled without include paths at all.
set(CMAKE_RC_COMPILE_OBJECT "<CMAKE_RC_COMPILER> -O coff <DEFINES> <SOURCE> <OBJECT>")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network)
qt_standard_project_setup()

set(MAIN_SOURCES
    src/main.cpp
    src/mainwindow.cpp        src/mainwindow.h
    src/tilewidget.cpp        src/tilewidget.h
    src/tileoutline.h
    src/addtiledialog.cpp     src/addtiledialog.h
    src/outlinedlabel.h
    src/countdownwidget.h
    src/colorwheel.h
    src/applogger.h
    src/customtiledialog.cpp  src/customtiledialog.h
    src/edittiledialog.cpp    src/edittiledialog.h
    src/aboutdialog.cpp       src/aboutdialog.h
    src/tmdbscraper.cpp       src/tmdbscraper.h
    src/tvmazescraper.cpp     src/tvmazescraper.h
    src/mediasource.h
    src/numerals.h
    src/loopschedule.h
    src/appversion.h
    src/feedback.h
    src/showoverrides.h
    src/releasehistory.h
    src/imagequeue.h
    src/igdbscraper.cpp       src/igdbscraper.h
    src/igdbcredentials.h
    src/jsonmanager.cpp       src/jsonmanager.h
    resources/resources.qrc
    # V5.4.18 — the application icon, compiled into the exe by windres rather
    # than injected afterwards with rcedit. A build step that lives outside the
    # build is one somebody forgets: every rebuild silently dropped the icon
    # until it was re-injected by hand. IDI_ICON1 is the lowest-numbered icon
    # resource, which is the one Windows Explorer shows for the executable.
    appicon.rc
    # V5.4.26 — and the version resource, generated from src/appversion.h so
    # the number cannot be typed twice. Without it the .exe had no version at
    # all in Explorer's Properties → Details, which is the first place anyone
    # looks to answer "which build is this?".
    ${CMAKE_CURRENT_BINARY_DIR}/version.rc
)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/version.rc.in
               ${CMAKE_CURRENT_BINARY_DIR}/version.rc @ONLY)

qt_add_executable(MediaCountdowns WIN32
    ${MAIN_SOURCES}
    mainwindow.ui
)

target_include_directories(MediaCountdowns PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(MediaCountdowns PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
)

install(TARGETS MediaCountdowns RUNTIME DESTINATION bin)
