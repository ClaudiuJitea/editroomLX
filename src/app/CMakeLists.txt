# The editroom-lx executable: the editor, and the headless tools (--version, --roundtrip, --bench, --mcp…).
qt_add_resources(EDITROOM_RESOURCES ${CMAKE_SOURCE_DIR}/packaging/linux/editroom.qrc)
add_executable(editroom-lx main.cpp Bench.h Bench.cpp SelfTest.h SelfTest.cpp SingleInstance.h SingleInstance.cpp ${EDITROOM_RESOURCES})
target_link_libraries(editroom-lx PRIVATE editroom_mcp Qt6::Network)
target_compile_definitions(editroom-lx PRIVATE EDITROOM_VERSION="${PROJECT_VERSION}" EDITROOM_MAC_BASELINE="${EDITROOM_MAC_BASELINE}")

# Installing: the program, its resources, the launcher entry, the project file type and the icon.
include(GNUInstallDirs)
install(TARGETS editroom-lx RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_SOURCE_DIR}/resources/magic-brief.json ${CMAKE_SOURCE_DIR}/resources/mcp-tools.json
        DESTINATION ${CMAKE_INSTALL_DATADIR}/editroom)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/resources/fonts DESTINATION ${CMAKE_INSTALL_DATADIR}/editroom)
# YuNet (MIT) for counting faces: small enough to come with Editroom, so nothing is downloaded for it.
install(FILES ${CMAKE_SOURCE_DIR}/third_party/models/yunet/face_detection_yunet_2023mar.onnx DESTINATION ${CMAKE_INSTALL_DATADIR}/editroom/models)
install(FILES ${CMAKE_SOURCE_DIR}/third_party/models/yunet/LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR} RENAME YuNet-LICENSE.txt)
install(FILES ${CMAKE_SOURCE_DIR}/packaging/linux/editroomlx.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES ${CMAKE_SOURCE_DIR}/packaging/linux/editroom-mime.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/mime/packages RENAME editroom.xml)
# The app icon at every size the desktop asks for (made from icons/icon.jpeg).
foreach(size 16 24 32 48 64 128 256 512)
  install(FILES ${CMAKE_SOURCE_DIR}/resources/icons/hicolor/editroom-${size}.png
          DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps RENAME editroomlx.png)
endforeach()
install(FILES ${CMAKE_SOURCE_DIR}/LICENSE ${CMAKE_SOURCE_DIR}/resources/fonts/Inter-LICENSE.txt DESTINATION ${CMAKE_INSTALL_DOCDIR} OPTIONAL)
