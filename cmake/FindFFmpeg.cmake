# FFmpeg (LGPL, dynamically linked). Headers from the system (libavcodec-dev…) or, when those aren't
# installed, from third_party/ffmpeg/include: the exact headers of the installed runtime (FFmpeg 8.0.1),
# linked against the runtime libraries by their full name (libavcodec.so.62…).
set(_ff_hint ${CMAKE_SOURCE_DIR}/third_party/ffmpeg/include)
find_path(FFmpeg_INCLUDE_DIR libavcodec/avcodec.h HINTS ${_ff_hint} PATHS /usr/include /usr/include/x86_64-linux-gnu)
set(FFmpeg_LIBRARIES)
foreach(lib avcodec avformat avutil swscale swresample)
  find_library(FFmpeg_${lib} NAMES ${lib} lib${lib}.so.62 lib${lib}.so.60 lib${lib}.so.9 lib${lib}.so.6
               PATHS /usr/lib/x86_64-linux-gnu /usr/lib64 /usr/lib)
  if(NOT FFmpeg_${lib})
    file(GLOB _cand /usr/lib/x86_64-linux-gnu/lib${lib}.so.* /usr/lib64/lib${lib}.so.*)
    list(SORT _cand)
    list(GET _cand 0 FFmpeg_${lib})
  endif()
  list(APPEND FFmpeg_LIBRARIES ${FFmpeg_${lib}})
endforeach()
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FFmpeg DEFAULT_MSG FFmpeg_INCLUDE_DIR FFmpeg_avcodec FFmpeg_avformat FFmpeg_avutil)
if(FFmpeg_FOUND AND NOT TARGET FFmpeg::FFmpeg)
  add_library(FFmpeg::FFmpeg INTERFACE IMPORTED)
  set_target_properties(FFmpeg::FFmpeg PROPERTIES INTERFACE_INCLUDE_DIRECTORIES ${FFmpeg_INCLUDE_DIR}
                        INTERFACE_LINK_LIBRARIES "${FFmpeg_LIBRARIES}")
endif()
