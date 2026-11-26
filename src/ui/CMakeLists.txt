# The editor's window (Qt Widgets, QPainter-drawn chrome in the Mac's palette).
add_library(editroom_ui STATIC Updates.h Updates.cpp Theme.h Theme.cpp Widgets.h Widgets.cpp Alert.h Alert.cpp Thumbnails.h Thumbnails.cpp
  Controller.h Controller.cpp TimelineCanvas.h TimelineCanvas.cpp TimelinePanel.h TimelinePanel.cpp
  LibraryPanel.h LibraryPanel.cpp PreviewPanel.h PreviewPanel.cpp InspectorPanel.h InspectorPanel.cpp
  InspectorWidgets.h InspectorWidgets.cpp InspectorGraphs.h InspectorGraphs.cpp Symbols.h Symbols.cpp SourcePreview.h SourcePreview.cpp MarkerPanel.h MarkerPanel.cpp ControllerMedia.cpp LevelMeter.h LevelMeter.cpp
  ExportDialog.h ExportDialog.cpp SettingsDialog.h SettingsDialog.cpp MainWindow.h MainWindow.cpp)
target_link_libraries(editroom_ui PUBLIC editroom_engine Qt6::Widgets)
target_compile_definitions(editroom_ui PRIVATE ER_RESOURCES="${CMAKE_SOURCE_DIR}/resources")
target_compile_options(editroom_ui PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
