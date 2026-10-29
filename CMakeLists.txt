cmake_minimum_required(VERSION 3.21)
project(PhotoPrint VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Gui Concurrent Test)
find_package(ZLIB REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(MAGICKWAND REQUIRED IMPORTED_TARGET MagickWand)

find_path(CUPS_INCLUDE_DIR NAMES cups/cups.h)
find_library(CUPS_LIBRARY NAMES cups)
if(NOT CUPS_INCLUDE_DIR OR NOT CUPS_LIBRARY)
  message(FATAL_ERROR "libcups development files are required.")
endif()

find_path(LCMS2_INCLUDE_DIR NAMES lcms2.h)
find_library(LCMS2_LIBRARY NAMES lcms2)
if(NOT LCMS2_INCLUDE_DIR OR NOT LCMS2_LIBRARY)
  message(FATAL_ERROR "LittleCMS2 (lcms2) development files are required.")
endif()

add_library(photoprint_core STATIC
  src/Document.cpp
  src/LayoutEngine.cpp
  src/PhotoItem.cpp
  src/CanvasController.cpp
  src/PagesPanel.cpp
  src/ProjectStore.cpp
  src/RecoveryStore.cpp
  src/ToneEngine.cpp
  src/JobJournal.cpp
  src/CupsBackend.cpp
  src/JobController.cpp
  src/TrialSheetBuilder.cpp
  src/PaperPresetStore.cpp
  src/PaperCatalog.cpp
  src/PaperGeometry.cpp
  src/LegacyPpdReader.cpp
  src/PrinterCatalog.cpp
  src/PrinterAdapter.cpp
  src/ImageSource.cpp
  src/ColorPipeline.cpp
  src/PageRenderer.cpp
  src/PhotoFilters.cpp
  src/MainWindow.cpp
)
target_include_directories(photoprint_core PUBLIC src ${LCMS2_INCLUDE_DIR} ${CUPS_INCLUDE_DIR})
target_link_libraries(photoprint_core PUBLIC Qt6::Widgets Qt6::Gui Qt6::Concurrent ${LCMS2_LIBRARY} ${CUPS_LIBRARY} ZLIB::ZLIB PkgConfig::MAGICKWAND)
target_compile_options(photoprint_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(photoprint src/main.cpp)
target_link_libraries(photoprint PRIVATE photoprint_core)

enable_testing()

function(photoprint_test name)
  add_executable(${name} tests/${name}.cpp)
  target_link_libraries(${name} PRIVATE photoprint_core Qt6::Test)
  target_compile_definitions(${name} PRIVATE
    PHOTOPRINT_EVIDENCE_DIR="${CMAKE_SOURCE_DIR}/build/evidence"
    PHOTOPRINT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  add_test(NAME ${name} COMMAND ${name} WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;PHOTOPRINT_NO_CUPS=1;PHOTOPRINT_NO_SETTINGS=1;PHOTOPRINT_NO_RECOVERY=1")
endfunction()

photoprint_test(tst_import_color)
photoprint_test(tst_first_sheet_pdf)
photoprint_test(tst_window_smoke)
photoprint_test(tst_layout)
photoprint_test(tst_canvas_edits)
photoprint_test(tst_printer_options)
photoprint_test(tst_paper_geometry)
photoprint_test(tst_composition)
photoprint_test(tst_multipage_pdf)
photoprint_test(tst_project_store)
photoprint_test(tst_recovery)
photoprint_test(tst_tone_engine)
photoprint_test(tst_job_controller)
photoprint_test(tst_job_recovery)
photoprint_test(tst_trial_sheet)
photoprint_test(tst_paper_presets)
photoprint_test(tst_paper_catalog)
photoprint_test(tst_daily_flow)
photoprint_test(tst_filters)
photoprint_test(tst_filter_integration)
photoprint_test(tst_ui_screens)

# Measured targets only run with PHOTOPRINT_RUNTIME_TARGETS=1; otherwise SKIP.
add_executable(tst_runtime_targets tests/tst_runtime_targets.cpp)
target_link_libraries(tst_runtime_targets PRIVATE Qt6::Test)
target_compile_definitions(tst_runtime_targets PRIVATE
  PHOTOPRINT_BINARY="$<TARGET_FILE:photoprint>"
  PHOTOPRINT_EVIDENCE_DIR="${CMAKE_SOURCE_DIR}/build/evidence")
add_dependencies(tst_runtime_targets photoprint)
add_test(NAME tst_runtime_targets COMMAND tst_runtime_targets)
set_tests_properties(tst_runtime_targets PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;PHOTOPRINT_NO_CUPS=1")
