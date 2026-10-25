# spklib API spec

| | |
|---|---|
| **Version** | 0.2.0: `abi` 1, library `schema` 2 (`sdk.version` reports them) |
| **Transports** | the C ABI in `include/spklib/spklib.h`, and `spklib-host` over stdio. One method table serves both |
| **Licence** | MIT, like the rest of this repository |
| **Used by** | SpektraLab 2.0, which bundles `spklib-host` as a helper; see its `API-SPEC-library-helper.md` for how the app runs its library on this API |

A change to a method's params or result, or to an error code, bumps `abi`. A change to the library
file format bumps `schema`. Libraries are migrated forward on open, and an older SDK refuses a newer
library rather than misreading it.

**0.2.0** (schema 2, abi still 1: every change is an addition an `abi` 1 client can ignore): watched
folders (`folders.*`), `images.facets`, `images.describe`, more filter keys, `title` and `created_at`
on collections, SpektraLab's own fixed rows (root, All Images, Recent Imports, Trash, In Catalog) and a
Recent Imports album per import. A 0.1.0 helper refuses a library a 0.2.0 helper has opened.


One surface, two transports:

- **C ABI.** `spklib_call(ctx, "images.list", "{...}", &out)` returns `SPKLIB_OK` with the result in
  `out`, or `SPKLIB_ERROR` with the error in `out`. Free `out` with `spklib_free`. See
  `include/spklib/spklib.h`.
- **stdio.** `spklib-host [--store DIR]` writes `{"event":"ready","version":…,"protocol":1}`, then
  reads one JSON request per line and writes one reply per line, in order:

```
→ {"id": 7, "method": "images.list", "params": {"library": "L1", "limit": 50}}
← {"id": 7, "result": {"total": 9657, "offset": 0, "images": [...]}}
← {"id": 8, "error": {"code": "read_only", "message": "Capture One's albums are read-only", "data": null}}
```

`host.ping` and `host.shutdown` are the host's own. stderr is a log. For one-shot use:
`spklib-host probe <catalog>` and `spklib-host call <method> '<params>'`.

Error codes: `not_found`, `invalid_params`, `read_only`, `conflict`, `source_busy`, `unsupported`,
`exists`, `in_use`, `ambiguous`, `cross_volume`, `io`, `internal`, `unknown_method`.

Handles: `library.create`, `library.open` and `capture_one.link` return `{"library": "L1", "info": …}`.
Every method marked *L* below takes that handle as `library`. Opening a library that is already open
returns its handle.

Times: `captured_local` is the camera's wall clock, without a zone (`2024-01-08T23:39:04`). The other
times are UTC (`…Z`).

## Methods

`sdk.methods` returns this table from the binary itself.

| Method | L | Params | Result | Writes |
|---|---|---|---|---|
| `sdk.version` | | | `version`, `abi`, `schema`, `capture_one: {write_back: false, previews: false}` | |
| `capture_one.probe` | | `path`: package, `.cocatalogdb`, session folder or `.cosessiondb` | `document` (catalog/session), `document_uuid`, `format`, `journal_mode`, `fingerprint`, `counts`, `supported`, `reason` | nothing (snapshot in a temp dir) |
| `capture_one.link` | | `source`; `path`? (default `<store>/Linked/<uuid>.spkcatalog`) | handle, `info`, `sync` report | the mirror |
| `library.create` | | `path`, `kind` (`catalog`/`session`), `name`?, `folders`? (session role → folder name) | handle, `info` | a new library |
| `library.open` / `library.close` | / L | `path` / — | handle, `info` / `closed` | |
| `library.info` | L | | `uuid`, `kind`, `name`, `path`, `origin {kind, path, document_uuid, format, synced_at, fingerprint, write_back}`, `counts` | |
| `library.sync` | L | `force`?, `source`? (the catalog's new place) | per table `{added, updated, unchanged, removed}`, `collections.parents_repaired`, `album_members`, `warnings`, `ms`; or `{skipped: true}` | the mirror |
| `collections.list` | L | `include_removed`? | `collections[]`: `uuid`, `parent`, `kind`, `name`, `title` (the name, or a folder's own name where Capture One stores none), `position`, `origin`, `editable` (SpektraLab's albums, groups, projects, smart albums only), `created_at` (UTC; a Capture One Recent Imports album's is its import time), `image_count` (null when not computable), `folder`, `criteria`, `supported` | |
| `images.list` | L | `collection`?, `recursive`? (folders), `filter`?, `images`? (uuids), `sort` (`captured` `imported` `name` `rating` `added`), `descending`?, `offset`?, `limit`?, `include_trashed`?, `include_removed`?, `check_files`?, `full`? | `total`, `offset`, `images[]`: `uuid`, `path`, `file_name`, `kind`, `rating`, `color_tag`, `trashed`, `captured_local`, `origin`, `variants[] {uuid, position, rating, color_tag, edit_revision, origin}`, `exists`?, and with `full` the EXIF (`camera {make, model, lens}`, `exposure {…}`), size, `imported_at`, `added_at` and `described` | |
| `images.facets` | L | as `images.list`: `collection`?, `recursive`?, `filter`?, `include_trashed`? | `total`, `rating[] {value, count}`, `color_tag[]`, `kind[]`, `captured {years[], months[], days[], undated}`, `camera[] {value (model), make, count}`, `lens[]`, `keyword[]`, `edited {yes, no}`, `undescribed`. Each facet is counted with every filter key but its own | |
| `images.describe` | L | `items[] {image, metadata}` (the `images.add` metadata keys) | `updated`, `results[] {image, error?}`. Fills what is given; `read_only` per item for Capture One's | SpektraLab's rows |
| `image.get` | L | `image` | one image (full, with `exists`), `variants[]` with `local`, `capture_one {rating, color_tag, metadata, geometry, adjusted}`, `edit_revision`, `albums[]` | |
| `images.lookup` | L | `paths[]` | `results[] {path, found, match {image, variant, origin_state}}` | |
| `locations.list` | L | | `locations[] {uuid, base, path, volume, relinked, origin, image_count, online}` | |
| `location.relink` | L | `location`, `path` (new folder) or `null` (undo); `all_matching`? (default true) | `path`, `online`, `relinked[]` | SpektraLab only |
| `edit.get` | L | `variant` | `revision` (0 = none), `edit` (object or null), `modified_at` | |
| `edit.put` | L | `variant`, `edit` (object), `base_revision` | `revision`, `modified_at`; `conflict` with `data.revision` | edit |
| `edit.delete` | L | `variant` | `deleted` | edit |
| `meta.set` | L | `variants[]` or `images[]` (→ first variant); `rating` 0–5 / null, `color_tag` 0–7 / null | `updated` | overrides |
| `images.trash` | L | `images[]`, `trashed` true / false / null | `updated`, `files_moved: false` | override |
| `variant.create` | L | `image`, `from_variant`? | `variant`, `position`, `edit_copied` | variant |
| `images.add` | L | `items[]`: path or `{path, metadata}`; `mode` `reference`/`copy`; `collection`? (a SpektraLab album); `destination`? (relative folder for copies); `recent_import`? (default true) | `added`, `results[] {path, image, variant, existing, stored_at?, error?}`, `recent_import`? (the new Recent Imports album) | rows; files only with `copy`, never overwriting |
| `collection.create` | L | `kind` (album/group/project/smart_album), `name`, `parent`?, `criteria`? (an `images.list` filter) | `collection` | |
| `collection.update` | L | `collection`, `name`? `parent`? `criteria`? `position`? | | |
| `collection.delete` | L | `collection`, `recursive`? | `deleted`, `images_deleted: 0` | |
| `collection.add` / `.remove` | L | `collection`, `images[]` | `added` / `removed` | |
| `folders.add` | L | `path`, `recursive`? (default true), `skip[]`? (folder names never walked, at any depth) | `folder`, `collection` (its folder row), `path`, `sync` (as one entry of `folders.sync`). The first look is an import (a Recent Imports album). Refused in a Capture One mirror (`read_only`), inside another library's package or inside this library (`invalid_params`) | rows |
| `folders.list` | L | | `folders[] {folder, path, recursive, skip, collection, online, image_count, added_at, synced_at, last_sync}` | |
| `folders.sync` | L | `folder`? (one; default every watched folder, and a session's own folder) | `folders[]`, `session`?, totals `added`, `moved`, `removed`, `restored`. Each: `path`, `offline`, `seen`, `added`, `moved`, `moved_files[]`, `removed`, `restored`, `updated`, `unchanged`, `ms` | rows only, never files |
| `folders.remove` | L | `folder` | `removed`, `images_removed: 0` (the images stay) | |
| `session.folders` | L | | `root`, `folders[] {location, path, relative_path, role, outside_session, exists, image_count}` | |
| `session.scan` | L | | `seen`, `added`, `missing` | rows |
| `session.move` | L | `images[]`, `to` (`capture`/`selects`/`output`/`trash` or a relative folder), `on_conflict` `fail`/`suffix` | `moved[] {image, from, to}`, `conflicts[]` | **files**, SpektraLab sessions only |
| `session.restore` | L | `images[]`, `on_conflict`? | as `session.move` | **files** |
| `session.trash.plan` | L | | `files[] {image, path}` | |
| `session.trash.commit` | L | `removed_paths[]` (files the app moved to the macOS Trash) | `dropped`, `refused[]` | rows |

Filter keys: `min_rating`, `max_rating`, `ratings[]` (any of), `color_tags[]`, `kinds[]` (`raw` `raster`
`movie` `other`), `text` (file or display name), `captured_from`, `captured_to` (`YYYY-MM-DD[THH:MM:SS]`,
half-open), `camera_model`, `camera_models[]` and `lenses[]` (any of; `null` in the list is "not known"),
`keywords[]` (any of), `has_edit`, `described`.

## Watched folders (0.2.0)

A SpektraLab catalog or session can reference a folder and keep it in step: `folders.add` adds every
picture under it by reference (RAW and raster files; hidden files, `skip` names and other libraries'
packages are not walked), and each `folders.sync` compares the folder with the library:

- a file the library has, still there: unchanged (its size updated if it changed);
- a new file whose name and size are those of exactly one file that went: **moved**, the row follows it,
  so its variants, ratings, albums and edit stay with it;
- any other new file: added, by reference, undescribed (`described: false`) until the app sends what its
  header says (`images.describe`); the helper never opens an image;
- a file that went: its row is marked `origin_state: "removed"` (hidden, kept with its edit), and is
  **restored** if the file comes back;
- a folder that is not there (an unplugged disk): `offline: true`, and nothing is marked.

`folders.sync` never writes a file. A session's own folder is synced the same way, with every watched
folder, when no `folder` is named.

