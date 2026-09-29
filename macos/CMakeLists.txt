# macOS-only native application. The Windows project is deliberately untouched.
cmake_minimum_required(VERSION 3.24)
project(SuperRecordingMac VERSION 1.7.0 LANGUAGES CXX OBJCXX)
if(NOT APPLE)
  message(FATAL_ERROR "Build this target with the macOS SDK (locally or GitHub Actions).")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Oldest supported macOS")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(APP_ICON "${CMAKE_BINARY_DIR}/SuperRecording.icns")
add_custom_command(OUTPUT "${APP_ICON}"
  COMMAND xcrun swift "${CMAKE_CURRENT_SOURCE_DIR}/tools/MakeIcon.swift"
    "${CMAKE_CURRENT_SOURCE_DIR}/../assets/SuperRecording.ico" "${APP_ICON}"
  DEPENDS tools/MakeIcon.swift ../assets/SuperRecording.ico
  VERBATIM)
set_source_files_properties("${APP_ICON}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources GENERATED TRUE)
add_executable(SuperRecording MACOSX_BUNDLE
  "${APP_ICON}"
  app/Main.mm app/AppDelegate.mm app/Preferences.mm app/HotKey.mm app/UpdateChecker.mm
  common/Support.mm
  capture/RegionSelector.mm capture/RecordingHUD.mm capture/Recorder.mm
  media/ExportJob.mm media/ExportComposition.mm media/GifExporter.mm media/ExportCoordinator.mm
  editor/EditorController.mm editor/EditorLayout.mm editor/AnnotationView.mm
  editor/TimelineView.mm editor/AnnotationDrawing.mm
  tests/SelfTest.mm tests/MediaTests.mm
  ../src/annotations/Annotation.cpp ../src/media/ExportQuality.cpp)
target_include_directories(SuperRecording PRIVATE . ../src)
target_compile_options(SuperRecording PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc> -Wall -Wextra -Werror
  -Wno-deprecated-declarations -Wno-unused-parameter)
target_link_libraries(SuperRecording PRIVATE
  "-framework AppKit" "-framework ScreenCaptureKit" "-framework AVFoundation"
  "-framework AVKit" "-framework CoreMedia" "-framework CoreVideo"
  "-framework CoreImage" "-framework CoreText" "-framework QuartzCore"
  "-framework ImageIO" "-framework UniformTypeIdentifiers" "-framework VideoToolbox"
  "-framework ServiceManagement" "-framework Carbon" "-framework AudioToolbox")
set_target_properties(SuperRecording PROPERTIES
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/app/Info.plist"
  MACOSX_BUNDLE_BUNDLE_NAME "SuperRecording"
  MACOSX_BUNDLE_GUI_IDENTIFIER "com.superrecording.mac"
  MACOSX_BUNDLE_SHORT_VERSION_STRING "1.7.0"
  MACOSX_BUNDLE_BUNDLE_VERSION "17001")
enable_testing()
add_test(NAME mac_core COMMAND $<TARGET_FILE:SuperRecording> --self-test)
add_test(NAME mac_media COMMAND $<TARGET_FILE:SuperRecording> --media-test ${CMAKE_BINARY_DIR}/test-results)
set_tests_properties(mac_media PROPERTIES TIMEOUT 180)
