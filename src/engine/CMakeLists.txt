# The engine: export settings, the file writer (libavcodec encoders), the exporter and its queue, the
# player, and the sound store tying the audio side to the media store.
add_library(editroom_engine STATIC Onnx.h Onnx.cpp Faces.h Faces.cpp SceneTagStore.h SceneTagStore.cpp Transcripts.h Transcripts.cpp ScrubCache.h ScrubCache.cpp ExportSettings.h ExportSettings.cpp MediaWriter.h MediaWriter.cpp Sound.h Sound.cpp
  Exporter.h Exporter.cpp PlaybackEngine.h PlaybackEngine.cpp)
target_link_libraries(editroom_engine PUBLIC editroom_audio editroom_editor Qt6::Network PUBLIC onnxruntime_headers PRIVATE whisper ${CMAKE_DL_LIBS})
target_compile_definitions(editroom_engine PRIVATE ER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(editroom_engine PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
