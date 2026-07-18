cmake_minimum_required(VERSION 3.16)
project(xarq LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Svg PrintSupport)
find_package(ZLIB REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(HARFBUZZ REQUIRED harfbuzz)

add_executable(xarq
    main.cpp
    mainwindow.cpp
    canvas.cpp
    model.cpp
    render.cpp
    panels.cpp
    dialogs.cpp
    galleries.cpp
    gif.cpp
    spell.cpp
    exif.cpp
    hyphen.cpp
    svgimport.cpp
    svgexport.cpp
    prepress.cpp
    trace.cpp
    photoops.cpp
    icc.cpp
    raw.cpp
    qr.cpp
    otshape.cpp
    startscreen.cpp
    splash.cpp
    icons.cpp
    pathedit.cpp
    xar.cpp
    selftest.cpp
    resources.qrc
)
# canvas.cpp #includes canvas_input.inc / canvas_io.inc
set_source_files_properties(canvas.cpp PROPERTIES OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/canvas_input.inc;${CMAKE_CURRENT_SOURCE_DIR}/canvas_io.inc")

target_link_libraries(xarq PRIVATE Qt6::Widgets Qt6::Svg Qt6::PrintSupport ZLIB::ZLIB ${CMAKE_DL_LIBS} ${HARFBUZZ_LIBRARIES})
target_include_directories(xarq PRIVATE ${HARFBUZZ_INCLUDE_DIRS})
