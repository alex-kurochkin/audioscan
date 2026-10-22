cmake_minimum_required(VERSION 3.16)
project(audioscout VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Multimedia Sql Concurrent)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SNDFFILE REQUIRED sndfile)
pkg_check_modules(TAGLIB REQUIRED taglib)

add_executable(audioscout
    src/main.cpp
    src/MainWindow.cpp
    src/AudioView.cpp
    src/AudioFile.cpp
    src/AudioPlayer.cpp
    src/FileTableModel.cpp
    src/PreviewWorker.cpp
    src/Renderer.cpp
    src/Settings.cpp
    src/Database.cpp
    src/UndoStack.cpp
    src/MarkerDialog.cpp
    src/SettingsDialog.cpp
    src/DirScanner.cpp
    src/FfmpegFallbackController.cpp
    src/PlayerController.cpp
    src/ColumnLayoutController.cpp
    src/ProcessSink.cpp
)

target_include_directories(audioscout PRIVATE
    ${SNDFFILE_INCLUDE_DIRS} ${TAGLIB_INCLUDE_DIRS})

target_link_libraries(audioscout PRIVATE
    Qt6::Core Qt6::Widgets Qt6::Multimedia Qt6::Sql Qt6::Concurrent
    ${SNDFFILE_LIBRARIES} ${TAGLIB_LIBRARIES})

install(TARGETS audioscout RUNTIME DESTINATION bin)
install(FILES resources/audioscout.desktop DESTINATION share/applications)

option(AUDIOSCOUT_TESTING "Enable in-process test hooks (dev builds only)" OFF)
if(AUDIOSCOUT_TESTING)
    target_compile_definitions(audioscout PRIVATE AUDIOSCOUT_TESTING)
endif()
