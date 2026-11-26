# Audio: the mix plan (CompositionBuilder's audio side), the mixer with Signalsmith Stretch for speed
# changes, loudness (BS.1770), waveforms, meters, reversed sound, and the player's output (miniaudio).
add_library(editroom_audio STATIC AudioPlan.h AudioPlan.cpp Mixer.h Mixer.cpp Loudness.h Loudness.cpp
  Waveform.h Waveform.cpp ReverseAudio.h ReverseAudio.cpp Meters.h Meters.cpp AudioStream.h AudioStream.cpp AudioScrubber.h AudioScrubber.cpp)
target_include_directories(editroom_audio SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/third_party/signalsmith ${CMAKE_SOURCE_DIR}/third_party/miniaudio)
target_link_libraries(editroom_audio PUBLIC editroom_media OpenMP::OpenMP_CXX ${CMAKE_DL_LIBS} pthread m)
target_compile_options(editroom_audio PRIVATE -Wall -Wextra -Wno-unknown-pragmas)
set_source_files_properties(AudioStream.cpp AudioScrubber.cpp PROPERTIES COMPILE_OPTIONS "-w")
