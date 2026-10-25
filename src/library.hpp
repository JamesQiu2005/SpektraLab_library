// library.hpp -- SpektraLab's own library: a catalog or a session.
//
// The schema borrows Capture One's shape (path locations, images, variants,
// collections) so a Capture One catalog maps onto it row for row, but it is
// SpektraLab's file, in SpektraLab's place, and the only file this SDK ever
// writes. docs/DESIGN.md is the reference; the rules that matter here:
//
//  - Every row has an `origin`: 'local' (SpektraLab made it) or
//    'capture_one' (a sync copied it). A sync writes only origin-owned
//    columns of origin rows. Nothing SpektraLab owns -- edits, rating and
//    colour-tag overrides, local albums, relinked roots -- is ever touched by
//    a sync, and nothing a sync owns is writable through the API.
//  - A row that disappears from Capture One is marked `origin_state =
//    'removed'`, never deleted, so the SpektraLab edit on it survives.
//  - Edits are the app's JSON, stored verbatim. The SDK checks that it is a
//    JSON object and never looks inside.

#pragma once

#include <memory>
#include <optional>
#include <set>
#include <vector>
#include <string>

#include "json.hpp"
#include "sqlite.hpp"

namespace spklib {

namespace c1 { struct Catalog; }

/// An error with a code the app can switch on: not_found, invalid_params,
/// read_only, conflict, source_busy, unsupported, exists, io, internal.
struct ApiError : std::runtime_error {
    std::string code;
    Json data;
    ApiError(std::string c, const std::string& msg, Json d = nullptr)
        : std::runtime_error(msg), code(std::move(c)), data(std::move(d)) {}
};

/// The SQL an images query becomes (library.cpp).
struct Where;

std::string new_uuid();
double now_unix();

class Library {
public:
    static constexpr int kSchemaVersion = 3;

    /// kind is "catalog" (`<path>` is a `.spkcatalog` package) or "session"
    /// (`<path>` is the session folder).
    static std::unique_ptr<Library> create(const std::string& path, const std::string& kind, const Json& options);
    static std::unique_ptr<Library> open(const std::string& path);

    /// The library folder and kind at `path` (its folder or its database).
    static std::pair<std::string, std::string> locate(const std::string& path);

    /// Where the database of a library at `path` is, by its kind.
    static std::string database_path(const std::string& path, const std::string& kind);

    const std::string& path() const { return path_; }
    const std::string& kind() const { return kind_; }
    std::string uuid();

    Json info();

    // --- Capture One, one way -----------------------------------------------------
    /// Point this library at a Capture One catalog and sync it. A library
    /// links one catalog for its whole life; linking another is refused.
    Json link_capture_one(const std::string& source, const Json& options);
    /// Copy what changed in the linked catalog. Skipped when the source's
    /// fingerprint is the one last synced, unless `force`.
    Json sync(const Json& options);

    // --- Capture One, copied ----------------------------------------------------------
    /// A new SpektraLab catalog at `path` holding a copy of the catalog at
    /// `source`: every row SpektraLab's own (origin local), each remembering
    /// Capture One's key and the values Capture One had, so that `update_copy`
    /// can bring in what Capture One changed later. `mirror`, a library made
    /// by `capture_one.link` from the same catalog, contributes SpektraLab's
    /// overrides and edits. Fills `report`.
    static std::unique_ptr<Library> copy_capture_one(const std::string& source, const std::string& path,
                                                     const Json& options, Library* mirror, Json& report);
    /// Bring in what changed in the Capture One catalog since the copy or the
    /// last update. SpektraLab wins: a field is taken from Capture One only
    /// where the copy still holds the value Capture One had before.
    Json update_copy(const Json& params);
    /// `copied_from` as `library.info` reports it, or null.
    Json copied_from();

    // --- reading ---------------------------------------------------------------------
    Json collections(const Json& params);
    Json images(const Json& params);
    Json image(const Json& params);
    Json lookup(const Json& params);
    Json locations(const Json& params);
    /// Counts per rating, colour tag, kind, capture date, camera, lens,
    /// keyword and edit state over the scope `images.list` would list. Each
    /// facet is counted with every filter but its own, so a choice in one
    /// facet narrows the others and never empties itself.
    Json facets(const Json& params);

    // --- SpektraLab-owned writes ----------------------------------------------------
    Json edit_get(const Json& params);
    Json edit_put(const Json& params);
    Json edit_delete(const Json& params);
    Json set_meta(const Json& params);
    Json set_trashed(const Json& params);
    /// Forget images whose file is gone; never a file (0.3.1).
    Json remove_images(const Json& params);
    Json variant_create(const Json& params);
    Json add_images(const Json& params);
    Json collection_create(const Json& params);
    Json collection_update(const Json& params);
    Json collection_delete(const Json& params);
    Json collection_add(const Json& params);
    Json collection_remove(const Json& params);
    Json relink_location(const Json& params);
    /// What the app read of a file's header, for an image a folder sync or
    /// an add without metadata left undescribed. SpektraLab's rows only.
    Json describe(const Json& params);

    // --- referenced folders kept in step (SpektraLab catalogs and sessions) -------------
    /// Watch a folder: its pictures are added by reference, and every
    /// `folders.sync` notices files that appeared, went or moved inside it.
    Json folders_add(const Json& params);
    Json folders_list(const Json& params);
    Json folders_sync(const Json& params);
    Json folders_remove(const Json& params);

    // --- a SpektraLab session's folders (native sessions only) ------------------------
    Json session_folders(const Json& params);
    Json session_scan(const Json& params);
    Json session_move(const Json& params);
    Json session_restore(const Json& params);
    Json session_trash_plan(const Json& params);
    Json session_trash_commit(const Json& params);

    Db& db() { return db_; }
    ~Library();

private:
    Json merge_capture_one(const c1::Catalog& cat, const std::string& package, Library* mirror);
    void lock_session();
    void ensure_system_collections();
    void require_own_library();
    void scope(const Json& params, Where& w);
    std::optional<int64_t> new_recent_import(const std::vector<int64_t>& images, std::optional<double> at = std::nullopt);
    Json sync_tree(const std::string& root, bool recursive, const std::set<std::string>& skip, bool recent_import);
    Json sync_watched(int64_t watched_id, bool first);
    std::vector<int64_t> locations_under(const std::string& root, bool recursive);
    void reconcile_intents();
    void require_session();
    std::optional<int64_t> role_location(const std::string& role);
    Json move_image(int64_t image_id, int64_t to_location, const std::string& op, bool suffix);
    Library(std::string path, std::string kind, Db db);
    void migrate();
    std::string meta(const std::string& key, const std::string& fallback = "");
    void set_meta_value(const std::string& key, const std::string& value);
    std::string resolve_location(int64_t location_id);
    std::string origin_package();
    int64_t variant_id(const std::string& uuid);
    int64_t image_id(const std::string& uuid);
    int64_t collection_id(const std::string& uuid, std::string* kind = nullptr, std::string* origin = nullptr);
    int64_t ensure_location(const std::string& dir);
    int64_t ensure_folder(int64_t location_id);
    void build_location_cache();

    std::string path_;
    std::string kind_;
    Db db_;
    struct LocationCache;
    std::shared_ptr<LocationCache> locations_;
};

}  // namespace spklib
