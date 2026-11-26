# On-device analysis: frame metrics and best moments, and the beat tracker (MediaAnalyzer.swift).
add_library(editroom_analysis STATIC MediaAnalyzer.h MediaAnalyzer.cpp AutoPlanner.h AutoPlanner.cpp LocalBrain.h LocalBrain.cpp)
target_link_libraries(editroom_analysis PUBLIC editroom_ui)
target_compile_options(editroom_analysis PRIVATE -Wall -Wextra)
