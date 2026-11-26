cmake_minimum_required(VERSION 3.24)
project(EditroomLX VERSION 1.0.10 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Network Concurrent Test)
find_package(OpenMP REQUIRED)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)
find_package(FFmpeg REQUIRED)

# Mac baseline this tree is synced to (see docs/sync/).
set(EDITROOM_MAC_BASELINE "4272a42")

add_subdirectory(src/model)
add_subdirectory(src/editor)
add_subdirectory(src/rendering)
add_subdirectory(src/media)
add_subdirectory(src/audio)
add_subdirectory(src/engine)
add_subdirectory(src/ui)
# whisper.cpp (MIT, third_party/whisper.cpp): what's said in clips, heard on this computer. A static CPU build
# for x86-64-v3 (AVX2, FMA, F16C), so a package runs on any machine from the last decade, not just this one.
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
set(GGML_AVX ON CACHE BOOL "" FORCE)
set(GGML_AVX2 ON CACHE BOOL "" FORCE)
set(GGML_FMA ON CACHE BOOL "" FORCE)
set(GGML_F16C ON CACHE BOOL "" FORCE)
set(GGML_OPENMP ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/whisper.cpp EXCLUDE_FROM_ALL SYSTEM)
# ONNX Runtime (MIT) for scene tags: its headers vendored; the library is the system's (libonnxruntime1.23),
# loaded when it's first wanted (engine/SceneTagStore.cpp), so it's a recommendation rather than a dependency.
add_library(onnxruntime_headers INTERFACE)
target_include_directories(onnxruntime_headers SYSTEM INTERFACE "${CMAKE_SOURCE_DIR}/third_party/onnxruntime/include")
# Optimised even in debug builds: unoptimised, hearing a minute of speech takes minutes.
foreach(t whisper ggml ggml-base ggml-cpu)
  if(TARGET ${t})
    target_compile_options(${t} PRIVATE -O3)
  endif()
endforeach()
add_subdirectory(src/analysis)
add_subdirectory(src/mcp)
add_subdirectory(src/app)

enable_testing()
option(EDITROOM_BUILD_TESTS "Build the test suites" ON)
if(EDITROOM_BUILD_TESTS)
  add_subdirectory(tests)
endif()

# Packages: `cmake --build --preset release && cd build/release && cpack` makes a .deb (its dependencies worked out
# from the libraries it links) and a .tar.gz.
set(CPACK_PACKAGE_NAME "editroom-lx")
set(CPACK_PACKAGE_VENDOR "Wonder Assembly LLC")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Editroom: a video editor with a timeline, and with Magic")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_CONTACT "Editroom")
set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_SOURCE_DIR}/LICENSE)
set(CPACK_GENERATOR "DEB;TGZ")
set(CPACK_DEBIAN_PACKAGE_SECTION "video")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "mesa-vulkan-drivers, ffmpeg")
set(CPACK_STRIP_FILES ON)
include(CPack)
