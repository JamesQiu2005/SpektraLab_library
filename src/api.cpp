#include "api.hpp"

#include <cstring>
#include <filesystem>
#include <functional>

#include "capture_one.hpp"
#include "spklib/spklib.h"

namespace fs = std::filesystem;

namespace spklib {

namespace {

using Handler = std::function<Json(Context&, Library*, const Json&)>;

struct Method {
    bool needs_library;
    bool writes;          ///< changes SpektraLab's data (never Capture One's)
    const char* summary;
};

/// The one table. `writes` is documentation and a test hook: no method,
/// writing or not, can reach a Capture One file except through a snapshot.
const std::map<std::string, Method>& table() {
    static const std::map<std::string, Method> t = {
        {"sdk.version", {false, false, "SDK, ABI and schema versions"}},
        {"sdk.methods", {false, false, "this table"}},
        {"capture_one.probe", {false, false, "read a Capture One catalog's version, counts and verdict (via a snapshot)"}},
        {"capture_one.link", {false, true, "create or reopen the SpektraLab library mirroring a Capture One catalog, and sync it"}},
        {"library.create", {false, true, "create a SpektraLab catalog or session"}},
        {"library.open", {false, false, "open a SpektraLab catalog or session"}},
        {"library.close", {true, false, "close a library handle"}},
        {"library.info", {true, false, "a library's identity, origin and counts"}},
        {"library.sync", {true, true, "copy what changed in the linked Capture One catalog"}},
        {"collections.list", {true, false, "the collection tree, flat, with counts"}},
        {"images.list", {true, false, "images in a collection or the whole library, filtered, sorted, paged"}},
        {"images.lookup", {true, false, "find images by absolute path"}},
        {"image.get", {true, false, "one image with every variant, its origin values and SpektraLab's"}},
        {"locations.list", {true, false, "every folder location, resolved, with online state"}},
        {"location.relink", {true, true, "point an offline location at a new root (SpektraLab-side only)"}},
        {"edit.get", {true, false, "a variant's SpektraLab edit JSON and revision"}},
        {"edit.put", {true, true, "replace a variant's edit JSON, given the revision it was made on"}},
        {"edit.delete", {true, true, "remove a variant's edit"}},
        {"meta.set", {true, true, "SpektraLab's rating and/or colour tag on variants (null clears)"}},
        {"images.trash", {true, true, "SpektraLab's trash flag on images (no file is moved)"}},
        {"variant.create", {true, true, "a new SpektraLab variant of an image, optionally copying an edit"}},
        {"images.add", {true, true, "add files to a SpektraLab library, by reference or by copy"}},
        {"collection.create", {true, true, "a SpektraLab album, group, project or smart album"}},
        {"collection.update", {true, true, "rename, move or re-criteria a SpektraLab collection"}},
        {"collection.delete", {true, true, "delete a SpektraLab collection (never images)"}},
        {"collection.add", {true, true, "add images to a SpektraLab album"}},
        {"collection.remove", {true, true, "remove images from a SpektraLab album"}},
        {"session.folders", {true, false, "a session's folders, their roles and counts"}},
        {"session.scan", {true, true, "add files that appeared in a SpektraLab session's folders"}},
        {"session.move", {true, true, "move images to Selects, Trash, Capture or a folder (SpektraLab sessions only; moves files)"}},
        {"session.restore", {true, true, "move images back to the folder they came from (moves files)"}},
        {"session.trash.plan", {true, false, "the files in a session's Trash, for the app to move to the macOS Trash"}},
        {"session.trash.commit", {true, true, "forget the images whose files the app moved to the macOS Trash"}},
    };
    return t;
}

}  // namespace

Context::Context(const Json& options) {
    if (options.is_object() && options.contains("store") && options["store"].is_string())
        store_ = options["store"].get<std::string>();
}

Json Context::methods() {
    Json out = Json::array();
    for (auto& [name, m] : table())
        out.push_back({{"method", name}, {"library", m.needs_library}, {"writes", m.writes}, {"summary", m.summary}});
    return out;
}

Library& Context::lib(const Json& params) {
    if (!params.is_object() || !params.contains("library") || !params["library"].is_string())
        throw ApiError("invalid_params", "'library' (a handle from library.open) is required");
    auto it = libraries_.find(params["library"].get<std::string>());
    if (it == libraries_.end()) throw ApiError("not_found", "no open library " + params["library"].get<std::string>());
    return *it->second;
}

Json Context::open_library(std::unique_ptr<Library> lib) {
    // One handle per library: opening it twice returns the first handle, so
    // two parts of the app cannot hold two connections that disagree.
    for (auto& [h, l] : libraries_) {
        if (l->path() == lib->path()) return {{"library", h}, {"info", l->info()}};
    }
    std::string handle = "L" + std::to_string(next_handle_++);
    Json info = lib->info();
    libraries_[handle] = std::move(lib);
    return {{"library", handle}, {"info", info}};
}

Json Context::call(const std::string& method, const Json& params_in) {
    std::lock_guard<std::mutex> lock(mutex_);
    const Json params = params_in.is_null() ? Json::object() : params_in;
    if (!params.is_object()) throw ApiError("invalid_params", "params must be a JSON object");
    auto m = table().find(method);
    if (m == table().end()) throw ApiError("unknown_method", "no method " + method);

    if (method == "sdk.version")
        return {{"version", SPKLIB_VERSION}, {"abi", SPKLIB_ABI_VERSION}, {"schema", Library::kSchemaVersion},
                {"capture_one", {{"write_back", false}, {"previews", false}}}};
    if (method == "sdk.methods") return {{"methods", methods()}};

    if (method == "capture_one.probe") {
        std::string path = params.value("path", std::string());
        if (path.empty()) throw ApiError("invalid_params", "'path' is required");
        try {
            return c1::probe(path, (fs::temp_directory_path() / ("spklib-probe-" + new_uuid())).string());
        } catch (const c1::SourceBusy& e) {
            throw ApiError("source_busy", e.what());
        } catch (const ApiError&) {
            throw;
        } catch (const std::exception& e) {
            throw ApiError("not_found", e.what());
        }
    }
    if (method == "capture_one.link") {
        std::string source = params.value("source", std::string());
        if (source.empty()) throw ApiError("invalid_params", "'source' is required");
        c1::Located located;
        try {
            located = c1::locate(source);
        } catch (const std::exception& e) {
            throw ApiError("not_found", e.what());
        }
        std::string path = params.value("path", std::string());
        if (!path.empty() && fs::path(path).extension() != ".spkcatalog") path += ".spkcatalog";
        if (path.empty()) {
            // Keyed by the catalog's document UUID, so a catalog that is
            // moved or renamed finds its SpektraLab library again.
            if (store_.empty()) throw ApiError("invalid_params", "give 'path', or create the context with a 'store'");
            Json pr;
            try {
                pr = c1::probe(source, (fs::temp_directory_path() / ("spklib-probe-" + new_uuid())).string());
            } catch (const c1::SourceBusy& e) {
                throw ApiError("source_busy", e.what());
            } catch (const std::exception& e) {
                throw ApiError("not_found", e.what());
            }
            if (!pr.value("supported", false)) throw ApiError("unsupported", pr.value("reason", std::string("unsupported catalog")));
            path = (fs::path(store_) / "Linked" / (pr["document_uuid"].get<std::string>() + ".spkcatalog")).string();
        }
        std::error_code ec;
        std::unique_ptr<Library> lib;
        Json report;
        if (fs::exists(Library::database_path(path, "catalog"), ec)) {
            lib = Library::open(path);
            Json o = params;
            o["source"] = located.package;
            report = lib->sync(o);
        } else {
            lib = Library::create(path, "catalog", params);
            try {
                report = lib->link_capture_one(source, params);
            } catch (...) {
                // A library that never synced is not worth keeping.
                std::string p = lib->path();
                lib.reset();
                fs::remove_all(p, ec);
                throw;
            }
        }
        Json out = open_library(std::move(lib));
        out["sync"] = report;
        return out;
    }
    if (method == "library.create") {
        std::string path = params.value("path", std::string());
        if (path.empty()) throw ApiError("invalid_params", "'path' is required");
        return open_library(Library::create(path, params.value("kind", std::string("catalog")), params));
    }
    if (method == "library.open") {
        std::string path = params.value("path", std::string());
        if (path.empty()) throw ApiError("invalid_params", "'path' is required");
        // Already open here: the same handle, without a second open (a
        // session's lock would refuse this process's own second open).
        auto [dir, kind] = Library::locate(path);
        for (auto& [h, l] : libraries_)
            if (l->path() == dir) return {{"library", h}, {"info", l->info()}};
        return open_library(Library::open(path));
    }
    if (method == "library.close") {
        lib(params);
        libraries_.erase(params["library"].get<std::string>());
        return {{"closed", true}};
    }

    Library& l = lib(params);
    if (method == "library.info") return l.info();
    if (method == "library.sync") return l.sync(params);
    if (method == "collections.list") return l.collections(params);
    if (method == "images.list") return l.images(params);
    if (method == "images.lookup") return l.lookup(params);
    if (method == "image.get") return l.image(params);
    if (method == "locations.list") return l.locations(params);
    if (method == "location.relink") return l.relink_location(params);
    if (method == "edit.get") return l.edit_get(params);
    if (method == "edit.put") return l.edit_put(params);
    if (method == "edit.delete") return l.edit_delete(params);
    if (method == "meta.set") return l.set_meta(params);
    if (method == "images.trash") return l.set_trashed(params);
    if (method == "variant.create") return l.variant_create(params);
    if (method == "images.add") return l.add_images(params);
    if (method == "collection.create") return l.collection_create(params);
    if (method == "collection.update") return l.collection_update(params);
    if (method == "collection.delete") return l.collection_delete(params);
    if (method == "collection.add") return l.collection_add(params);
    if (method == "collection.remove") return l.collection_remove(params);
    if (method == "session.folders") return l.session_folders(params);
    if (method == "session.scan") return l.session_scan(params);
    if (method == "session.move") return l.session_move(params);
    if (method == "session.restore") return l.session_restore(params);
    if (method == "session.trash.plan") return l.session_trash_plan(params);
    if (method == "session.trash.commit") return l.session_trash_commit(params);
    throw ApiError("internal", "method in the table without a handler: " + method);
}

Json Context::call_safe(const std::string& method, const Json& params) {
    try {
        return {{"ok", true}, {"result", call(method, params)}};
    } catch (const ApiError& e) {
        return {{"ok", false}, {"error", {{"code", e.code}, {"message", e.what()}, {"data", e.data}}}};
    } catch (const nlohmann::json::exception& e) {
        return {{"ok", false}, {"error", {{"code", "invalid_params"}, {"message", e.what()}, {"data", nullptr}}}};
    } catch (const DbError& e) {
        return {{"ok", false}, {"error", {{"code", "io"}, {"message", e.what()}, {"data", {{"sqlite", e.code}}}}}};
    } catch (const std::exception& e) {
        return {{"ok", false}, {"error", {{"code", "internal"}, {"message", e.what()}, {"data", nullptr}}}};
    }
}

}  // namespace spklib

// --- the C ABI ---------------------------------------------------------------------------------

struct spklib_context {
    spklib::Context ctx;
    explicit spklib_context(const spklib::Json& o) : ctx(o) {}
};

namespace {
char* dup(const std::string& s) {
    char* p = static_cast<char*>(std::malloc(s.size() + 1));
    std::memcpy(p, s.c_str(), s.size() + 1);
    return p;
}
}  // namespace

extern "C" {

spklib_context* spklib_create(const char* options_json, char** out_error) {
    try {
        spklib::Json o = options_json && *options_json ? spklib::Json::parse(options_json) : spklib::Json::object();
        return new spklib_context(o);
    } catch (const std::exception& e) {
        if (out_error) *out_error = dup(e.what());
        return nullptr;
    }
}

void spklib_destroy(spklib_context* ctx) { delete ctx; }

int spklib_call(spklib_context* ctx, const char* method, const char* params_json, char** out_json) {
    spklib::Json reply;
    if (!ctx || !method) {
        reply = {{"ok", false}, {"error", {{"code", "invalid_params"}, {"message", "null context or method"}, {"data", nullptr}}}};
    } else {
        spklib::Json params;
        try {
            params = params_json && *params_json ? spklib::Json::parse(params_json) : spklib::Json::object();
            reply = ctx->ctx.call_safe(method, params);
        } catch (const std::exception& e) {
            reply = {{"ok", false}, {"error", {{"code", "invalid_params"}, {"message", std::string("params are not JSON: ") + e.what()}, {"data", nullptr}}}};
        }
    }
    bool ok = reply["ok"].get<bool>();
    if (out_json) *out_json = dup((ok ? reply["result"] : reply["error"]).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
    return ok ? SPKLIB_OK : SPKLIB_ERROR;
}

void spklib_free(char* s) { std::free(s); }

const char* spklib_version(void) {
    static const std::string v = std::string("spklib ") + SPKLIB_VERSION + " (abi " + std::to_string(SPKLIB_ABI_VERSION) +
                                 ", schema " + std::to_string(spklib::Library::kSchemaVersion) + ")";
    return v.c_str();
}

}  // extern "C"
