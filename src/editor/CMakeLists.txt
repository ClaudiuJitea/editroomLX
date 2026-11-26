# The editor's state and commands (EditorModel.swift): Qt Core only, so it runs headless in tests.
add_library(editroom_editor STATIC EditorHost.h EditorModel.h EditorModel.cpp)
target_link_libraries(editroom_editor PUBLIC editroom_model)
target_compile_options(editroom_editor PRIVATE -Wall -Wextra)
