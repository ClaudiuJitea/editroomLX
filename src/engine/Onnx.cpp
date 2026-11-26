#include "Onnx.h"
#include <cstdio>
#include <dlfcn.h>
#include <fcntl.h>
#include <memory>
#include <unistd.h>

namespace er::onnx {

bool load() {
    static const bool ok = [] {
        void* lib = nullptr;
        fflush(stderr);
        const int saved = dup(STDERR_FILENO), null = open("/dev/null", O_WRONLY);
        if (null >= 0) dup2(null, STDERR_FILENO);
        for (const char* name : {"libonnxruntime.so.1", "libonnxruntime.so", "libonnxruntime.so.1.23"})
            if ((lib = dlopen(name, RTLD_NOW | RTLD_LOCAL))) break;
        fflush(stderr);
        if (saved >= 0) { dup2(saved, STDERR_FILENO); close(saved); }
        if (null >= 0) close(null);
        if (!lib) return false;
        using GetBase = const OrtApiBase* (*)();
        auto get = reinterpret_cast<GetBase>(dlsym(lib, "OrtGetApiBase"));
        const OrtApi* api = get ? get()->GetApi(ORT_API_VERSION) : nullptr;
        if (!api) return false;
        Ort::InitApi(api);
        return true;
    }();
    return ok;
}

Ort::Env& env() {
    static Ort::Env e(ORT_LOGGING_LEVEL_ERROR, "editroom");
    return e;
}

} // namespace er::onnx
