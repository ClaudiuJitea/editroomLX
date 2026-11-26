# MCP: the tools (schemas from resources/mcp-tools.json), the local HTTP server and the stdio bridge.
add_library(editroom_mcp STATIC MagicEdit.h MagicEdit.cpp MagicPlan.h MagicPlan.cpp LocalImprover.h LocalImprover.cpp LocalMagic.h LocalMagic.cpp MagicMentions.h MagicMentions.cpp MentionField.h MentionField.cpp MagicShow.h MagicShow.cpp MagicThinking.h MagicThinking.cpp LocalPlanners.h LocalPlanners.cpp MCPTools.h MCPTools.cpp MCPServer.h MCPServer.cpp MagicAgent.h MagicAgent.cpp MagicPopover.h MagicPopover.cpp)
target_link_libraries(editroom_mcp PUBLIC editroom_analysis Qt6::Network)
target_compile_definitions(editroom_mcp PRIVATE ER_RESOURCES="${CMAKE_SOURCE_DIR}/resources")
target_compile_options(editroom_mcp PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
