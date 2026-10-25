# spklib design

spklib is SpektraLab's library SDK, introduced with SpektraLab 2.0.0. It tells the app where images are
and keeps the data around them: libraries (catalogs and sessions), images, variants, ratings, colour
tags, collections, and SpektraLab's own edits. It is native C++20 with a one-function C ABI, plus a
stdio helper that serves the same methods. It is MIT licensed and lives in its own repository; the
GPL desktop app ships its helper as a bundled binary.

## The boundary

| spklib does | spklib never does |
|---|---|
| Read Capture One catalogs, through a snapshot | Write, lock or create anything in a Capture One catalog or session |
| Keep SpektraLab's own catalogs and sessions | Decode an image, read EXIF from a file, render a preview, keep a cache |
| Store the app's edit JSON per variant, verbatim | Interpret an edit |
| Move files between a *SpektraLab* session's folders, when asked | Delete an image file (the app moves files to the macOS Trash) |

The worst case of this boundary is "the catalog cannot be read". Nothing it does can damage a Capture
One library.

## One schema, three kinds of library

A **SpektraLab catalog** is a package, `<Name>.spkcatalog/Library.spkdb` (SQLite, WAL), with
`Originals/` for images copied in. A **SpektraLab session** is a folder:
`<Name>/<Name>.spksession` (SQLite, rollback journal, `synchronous=FULL`) beside
`Capture/ Selects/ Output/ Trash/`, with every path stored relative to the folder so it moves as one
(docs/SESSION-DESIGN.md). A **linked Capture One catalog** is a SpektraLab catalog whose rows were
copied from Capture One. By default it lives at `<store>/Linked/<document UUID>.spkcatalog`, and the
app passes `store` = `~/Library/Application Support/SpektraLab/Libraries`.

All three use one schema (`src/library.cpp`, `kSchema`), shaped like Capture One's so a catalog maps
row for row:

```
path_location  folder: base (absolute | library | origin), root, path, volume, relinked_dir
image          file: location, name, kind, size, EXIF, capture/import time, trashed
variant        per image: position, Capture One's rating/tag/IPTC/geometry, SpektraLab's rating/tag
edit           per variant: SpektraLab's JSON, revision, modified_at
collection     tree: root, project, group, album, smart_album, folder, all_images, trash, ...
collection_image  album membership
keyword, meta, sync_log, session_folder (roles), fs_intent (crash-safe moves)
watched_folder, variant_keyword (schema 2); copy_source, copy_member (schema 3: a copy's link back)
```

### Who owns which column

Every row carries `origin` (`local` or `capture_one`), `origin_key` (Capture One's UUID, or its
`Z_PK` for locations and keywords) and `origin_state` (`present` or `removed`).

- **A sync** writes only origin-owned columns, and only on `capture_one` rows. A column is updated
  only when its value differs, so a forced resync of an unchanged catalog reports 0 updates (tested on
  the real catalog).
- **The API** writes only SpektraLab-owned data: `edit`, `variant.rating` and `variant.color_tag`
  (overrides), `image.trashed` (override), `path_location.relinked_dir`, and `local` collections and
  memberships. Changing a Capture One album, renaming it or adding to it answers `read_only`.
- **Effective values** are SpektraLab's if set, else Capture One's: rating
  `COALESCE(rating, origin_rating, 0)`, trashed `COALESCE(trashed, origin_trashed)`. Setting
  `null` clears the override and hands the field back to Capture One.
- **A row gone from Capture One** becomes `origin_state = 'removed'`. It is hidden from lists, still
  readable by `image.get`, and its edit survives. If the row returns, it is `present` again.

This split is why there is no write-back at 2.0.0 and none is needed to be correct. A later two-way
feature would add a write path for the override columns. It would be a separate design with its own
tests against a scratch catalog, never the owner's.

## Capture One, one way

`capture_one.link {source}` probes the catalog (snapshot → schema check → document UUID), then
creates or reopens the mirror and syncs. `library.sync` repeats the sync. It is skipped when size,
mtime and the SQLite file-change counter are the ones last synced. docs/CAPTURE-ONE-CATALOG.md has the
table-by-table diagnosis and the snapshot procedure.

Capture One **sessions** are probe-only: `capture_one.probe` reads a `.cosessiondb`, and
`capture_one.link` refuses it with `unsupported`. Their per-image state lives in `.cos` sidecars, and
their schema could not be checked against a real session (SESSION-DESIGN.md §2, §6).

## Capture One, copied (0.3.0)

A copy is the other way to bring a Capture One catalog in: `capture_one.copy {source, path}` makes a
new `.spkcatalog`, saved wherever the person chooses, that is an ordinary SpektraLab catalog (origin
`none`) and not a mirror. Every row is `origin = 'local'` and SpektraLab's to change. The copied
rating, colour tag and trash flag are written into SpektraLab's own columns (`rating`, `color_tag`,
`trashed`), not as overrides of Capture One's, so a copy behaves like any library SpektraLab made.
API-SPEC.md, "Copies", says what it holds.

A copy remembers where it came from, so `capture_one.update` can bring in Capture One's later work:

- **`copied_from`** in `meta` (as JSON): the package, its document UUID, format, fingerprint,
  `copied_at`, `updated_at`. An update finds a moved catalog by a new `source` whose document UUID is the
  copy's, and refuses another catalog with `conflict`. Images inside the Capture One package keep
  `base = 'origin'` and resolve against `copied_from.path`, so they follow the package.
- **`copy_source`** (schema 3): per copied row, Capture One's key, the row it became (by uuid, never a
  rowid, which SQLite can reuse), and the **baseline**: the values Capture One had at the copy or the
  last update. Capture One's key is deliberately *not* in `origin_key`: that column means "Capture One's
  key" only on `origin = 'capture_one'` rows, which a sync selects and marks by it; a copied row is not
  one. A side table also outlives the row, so a copied album SpektraLab deleted is a tombstone an update
  does not bring back.
- **`copy_member`**: album memberships as Capture One had them, by keys.

**SpektraLab wins.** For each tracked field an update compares three values: Capture One's now, the
baseline, and the copy's. Capture One's change is applied only if the copy still holds the baseline;
if SpektraLab changed the field too, its value stays and is counted in `conflicts_kept`. The baseline
becomes Capture One's value either way, so a conflict is reported once, and a later Capture One change
is judged against the value Capture One last had. Membership is judged per album: Capture One's
additions are added; its removals are applied only to an album whose members are still exactly the
baseline's. A row gone from Capture One is marked `removed` (never deleted, its memberships kept) and is
present again if it returns; `copy_source.removed` records that the update marked it, so a row a
watched-folder sync marked removed (its file went) is not revived by Capture One. An update runs in one
transaction; `dry_run` rolls it back and reports what it would have done. It is skipped, without a
snapshot, while the catalog's fingerprint is the one last read.

`mirror` on a copy carries a linked catalog's SpektraLab data (rating and colour-tag overrides, trash
flags, edits) into the copy. The baseline stays Capture One's, so those values count as SpektraLab's
changes and win at updates. A mirror's own albums, imported images and extra variants are not carried.

## Edits

`edit.put {variant, edit, base_revision}` stores the app's JSON object (today's `Sidecar.swift`
encoding) and returns `revision + 1`. A `base_revision` that is not the current one answers
`conflict` with the current revision, so two windows, or the app and the CLI, cannot overwrite each
other silently. `variant.create {image, from_variant}` makes a SpektraLab variant with a copy of an
edit: the way to keep two prints of one negative.

Migrating today's sidecars is the app's job, and is one call per frame. `images.lookup {paths}` finds
the variant for a file, then `edit.put` stores its `Sidecar.load(for:)` with `base_revision: 0`.

## Native libraries

`images.add {items, mode}` adds files by reference (in place) or by copy (into the catalog's
`Originals/yyyy/mm/dd` or the session's `Capture`), and never overwrites. Each item may carry the
metadata the app's decoder already read (`captured_local`, `width`, camera, exposure). spklib reads no
image file. Folders become `folder` collections, nested by path. Smart albums store the
`images.list` filter (`min_rating`, `color_tags`, `kinds`, `text`, `captured_from/to`,
`camera_model`, `has_edit`) and are evaluated by the same code.

## Paths

The resolved path is `relinked_dir` if set, otherwise `root/path` (absolute), `library/path`
(relative to the SpektraLab library) or `<Capture One package>/path` (Capture One's in-catalog
images). `images.lookup` compares in Unicode NFC, so a path the app typed and a path Capture One stored
match in either normalisation (macOS; identity elsewhere until a port needs it).
`location.relink {location, path}` is "locate this folder": the changed prefix is applied to every
location under the same old prefix, so a renamed volume is found in one step.

## Sessions

See SESSION-DESIGN.md. Implemented for SpektraLab's own sessions:

- **Roles.** The folder roles live in `session_folder`, so a renamed folder keeps its role.
- **One writer.** SQLite's exclusive locking mode lets one SpektraLab hold the session; a second
  answers `in_use`, and the same process gets its existing handle.
- **Moves and restore.** `session.move` and `session.restore` use `renamex_np(RENAME_EXCL)` and never
  overwrite. Each move writes an `fs_intent` row first, and the next open finishes a move a crash cut
  short.
- **Scan.** `session.scan` picks up files a tethered camera or Finder put into the folder.
- **Trash.** `session.trash.plan` and `session.trash.commit` hand emptying the Trash to the app, and
  spklib forgets only files that really left.

## Licensing, and why the helper exists

spklib is MIT. The SpektraLab desktop app is GPL-3.0-or-later (a fork of spektrafilm) and the mobile
app is MIT. MIT code may be combined with either, so the licence no longer decides how the app
reaches the SDK. The architecture does:

- **`spklib-host`**, a separate executable, is what the desktop uses. The app bundles it in
  `Contents/Helpers` and talks to it over pipes. The library then has its own process: a crash in it
  cannot take a render down with it, the app's main actor never waits on SQLite, and the Windows and
  Linux ports can drive the same binary the same way. `tests/host_smoke.swift` exercises this path
  from Swift.
- **`libspklib.a` and the C header** are for where a child process is impossible, as on iOS.

The method table is the same in both, by construction: the host is a client of the C ABI.

## Open items

- Capture One sessions: verify the `.cosessiondb` schema and the `.cos` keys on a throwaway session
  (SESSION-DESIGN.md §6), then implement the read side: snapshot + `.cos`, a files pass, a full pass.
- Mapping Capture One's crop/rotation into SpektraLab's geometry: format unverified, kept raw.
- Unicode NFC off Apple platforms; a Windows/Linux port needs it before it ships.
- Capture One smart-album criteria are stored but not evaluated.
- Native sessions: no favourites, no folder renaming or creation methods yet (SESSION-DESIGN.md §5),
  and no `outside_session` warnings beyond `session.folders` reporting it.
