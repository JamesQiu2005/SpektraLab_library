// api.hpp -- the method table. Everything the C ABI and the stdio host do is
// a `Context::call`.

#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "json.hpp"
#include "library.hpp"

#ifndef SPKLIB_VERSION
#define SPKLIB_VERSION "0.2.0"
#endif

namespace spklib {

class Context {
public:
    explicit Context(const Json& options);

    /// Throws ApiError; every other exception is mapped to `internal` by
    /// `call_safe`.
    Json call(const std::string& method, const Json& params);

    /// {"ok": true, "result": ...} or {"ok": false, "error": {...}}.
    Json call_safe(const std::string& method, const Json& params);

    static Json methods();

private:
    Library& lib(const Json& params);
    Json open_library(std::unique_ptr<Library> lib);

    std::mutex mutex_;
    std::string store_;
    std::map<std::string, std::unique_ptr<Library>> libraries_;
    int next_handle_ = 1;
};

}  // namespace spklib
