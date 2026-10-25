// capture_one.hpp -- reading a Capture One catalog, and only reading it.
//
// The user's `.cocatalogdb` is never opened by SQLite. `take_snapshot` copies
// the file byte for byte into SpektraLab's own store, checks the copy, and
// `read` opens the copy `immutable=1`. So the worst this code can do to a
// Capture One library is fail to read it.
//
// What is read is the part SpektraLab 2.0 needs: where every image is, its
// variants, ratings and colour tags, the collection tree and EXIF. Capture
// One's adjustments, previews (Cache/), masks (Adjustments/LAM) and styles are
// not read; docs/CAPTURE-ONE-CATALOG.md says why, table by table.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "json.hpp"

namespace spklib::c1 {

/// What identifies one state of the source file. Equal fingerprints mean
/// nothing was committed in between: the SQLite header's file change counter
/// is incremented by every write transaction in rollback-journal mode, which
/// is the mode Capture One uses (checked by `probe`).
struct Fingerprint {
    int64_t size = 0;
    int64_t mtime_ns = 0;
    uint32_t change_counter = 0;
    bool operator==(const Fingerprint&) const = default;
    Json to_json() const;
    static Fingerprint from_json(const Json&);
};

struct Located {
    std::string package;   ///< the `.cocatalog` folder
    std::string database;  ///< the `.cocatalogdb` inside it
};

/// Accepts the package, or the database file inside it. Throws when neither.
Located locate(const std::string& path);

/// The state of the source right now, read with `stat` and one 100-byte read.
Fingerprint fingerprint(const std::string& database);

struct Snapshot {
    std::string path;          ///< the copy, in SpektraLab's store
    Fingerprint source;        ///< the source as it was copied
    int attempts = 0;
};

/// Copy `database` to `<dest_dir>/snapshot.cocatalogdb` and prove the copy is
/// a consistent database. Refuses (throws `SourceBusy`) while Capture One has
/// a transaction open -- a hot `-journal` -- or the file changes under the
/// copy, after `retries` attempts.
struct SourceBusy : std::runtime_error { using std::runtime_error::runtime_error; };
Snapshot take_snapshot(const std::string& database, const std::string& dest_dir, int retries = 5);

// --- the model -------------------------------------------------------------------

struct PathLocation {
    int64_t pk = 0;
    bool relative = false;     ///< relative to the catalog package
    std::string mac_root, relative_path, win_root, volume;
};

struct Image {
    int64_t pk = 0;
    std::string uuid, file_name, display_name;
    int64_t location = 0;
    bool inside_catalog = false, trashed = false;
    std::string kind;          ///< raw | raster | movie | other
    std::optional<int64_t> width, height, file_size, iso;
    /// The camera's wall clock, stored by Capture One as if it were UTC:
    /// 23:39 on the camera reads 23:39Z here. Not an instant.
    std::optional<double> captured_local;
    /// Real UTC instants.
    std::optional<double> imported_at;
    std::optional<double> aperture, shutter, focal_length;
    std::optional<std::string> camera_make, camera_model, lens, exposure_comp,
        gps_lat, gps_lon, gps_alt;
};

struct Variant {
    int64_t pk = 0;
    std::string uuid;
    int64_t image = 0;
    int64_t position = 1;      ///< 1-based; Capture One's ZINDEX is 128*n - 1
    bool modified = false;     ///< Capture One has adjusted it
    std::optional<int64_t> rating, color_tag;
    Json meta = Json::object();       ///< IPTC text fields that are set
    Json geometry = Json::object();   ///< crop/rotation/flip, raw, not mapped
};

struct Collection {
    int64_t pk = 0;
    std::string entity;        ///< Capture One's entity name, e.g. AlbumCollection
    std::string kind;          ///< ours: project, group, album, smart_album, folder, ...
    std::string uuid, name, sort_order;
    std::optional<int64_t> parent, position, folder_location;
    std::optional<std::string> smart_criteria;
    /// ZDATECREATED: a real UTC instant. A Recent Imports album's is the
    /// import's time, which its name ("2026年10月3日 16:53") spells in the UI
    /// language and the zone of the Mac that imported.
    std::optional<double> created_at;
};

struct Keyword {
    int64_t pk = 0;
    std::string name;
    std::optional<int64_t> parent;
};

struct Catalog {
    std::string document_uuid;
    int64_t document_type = 0;   ///< 1 in every catalog seen so far
    std::optional<double> created_at, modified_at;
    std::string format;          ///< the newest ZVERSIONINFO row, e.g. "16.8.6.25 Pro Mac"
    int64_t compatible_version = 0;
    /// The root's pointers say which collection plays which role; the names
    /// are in the UI's language and are no use for that.
    int64_t root_collection = 0, recent_imports_collection = 0, recent_captures_collection = 0;

    std::vector<PathLocation> locations;
    std::vector<Image> images;
    std::vector<Variant> variants;
    std::vector<Collection> collections;
    std::vector<std::pair<int64_t, int64_t>> album_members;  ///< (collection, image), albums only
    std::vector<Keyword> keywords;
    std::vector<std::string> warnings;
};

/// What the reader needs, as (table, column). A catalog missing any of these
/// is refused with the list, rather than read half-way.
const std::vector<std::pair<std::string, std::string>>& required_schema();

/// Read a snapshot. Throws on a schema it cannot read.
Catalog read(const std::string& snapshot_path);

/// Map Capture One's collection entity to SpektraLab's kind.
std::string collection_kind(const std::string& entity);

/// Absolute path of a location's folder. `package` resolves relative ones.
std::string resolve(const PathLocation& loc, const std::string& package);

/// Everything `catalog.probe` reports: version, counts, the journal state and
/// a verdict, read from a snapshot in `scratch_dir` (removed afterwards).
Json probe(const std::string& path, const std::string& scratch_dir);

}  // namespace spklib::c1
