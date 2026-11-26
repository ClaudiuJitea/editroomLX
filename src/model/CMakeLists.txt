# Pure model: Qt Core only (no GUI, no Vulkan, no FFmpeg), so it can be unit-tested and diffed against Swift.
add_library(editroom_model STATIC
  Json.h Json.cpp
  SceneTags.h SceneTags.cpp
  Types.h
  Project.h Project.cpp
  SpeedRamp.h SpeedRamp.cpp
  ZoomMove.h ZoomMove.cpp
  ColorGrade.h ColorGrade.cpp
  Captions.h Captions.cpp
  MediaFolders.h MediaFolders.cpp
  Timecode.h Timecode.cpp
  Codec.h Codec.cpp
  Shortcuts.h Shortcuts.cpp
)
target_include_directories(editroom_model PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(editroom_model PUBLIC Qt6::Core)
target_compile_options(editroom_model PRIVATE -Wall -Wextra)
