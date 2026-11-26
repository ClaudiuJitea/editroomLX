// ONNX Runtime, loaded when first wanted (dlopen) rather than linked: Editroom runs without it, and Ubuntu's build,
// which registers ONNX's schemas twice and says so on stderr hundreds of times, says it to nobody.
#pragma once
#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>
#undef ORT_API_MANUAL_INIT

namespace er::onnx {
/// True once the library is loaded and its API is set up (Ort:: can be used).
bool load();
/// The ONNX Runtime environment, made once.
Ort::Env& env();
} // namespace er::onnx
