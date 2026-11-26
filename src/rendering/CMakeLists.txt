# The compositor: RenderPlan (CompositionBuilder's video side), the shared kernel (ColorMath.h +
# Kernel.inc), the CPU renderer (OpenMP) and the Vulkan renderer (shaders/*.comp, compiled to SPIR-V and
# embedded at build time).
find_package(Vulkan REQUIRED)
find_program(ER_GLSLANG NAMES glslangValidator glslang
  HINTS ${CMAKE_SOURCE_DIR}/third_party/tools/bin REQUIRED)

set(ER_SPV_HEADERS)
foreach(shader rendering pack)
  set(out ${CMAKE_CURRENT_BINARY_DIR}/${shader}_spv.h)
  add_custom_command(
    OUTPUT ${out}
    COMMAND ${ER_GLSLANG} -V --target-env vulkan1.1 -I${CMAKE_CURRENT_SOURCE_DIR}
            --vn er_${shader}_spv -o ${out} ${CMAKE_SOURCE_DIR}/shaders/${shader}.comp
    DEPENDS ${CMAKE_SOURCE_DIR}/shaders/${shader}.comp ColorMath.h Kernel.inc
    COMMENT "SPIR-V ${shader}.comp")
  list(APPEND ER_SPV_HEADERS ${out})
endforeach()

add_library(editroom_rendering STATIC
  ColorMath.h Kernel.inc VideoFrame.h VideoFrame.cpp SourceColor.h SourceColor.cpp
  RenderPlan.h RenderPlan.cpp CpuRenderer.h CpuRenderer.cpp GpuRenderer.h GpuVulkan.cpp
  ${ER_SPV_HEADERS})
target_include_directories(editroom_rendering PUBLIC ${CMAKE_SOURCE_DIR}/src PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(editroom_rendering PUBLIC editroom_model Qt6::Gui Vulkan::Vulkan OpenMP::OpenMP_CXX)
target_compile_options(editroom_rendering PRIVATE -Wall -Wextra -Wno-unknown-pragmas -Wno-missing-field-initializers)
