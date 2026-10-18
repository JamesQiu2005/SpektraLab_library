// spklib-host -- the SDK as a child process (docs/PROTOCOL.md).
//
// stdin carries one JSON request per line, stdout one JSON reply per line,
// stderr is a free-form log. Requests run in order. The host is a client of
// the C ABI exactly as an app that links the library is, so the two can
// never offer different methods.
//
//   spklib-host [--store DIR]                    serve stdio
//   spklib-host probe PATH                       capture_one.probe, printed
//   spklib-host call METHOD [PARAMS] [--store DIR]   one call, printed

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

#include "json.hpp"
#include "spklib/spklib.h"

using spklib::Json;

namespace {

std::string take(char* s) {
    std::string out = s ? s : "";
    spklib_free(s);
    return out;
}

int one_shot(spklib_context* ctx, const char* method, const char* params) {
    char* out = nullptr;
    int rc = spklib_call(ctx, method, params, &out);
    std::string text = take(out);
    std::cout << Json::parse(text).dump(2) << "\n";
    return rc == SPKLIB_OK ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::string store;
    std::vector<const char*> args;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--store") && i + 1 < argc) store = argv[++i];
        else args.push_back(argv[i]);
    }
    Json options = Json::object();
    if (!store.empty()) options["store"] = store;
    char* err = nullptr;
    spklib_context* ctx = spklib_create(options.dump().c_str(), &err);
    if (!ctx) {
        std::fprintf(stderr, "spklib-host: %s\n", take(err).c_str());
        return 2;
    }

    if (!args.empty()) {
        int rc = 2;
        if (!std::strcmp(args[0], "probe") && args.size() == 2) {
            rc = one_shot(ctx, "capture_one.probe", Json{{"path", args[1]}}.dump().c_str());
        } else if (!std::strcmp(args[0], "call") && (args.size() == 2 || args.size() == 3)) {
            rc = one_shot(ctx, args[1], args.size() == 3 ? args[2] : "{}");
        } else if (!std::strcmp(args[0], "--version")) {
            std::cout << spklib_version() << "\n";
            rc = 0;
        } else {
            std::fprintf(stderr, "usage: spklib-host [--store DIR] | probe PATH | call METHOD [PARAMS_JSON]\n");
        }
        spklib_destroy(ctx);
        return rc;
    }

    std::ios::sync_with_stdio(false);
    std::cout << Json{{"event", "ready"}, {"version", spklib_version()}, {"protocol", 1}}.dump() << "\n" << std::flush;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        Json reply;
        Json id = nullptr;
        try {
            Json req = Json::parse(line);
            id = req.value("id", Json(nullptr));
            std::string method = req.value("method", std::string());
            if (method == "host.shutdown") {
                std::cout << Json{{"id", id}, {"result", {{"shutdown", true}}}}.dump() << "\n" << std::flush;
                break;
            }
            if (method == "host.ping") {
                reply = {{"id", id}, {"result", {{"pong", true}}}};
            } else {
                Json params = req.contains("params") ? req["params"] : Json::object();
                char* out = nullptr;
                int rc = spklib_call(ctx, method.c_str(), params.dump().c_str(), &out);
                Json body = Json::parse(take(out));
                reply = rc == SPKLIB_OK ? Json{{"id", id}, {"result", body}} : Json{{"id", id}, {"error", body}};
            }
        } catch (const std::exception& e) {
            reply = {{"id", id}, {"error", {{"code", "invalid_params"}, {"message", std::string("bad request: ") + e.what()}, {"data", nullptr}}}};
        }
        std::cout << reply.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << "\n" << std::flush;
    }
    spklib_destroy(ctx);
    return 0;
}
