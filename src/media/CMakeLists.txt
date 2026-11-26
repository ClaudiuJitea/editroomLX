# Media: FFmpeg probing and decoding (VA-API / Vulkan / CUDA / software), the chunk cache, stills
# (Qt image plugins + libheif) and the media store.
find_package(PkgConfig REQUIRED)
pkg_check_modules(HEIF REQUIRED IMPORTED_TARGET libheif)
add_library(editroom_media STATIC Decoder.h Decoder.cpp ChunkCache.h ChunkCache.cpp Stills.h Stills.cpp MediaStore.h MediaStore.cpp)
target_link_libraries(editroom_media PUBLIC editroom_rendering FFmpeg::FFmpeg PkgConfig::HEIF Qt6::Concurrent)
target_compile_options(editroom_media PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
