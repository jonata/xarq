cmake_minimum_required(VERSION 3.16)
project(xarq LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# The version the app shows. The release builds pass the tag's (without the leading "v").
set(XARQ_VERSION "26.10-alpha" CACHE STRING "Version shown by the app")

find_package(Qt6 REQUIRED COMPONENTS Widgets Svg PrintSupport)
find_package(ZLIB REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(HARFBUZZ REQUIRED harfbuzz)

# Optional: CorelDRAW (.cdr/.cmx) vector import via libcdr + librevenge (Document Liberation Project).
# When present, .cdr files import their real editable vectors; otherwise .cdr falls back to the embedded
# preview bitmap. Install libcdr-dev + librevenge-dev to enable.
pkg_check_modules(LIBCDR libcdr-0.1 librevenge-0.0 librevenge-stream-0.0 librevenge-generators-0.0)

# Optional: the DLP sibling vector formats — WordPerfect Graphics, Visio, FreeHand, Publisher — via the
# same librevenge SVG bridge (see extvector.cpp). Each is independently optional; install the matching
# -dev package (libwpg-dev / libvisio-dev / libfreehand-dev / libmspub-dev) to enable it.
pkg_check_modules(LIBWPG      libwpg-0.3      librevenge-0.0 librevenge-stream-0.0 librevenge-generators-0.0)
pkg_check_modules(LIBVISIO    libvisio-0.1    librevenge-0.0 librevenge-stream-0.0 librevenge-generators-0.0)
pkg_check_modules(LIBFREEHAND libfreehand-0.1 librevenge-0.0 librevenge-stream-0.0 librevenge-generators-0.0)
pkg_check_modules(LIBMSPUB    libmspub-0.1    librevenge-0.0 librevenge-stream-0.0 librevenge-generators-0.0)
# More DLP siblings — PageMaker, Zoner Draw, QuarkXPress, StarOffice (StarDraw/Impress), Apple iWork
# (Keynote), legacy Mac (MacDraw/ClarisDraw) — same librevenge SVG bridge (drawing + presentation).
pkg_check_modules(LIBPAGEMAKER  libpagemaker-0.0  librevenge-0.0 librevenge-stream-0.0 librevenge-generators-0.0)
pkg_check_modules(LIBZMF        libzmf-0.0        librevenge-0.0 librevenge-stream-0.0 librevenge-generators-0.0)
pkg_check_modules(LIBQXP        libqxp-0.0        librevenge-0.0 librevenge-stream-0.0 librevenge-generators-0.0)
pkg_check_modules(LIBSTAROFFICE libstaroffice-0.0 librevenge-0.0 librevenge-stream-0.0 librevenge-generators-0.0)
pkg_check_modules(LIBETONYEK    libetonyek-0.1    librevenge-0.0 librevenge-stream-0.0 librevenge-generators-0.0)
pkg_check_modules(LIBMWAW       libmwaw-0.3       librevenge-0.0 librevenge-stream-0.0 librevenge-generators-0.0)

# Optional: JPEG XR / HD Photo (.jxr/.wdp/.hdp) via Microsoft's reference jxrlib (jxrglue). Install
# libjxr-dev to enable; otherwise jxr.cpp compiles to a stub and the format is simply not offered.
pkg_check_modules(LIBJXR libjxr)

# Optional: Zstandard — Affinity container v11 (#FT4, Affinity 2.x) compresses doc.dat and its raster
# tiles with zstd. Without it those files decline to the embedded preview; v9/v10 files are unaffected.
pkg_check_modules(LIBZSTD libzstd)

# Optional: DICOM medical images via GDCM (find_package, no .pc) and WMF metafiles via libwmf.
find_package(GDCM QUIET)
pkg_check_modules(LIBWMF libwmf)
# Optional: whole-slide microscopy (.svs/.ndpi/.scn/.mrxs…) via OpenSlide.
pkg_check_modules(OPENSLIDE openslide)
# Optional: GDAL/OGR (geospatial vector cluster: DGN/MapInfo/GeoPackage/S-57/GML…) + Graphviz (DOT).
pkg_check_modules(GDAL gdal)
pkg_check_modules(GRAPHVIZ libgvc libcgraph)
# Optional: AutoCAD DWG via GNU LibreDWG (not in apt; build from source + `make install`, or drop the shared
# lib + dwg.h/dwg_api.h under ~/.local). Detected by header+lib so no PKG_CONFIG_PATH env is required.
find_path(LIBREDWG_INCLUDE_DIR dwg.h HINTS $ENV{HOME}/.local/include /usr/local/include)
find_library(LIBREDWG_LIBRARY NAMES redwg HINTS $ENV{HOME}/.local/lib /usr/local/lib)

add_executable(xarq
    main.cpp
    dialogchrome.cpp
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
    rasterextra.cpp
    vecextra.cpp
    gisvector.cpp
    dwgvector.cpp
    affvector.cpp
    fxgvector.cpp
    pxdimport.cpp
    cptimport.cpp
    xcfimport.cpp
    kraimport.cpp
    procreateimport.cpp
    sqlitemini.cpp
    clipimport.cpp
    pspimport.cpp
    xdvector.cpp
    pdfvector.cpp
    psdimport.cpp
    extcodecs.cpp
    exr.cpp
    jxr.cpp
    dicom.cpp
    wmf.cpp
    slide.cpp
    qr.cpp
    otshape.cpp
    cdrvector.cpp
    extvector.cpp
    startscreen.cpp
    splash.cpp
    icons.cpp
    pathedit.cpp
    xar.cpp
    selftest.cpp
    resources.qrc
)
# The self-test is one enormous function: GCC 13 at -O3 takes close to an hour over it, and it gains nothing.
set_source_files_properties(selftest.cpp PROPERTIES COMPILE_OPTIONS "-O0")
# canvas.cpp #includes canvas_input.inc / canvas_io.inc
set_source_files_properties(canvas.cpp PROPERTIES OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/canvas_input.inc;${CMAKE_CURRENT_SOURCE_DIR}/canvas_io.inc")

target_link_libraries(xarq PRIVATE Qt6::Widgets Qt6::Svg Qt6::PrintSupport ZLIB::ZLIB ${CMAKE_DL_LIBS} ${HARFBUZZ_LIBRARIES})
target_include_directories(xarq PRIVATE ${HARFBUZZ_INCLUDE_DIRS})
target_compile_definitions(xarq PRIVATE XARQ_VERSION="${XARQ_VERSION}")

# Windows: a windowed app with its icon, and dlopen's stand-in. MinGW's default object format runs out of
# sections on the largest sources.
if(WIN32)
    enable_language(RC)
    # MSYS2's GCC 16 at -O3 makes the fractal fill differ from call to call in xar.cpp (cause not found; the same
    # generator alone is stable). The self-test's determinism check guards this.
    set_source_files_properties(xar.cpp PROPERTIES COMPILE_OPTIONS "-O1")
    set_target_properties(xarq PROPERTIES WIN32_EXECUTABLE TRUE)
    target_sources(xarq PRIVATE dlcompat.cpp xarq.rc)
    if(MINGW)
        target_compile_options(xarq PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wa,-mbig-obj>)
    endif()
endif()

if(LIBCDR_FOUND)
    target_compile_definitions(xarq PRIVATE HAVE_LIBCDR)
    target_include_directories(xarq PRIVATE ${LIBCDR_INCLUDE_DIRS})
    target_link_directories(xarq PRIVATE ${LIBCDR_LIBRARY_DIRS})
    target_link_libraries(xarq PRIVATE ${LIBCDR_LIBRARIES})
    message(STATUS "CorelDRAW vector import: ENABLED (libcdr found)")
else()
    message(STATUS "CorelDRAW vector import: disabled — .cdr uses the preview-bitmap fallback "
                   "(install libcdr-dev + librevenge-dev to enable full vector import)")
endif()

# DLP sibling vector formats — enable each independently when its -dev package was found. The pkg prefix
# (LIBWPG…) is also the HAVE_ define suffix that extvector.cpp keys off.
foreach(dlp LIBWPG:libwpg-dev:WordPerfect\ Graphics LIBVISIO:libvisio-dev:Visio LIBFREEHAND:libfreehand-dev:FreeHand LIBMSPUB:libmspub-dev:Publisher LIBPAGEMAKER:libpagemaker-dev:PageMaker LIBZMF:libzmf-dev:Zoner\ Draw LIBQXP:libqxp-dev:QuarkXPress LIBSTAROFFICE:libstaroffice-dev:StarOffice LIBETONYEK:libetonyek-dev:Apple\ iWork LIBMWAW:libmwaw-dev:legacy\ Mac)
    string(REPLACE ":" ";" _parts "${dlp}")
    list(GET _parts 0 _var)
    list(GET _parts 1 _pkg)
    list(GET _parts 2 _label)
    if(${_var}_FOUND)
        target_compile_definitions(xarq PRIVATE HAVE_${_var})
        target_include_directories(xarq PRIVATE ${${_var}_INCLUDE_DIRS})
        target_link_directories(xarq PRIVATE ${${_var}_LIBRARY_DIRS})
        target_link_libraries(xarq PRIVATE ${${_var}_LIBRARIES})
        message(STATUS "${_label} vector import: ENABLED")
    else()
        message(STATUS "${_label} vector import: disabled (install ${_pkg} to enable)")
    endif()
endforeach()

# Poppler for PDF import. Its core headers are its private API, and pdfvector.cpp is written against the
# 26.x one, so an older poppler means no PDF route rather than a failed build.
pkg_check_modules(POPPLER poppler)
if(POPPLER_FOUND AND POPPLER_VERSION VERSION_GREATER_EQUAL "26.1" AND EXISTS "${POPPLER_INCLUDEDIR}/poppler/SplashOutputDev.h")
    set_source_files_properties(pdfvector.cpp PROPERTIES COMPILE_DEFINITIONS "HAVE_POPPLER" COMPILE_OPTIONS "-std=c++20" INCLUDE_DIRECTORIES "${POPPLER_INCLUDEDIR}/poppler")
    target_link_directories(xarq PRIVATE ${POPPLER_LIBRARY_DIRS})
    target_link_libraries(xarq PRIVATE ${POPPLER_LIBRARIES})
endif()

# Zstandard for the Affinity reader. Scope HAVE_ZSTD + its includes to affvector.cpp only.
if(LIBZSTD_FOUND)
    target_link_directories(xarq PRIVATE ${LIBZSTD_LIBRARY_DIRS})
    target_link_libraries(xarq PRIVATE ${LIBZSTD_LIBRARIES})
    if(LIBZSTD_INCLUDE_DIRS)
        target_include_directories(xarq PRIVATE ${LIBZSTD_INCLUDE_DIRS})
    endif()
    set_source_files_properties(affvector.cpp PROPERTIES COMPILE_DEFINITIONS "HAVE_ZSTD")
    message(STATUS "Affinity vector import: ENABLED with zstd (container v11 / Affinity 2.x supported)")
else()
    message(STATUS "Affinity vector import: ENABLED without zstd — v11 files fall back to the preview "
                   "(install libzstd-dev to enable them)")
endif()

# JPEG XR — link jxrlib and scope its headers/defines (-D__ANSI__ etc.) to jxr.cpp only, so the
# jxrlib compatibility macros never leak into the rest of the build.
if(LIBJXR_FOUND)
    target_link_directories(xarq PRIVATE ${LIBJXR_LIBRARY_DIRS})
    target_link_libraries(xarq PRIVATE ${LIBJXR_LIBRARIES})
    set_source_files_properties(jxr.cpp PROPERTIES
        COMPILE_DEFINITIONS "HAVE_JXR;__ANSI__;DISABLE_PERF_MEASUREMENT"
        INCLUDE_DIRECTORIES "${LIBJXR_INCLUDE_DIRS}")
    message(STATUS "JPEG XR import: ENABLED (libjxr found)")
else()
    message(STATUS "JPEG XR import: disabled (install libjxr-dev to enable)")
endif()

# DICOM via GDCM (C++). Scope HAVE_GDCM + its includes to dicom.cpp; link the core GDCM libraries.
if(GDCM_FOUND)
    set_source_files_properties(dicom.cpp PROPERTIES
        COMPILE_DEFINITIONS "HAVE_GDCM"
        INCLUDE_DIRECTORIES "${GDCM_INCLUDE_DIRS}")
    target_link_libraries(xarq PRIVATE gdcmMSFF gdcmDSED gdcmIOD gdcmDICT gdcmCommon)
    message(STATUS "DICOM import: ENABLED (GDCM found)")
else()
    message(STATUS "DICOM import: disabled (install libgdcm-dev to enable)")
endif()

# WMF via libwmf. Scope HAVE_WMF + its includes/flags to wmf.cpp.
if(LIBWMF_FOUND)
    target_link_directories(xarq PRIVATE ${LIBWMF_LIBRARY_DIRS})
    target_link_libraries(xarq PRIVATE ${LIBWMF_LIBRARIES})
    set_source_files_properties(wmf.cpp PROPERTIES
        COMPILE_DEFINITIONS "HAVE_WMF"
        INCLUDE_DIRECTORIES "${LIBWMF_INCLUDE_DIRS}")
    message(STATUS "WMF import: ENABLED (libwmf found)")
else()
    message(STATUS "WMF import: disabled (install libwmf-dev to enable)")
endif()

# Whole-slide microscopy via OpenSlide. Scope HAVE_OPENSLIDE + its includes to slide.cpp.
if(OPENSLIDE_FOUND)
    target_link_directories(xarq PRIVATE ${OPENSLIDE_LIBRARY_DIRS})
    target_link_libraries(xarq PRIVATE ${OPENSLIDE_LIBRARIES})
    # A file's own include path isn't filtered as a target's is, and the compiler's own directory there breaks
    # the standard library's #include_next (OpenSlide and Graphviz give it on Windows).
    list(REMOVE_ITEM OPENSLIDE_INCLUDE_DIRS ${CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES})
    set_source_files_properties(slide.cpp PROPERTIES
        COMPILE_DEFINITIONS "HAVE_OPENSLIDE"
        INCLUDE_DIRECTORIES "${OPENSLIDE_INCLUDE_DIRS}")
    message(STATUS "Whole-slide import: ENABLED (OpenSlide found)")
else()
    message(STATUS "Whole-slide import: disabled (install libopenslide-dev to enable)")
endif()

# GDAL/OGR + Graphviz vector import — both isolated to gisvector.cpp. Accumulate whichever are found into
# a single set_source_files_properties (the property is set once per file, so combine defs + include dirs).
set(GIS_DEFS "")
set(GIS_INCS "")
if(GDAL_FOUND)
    target_link_directories(xarq PRIVATE ${GDAL_LIBRARY_DIRS})
    target_link_libraries(xarq PRIVATE ${GDAL_LIBRARIES})
    list(APPEND GIS_DEFS HAVE_GDAL)
    list(APPEND GIS_INCS ${GDAL_INCLUDE_DIRS})
    message(STATUS "GDAL/OGR vector import: ENABLED (DGN/MapInfo/GeoPackage/S-57/GML…)")
else()
    message(STATUS "GDAL/OGR vector import: disabled (install libgdal-dev to enable)")
endif()
if(GRAPHVIZ_FOUND)
    target_link_directories(xarq PRIVATE ${GRAPHVIZ_LIBRARY_DIRS})
    target_link_libraries(xarq PRIVATE ${GRAPHVIZ_LIBRARIES})
    # The dot layout and core (SVG) plugins are PRELOADED by gisvector.cpp rather than discovered through
    # the system's plugin registry, so link them directly. They live in graphviz's own plugin directory.
    find_library(GV_DOT_LAYOUT gvplugin_dot_layout PATHS ${GRAPHVIZ_LIBRARY_DIRS} PATH_SUFFIXES graphviz NO_DEFAULT_PATH)
    find_library(GV_CORE       gvplugin_core       PATHS ${GRAPHVIZ_LIBRARY_DIRS} PATH_SUFFIXES graphviz NO_DEFAULT_PATH)
    if(HAIKU)
        # gisvector.cpp uses the system plugin registry there, so nothing to link.
    elseif(GV_DOT_LAYOUT AND GV_CORE)
        target_link_libraries(xarq PRIVATE ${GV_DOT_LAYOUT} ${GV_CORE})
        get_filename_component(GV_PLUGIN_DIR ${GV_CORE} DIRECTORY)
        set_property(TARGET xarq APPEND PROPERTY BUILD_RPATH ${GV_PLUGIN_DIR})
    else()
        message(WARNING "graphviz plugins not found — DOT import will rely on the system plugin registry")
    endif()
    list(APPEND GIS_DEFS HAVE_GRAPHVIZ)
    list(APPEND GIS_INCS ${GRAPHVIZ_INCLUDE_DIRS})
    message(STATUS "Graphviz DOT import: ENABLED")
else()
    message(STATUS "Graphviz DOT import: disabled (install libgraphviz-dev to enable)")
endif()
if(GIS_DEFS)
    if(GIS_INCS)
        list(REMOVE_ITEM GIS_INCS ${CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES})
    endif()
    set_source_files_properties(gisvector.cpp PROPERTIES
        COMPILE_DEFINITIONS "${GIS_DEFS}"
        INCLUDE_DIRECTORIES "${GIS_INCS}")
endif()

# AutoCAD DWG via LibreDWG — isolated to dwgvector.cpp. Link the shared lib and bake its directory into the
# build RPATH so the runtime loader finds it when it lives outside the default search path (e.g. ~/.local/lib).
if(LIBREDWG_INCLUDE_DIR AND LIBREDWG_LIBRARY)
    set_source_files_properties(dwgvector.cpp PROPERTIES
        COMPILE_DEFINITIONS "HAVE_LIBREDWG"
        INCLUDE_DIRECTORIES "${LIBREDWG_INCLUDE_DIR}")
    target_link_libraries(xarq PRIVATE ${LIBREDWG_LIBRARY})
    get_filename_component(LIBREDWG_LIBDIR "${LIBREDWG_LIBRARY}" DIRECTORY)
    set_property(TARGET xarq APPEND PROPERTY BUILD_RPATH "${LIBREDWG_LIBDIR}")
    message(STATUS "AutoCAD DWG import (LibreDWG): ENABLED (${LIBREDWG_LIBRARY})")
else()
    message(STATUS "AutoCAD DWG import (LibreDWG): disabled (build libredwg + place under ~/.local or /usr/local)")
endif()

# Haiku keeps an application's signature, version and vector icon as resources inside the executable.
if(HAIKU)
    set(XARQ_RDEF "${CMAKE_CURRENT_SOURCE_DIR}/haiku/XarQ.rdef")
    set(XARQ_RSRC "${CMAKE_CURRENT_BINARY_DIR}/XarQ.rsrc")
    add_custom_command(OUTPUT "${XARQ_RSRC}" COMMAND rc -o "${XARQ_RSRC}" "${XARQ_RDEF}" DEPENDS "${XARQ_RDEF}" VERBATIM)
    add_custom_target(xarq_resources DEPENDS "${XARQ_RSRC}")
    add_dependencies(xarq xarq_resources)
    add_custom_command(TARGET xarq POST_BUILD
        COMMAND xres -o "$<TARGET_FILE:xarq>" "${XARQ_RSRC}"
        COMMAND mimeset -f "$<TARGET_FILE:xarq>" VERBATIM)
endif()
