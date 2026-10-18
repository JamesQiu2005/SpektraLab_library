# spklib — Session design

Status: proposal, 2026-10-18. Scope: SpektraLab v2.0.0 library. Companion to the catalog design
(snapshot rule, origin/local-owned columns, opaque `edit` JSON, `origin_state='removed'`). Everything
here mirrors that design; where this document names a table, column or store path that the catalog
design names differently, the catalog design wins and this document should be updated to match.

> **Implemented in 0.1.0** (see `docs/PROTOCOL.md` for the names as built):
>
> - **Native sessions.** §3.1 layout; §3.2 `session_folder` roles, paths relative to the session root;
>   §3.4 rollback journal with `synchronous=FULL`.
> - **One writer.** Enforced by SQLite's exclusive locking mode rather than flock plus an open row: an
>   flock on the db collides with Apple's SQLite. A second opener gets `in_use`.
> - **§5 methods.** `session.move`, `session.restore`, `session.scan` (the files pass),
>   `session.trash.plan` and `.commit`. `session.create` and `session.open` are `library.create` /
>   `library.open` with `kind: "session"`. Error codes are the SDK's lowercase set (`read_only`,
>   `in_use`, `cross_volume`, `ambiguous`, …), not the `E_*` names below.
> - **Capture One sessions.** `capture_one.probe` reads a `.cosessiondb`, and `capture_one.link`
>   refuses one (`unsupported`) until §6 has been run on a real session.
>
> **Not yet:** favourites, `session.folders.create`/`set_role`, `.cos` reading, link IDs and unlink.

## 0. How to read the evidence tags

Every claim about Capture One carries one tag:

| Tag | Meaning |
|---|---|
| **[D]** | Documented by Capture One (support.captureone.com), source in §7 |
| **[C]** | Community or third-party source (forum, ExifTool, PRONOM), source in §7 |
| **[X]** | Observed in the owner's real Capture One 16.8 *catalog* (schema dump); not yet seen in a session |
| **[A]** | Assumed. Needs the check in §6 before code depends on it |

No Capture One session exists on this Mac, so nothing about the `.cosessiondb` schema is **[X]**.

---

## 1. What a Capture One session is

A session is a self-contained, file-based project document: one master folder, named after the
session, holding the session database `<Name>.cosessiondb` and four default subfolders, `Capture`,
`Selects`, `Output` and `Trash` [D]. The user can rename or relocate the four at creation, and since
16.6.0 any folder in the session can be given the Capture, Selects, Output or Trash role by
right-clicking it. The Library tool shows those four as "Shortcuts" and shows the full folder tree under
"Session Folders" [D]. The default parent location is `~/Pictures` [D]. Renaming the
`.cosessiondb` file renames the session [D], so **the folder name and the db name can differ**.

Images are always **referenced, never imported into a container**: the Library in a session is "a
filtered file explorer" [D], and images can stay in an existing folder structure with no import at all
[D]. Tethered captures land in the current Capture folder. The Capture folder may be any folder, even
one outside the session on another drive, though Capture One recommends keeping it inside the
session [D]. Moving a file to Selects (Cmd+J) moves it on disk [D]. Deleting it moves it to the session
Trash folder, not the system Trash. Images in the Trash are read-only in Capture One, and emptying the
Trash deletes the source files permanently [D].

**Where the data lives.** This is the main difference from a catalog. Per-image adjustments **and
metadata** (rating, colour tag, keywords, IPTC) are written to sidecar files beside each image:
`<image folder>/CaptureOne/Settings<NNN>/<image file name>.cos`. NNN is the version of the engine that
wrote them, for example `Settings120` for 12 and `Settings166`/`Settings1665` for 16.6 and 16.6.5 [D].
In the official wording, outside the XMP-sidecar workflow "the metadata adjustments are recorded along
with the rest of the adjustments in .cos files (when in a Session) or in the database (when in a
Catalog)" [D]. The same `CaptureOne/` folder holds `Cache/Proxies` (`.cop` previews, `.cof` focus masks)
and `Cache/Thumbnails` (`.cot`), and `Settings*/` also holds `.comask` and `.icm` files [D]. The
`.cosessiondb` holds the structure: Session Albums, Smart Albums and Session Favorites "are stored in
the .cosessiondb file" [D]. It also holds an index of the special folders and Favorites that drives
Smart Albums and duplicate detection [D]. Two consequences are documented:

- If the db is replaced by a blank one, adjustments survive and only Smart Albums and Favorites are
  lost [D] (albums too, by implication [A]).
- An image folder can be moved without its db and still shows its edits, "providing the CaptureOne
  sub-folder ... remains within the source images' folder" [D].

So the `.cos` files are the authority for per-image state, and the db is structure plus an index or
cache.

**Why people use sessions instead of catalogs.** Tethered or single-shoot work, client hand-off,
portability (copy the folder, open it on another Mac or on Capture One mobile 3.0+), no import step,
and a fixed Capture → Selects → Output → Trash flow [D]. A session can be imported into a catalog later,
but a catalog cannot be turned into a session [D]. Since 16.8.2 a session can be shared live on a LAN
("Multi-User Sessions", beta, macOS). Collaborators then work on "a synchronised local copy of the
session" that keeps changing under them [D].

**Database facts.** `.cosessiondb` is SQLite. The support article on "Database connection failed"
treats `.cocatalogdb` and `.cosessiondb` identically: SQLite writes a journal file next to either, and
the article checks either with `sqlite3 "file:...?mode=ro" "PRAGMA integrity_check;"` [D]. When opened,
Capture One "places a writelock on the entire database", and copying a session that is still open
copies the writelock too [D]. Where a session keeps that writelock is not documented; a catalog keeps it
inside its package [A]. Each upgrade to a newer dot release rewrites the db and leaves
`<Name>.cosessiondb.backup` beside it [D]. ZENTITIES in the owner's catalog lists `Session` (26),
`Catalog` (27), `SessionFolderCollection` (52), `TempFolderCollection` (4) and
`VirtualFolderCollection` (8). Together with the shared support guidance, this suggests that sessions
and catalogs share one Core Data schema, told apart by `ZDOCUMENTCONTENT.ZDOCUMENTTYPE` (1 = catalog
[X]; the session value is unknown [A]).

---

## 2. Reading a Capture One session (read-only, always)

### 2.1 Rules

1. **Nothing is ever written inside a Capture One session folder or any folder it references.** No
   lock files, no SpektraLab sidecars, no thumbnails, no `CaptureOne/` folders, no xattrs, no renames.
   Every method in §5 that changes the filesystem is refused for a Capture One–origin library with
   `E_READ_ONLY_ORIGIN`.
2. **The `.cosessiondb` is only ever read through a snapshot**, using the same procedure as the catalog:
   1. `stat` the db, keeping size, mtime and inode.
   2. Refuse with `E_C1_BUSY` if a non-empty `<db>-journal`, `<db>.journal` or `<db>-wal` exists. The
      support article spells it ".journal"; check all three.
   3. Copy the bytes with a plain `O_RDONLY` read, or with `clonefile` on APFS, into
      `<store>/linked/<link_id>/snapshots/`.
   4. `stat` again. Size, mtime and inode must be unchanged and the journal still absent. Otherwise
      discard the copy and retry, three tries with backoff (0.5 s, 2 s, 5 s), then report `E_C1_BUSY`.
   5. Open the copy as `file:<copy>?mode=ro&immutable=1`, then run `PRAGMA quick_check`, the
      required-table and column check, the `ZVERSIONINFO.ZCOMPATIBLEVERSION` range check, and a
      document-type check that the file really is a session.
   6. Keep the last good snapshot. A failed snapshot never replaces it.

   `sqlite3_open` is never called on the original, not even read-only. SQLite may create `-shm` or
   `-journal` files beside a database it opens. Ignore `*.cosessiondb.backup` files. They are Capture
   One's upgrade backups, not the live db.
3. **A writelock means "Capture One has it open", not "refuse".** If a writelock exists (location to be
   confirmed in §6), report `c1_open: true` and go on. The journal check and the before/after `stat`
   are what guarantee a consistent copy. With tethered shooting this is the normal state, and §4.3
   covers it.
4. **`.cos` files are read in place, read-only.** They are small XML files and there are thousands of
   them, so copying each one first adds nothing. For each file: `open(O_RDONLY)`, `fstat`, read it
   whole, `fstat` again. If size or mtime changed, or the XML does not parse (Capture One may be
   mid-write), skip it this pass and keep the previous value. Store `cos_mtime`, `cos_size` and
   `cos_sha256` in the mirror so unchanged files are skipped next time. spklib reads only the metadata
   keys in §2.4, never adjustments. Adjustments stay out of scope, exactly as for catalogs.
5. **spklib decides nothing about image formats.** The app passes the accepted extension list
   (`sync.options.extensions`). spklib never opens an image file and only `stat`s it.

### 2.2 What a session's root and paths are

- **Session root** = the directory that contains the chosen `.cosessiondb`. **Session name** = the db
  file stem, which may differ from the folder name [D].
- `ZPATHLOCATION` (columns `ZMACROOT`, `ZRELATIVEPATH`, `ZISRELATIVE`, `ZVOLUME`, `ZWINROOT` [X]):
  - With `ZISRELATIVE=1`, a path resolves as `<session root>/<ZRELATIVEPATH>` [A]. This is what
    Capture One's "Relative to session" folder link means [D], so moving the folder is safe.
  - With `ZISRELATIVE=0`, a path resolves as `<ZMACROOT>/<ZRELATIVEPATH>`. That is an outside folder
    such as a Capture folder or Favorite on another drive, and it may be offline.
  - The separator Capture One uses in `ZRELATIVEPATH` is unknown [A]: accept both `/` and `\`.
  - The mirror stores each resolved path as session-relative where possible. It keeps the raw bytes and
    also an NFC-normalised comparison key, because macOS volumes differ in normalisation.
- The `.cos` for image `<dir>/<file.ext>` is `<dir>/CaptureOne/Settings<NNN>/<file.ext>.cos` [D]. The
  extension is kept, so `A.NEF` and `A.JPG` have separate `.cos` files [D]. Several `Settings<NNN>`
  folders may sit side by side (old folders left behind by upgrades [A]); use the newest. Parse NNN
  as a version, not an integer:
  - A leading `1` with ≥3 digits means a two-digit major (`120`→12.0, `166`→16.6, `1665`→16.6.5).
  - Otherwise the major is one digit (`70`→7.0).
  - Compare (major, minor, patch).
- An image that Capture One has never shown has no `CaptureOne/` folder and no `.cos` [D]. Treat that
  as "no Capture One metadata", which is not an error.

### 2.3 Which images belong to the session

The db is **not** a complete list of images. It indexes the special folders and Favorites [D]. Folders
added only through Next Capture Location, or only browsed through System Folders, are not indexed [D].
Files copied in with Finder while Capture One is closed reach the db only when Capture One next scans
the folder [A]. The image set is therefore the **union** of two sources:

- **db rows**: `ZIMAGE` joined to `ZPATHLOCATION` and `ZVARIANT` in the snapshot. `ZISTRASHED` exists in
  the catalog schema [X]; whether sessions set it is unknown [A].
- **filesystem scan**: walk the session root, plus every outside folder that holds a role or is a
  Favorite. Skip `CaptureOne/` directories, dot-files, `*.cosessiondb*`, and files outside the
  extension list. Skip files modified less than 2 s ago or whose size changed since the previous scan,
  because a tethered transfer may still be in progress.

Each image's role comes from its folder: `capture`, `selects`, `output`, `trash`, `favorite` or
`other`. An image under the Trash-role folder gets `c1_trashed=1`, the session equivalent of the
catalog's `ZISTRASHED`.

### 2.4 What is read, and from where

| Entry | Source | v2.0.0 | Notes |
|---|---|---|---|
| Session name, root, `ZVERSIONINFO.ZFORMAT`/`ZCOMPATIBLEVERSION` | file name; snapshot | **essential** | compat range gates the read |
| Document UUID (`ZDOCUMENTCONTENT.ZDOCUMENTUUID`) | snapshot | **essential** | link identity (§4.1); present in sessions [A] |
| Path locations | `ZPATHLOCATION` | **essential** | §2.2 |
| Images: file name, location, class (17 RAW / 19 raster / 6 movie [X]), UUID, width/height | `ZIMAGE` ∪ fs scan | **essential** | fs-only images have no UUID until a later snapshot has them |
| Variants: UUID, image, index (127/255/383 → 1/2/3 [X]) | `ZVARIANT` | **essential** | fs-only images get a synthetic variant 1 |
| Rating, colour tag | `.cos` first, then the db's combined-layer metadata (`ZVARIANTMETADATA` on `ZVARIANTLAYER` Z_ENT 20 [X]) | **essential** | precedence in §2.5 |
| Folder roles: Capture / Selects / Output / Trash | snapshot (`ZCOLLECTION`, probably `SessionFolderCollection` 52 [A]); fallback: default folder names under the root | **essential** | |
| Session Favorites | snapshot (`ZCOLLECTION`, entity to be identified [A]) | **essential** | folders; contents come from the fs scan |
| Session Albums (regular) | `ZCOLLECTION` Z_ENT 2 + `ZIMAGEINCOLLECTION` [X for catalogs] | **essential** | |
| EXIF summary (`ZEXP_*`, `ZCAMERA_*`, `ZISO`) | `ZIMAGE` | optional | for sorting and filtering; the app can read EXIF itself |
| Keywords | `.cos` / `ZKEYWORD` nested set [X] | optional | |
| IPTC caption, copyright, etc. | `.cos` / `ZVARIANTMETADATA` | optional | |
| Smart Albums | `ZCOLLECTION` Z_ENT 5, `ZSMARTCRITERIA` | optional | stored raw, never evaluated at v2.0.0 |
| Album groups / projects, sort order, Next Capture Naming | snapshot | optional | |
| All adjustments (`.cos` adjustment keys, `ZVARIANTLAYER` tool columns, `.comask`, `.icm`) | — | **ignored** | out of scope by decision |
| `CaptureOne/Cache/**` (`.cop`, `.cot`, `.cof`, Windows `.bc`) | — | **ignored** | never opened |
| `.cosessiondb.backup`, `-journal`, `-wal` | — | **ignored** | except the journal presence check |
| XMP sidecars (`.xmp`, written by Capture One only with Auto Sync = Full [D]) | — | ignored at v2.0.0 | candidate fallback later |
| EIP packages (`.eip`, a ZIP of RAW + `.cos` [C]) | — | ignored at v2.0.0 | listed as unsupported files |
| Capture Pilot / Live / Multi-User share state | — | **ignored** | |

### 2.5 Rating and colour-tag precedence (Capture One side)

`c1_rating` and `c1_color_tag` are origin-owned, as in the catalog:

1. The value parsed from the image's newest `.cos`, if the file exists, parses and holds the key.
   `c1_meta_source='cos'`.
2. Otherwise the value from the snapshot's combined layer. `c1_meta_source='db'`.
3. Otherwise NULL.

Why `.cos` wins: Capture One says session metadata is recorded in `.cos` [D], and that replacing the db
keeps the image state [D]. A community report from the Capture One 7 era says "the rankings and all
other settings are held in the CaptureOne settings file (.cos)" [C]. The effective value is
`local ?? c1`, unchanged from the catalog. The `.cos` key names for rating, colour tag and keywords are
**not documented anywhere**. What is known is the shape: XML, root `<IMG>`, properties written as
`<E K="name" V="value"/>`, and an early `Author` key [C]. The parser is therefore table-driven, with key
names filled in from the check in §6 (step 6).

How variants are stored in `.cos` (one file holding several variants, or one file per variant) is
unknown [A]. Until the check in §6 settles it, `.cos` values apply to variant 1 only, and later
variants take the db value.

---

## 3. A SpektraLab session (native)

### 3.1 On-disk layout

```
Wedding_2026-10-18/                    session root; the folder is the document
  Wedding_2026-10-18.spksession        SQLite library db, kind='session', origin='native'
  Capture/                             default capture/import destination
  Selects/
  Output/                              default export destination
  Trash/                               session trash; "empty" sends to the macOS Trash (§5)
```

- The folder names are a template. The user can rename them at creation, and roles live in the db, not
  in the names. The app picks the default parent (`~/Pictures`, as Capture One does).
- **No other files.** No hidden SpektraLab folder, no per-image files, no lock file (§3.4). The only
  extra files that ever appear are SQLite's transient `-journal` during a write.
- Opening a folder means opening the single `*.spksession` it contains. If there are none or several,
  the result is `E_NOT_A_SESSION` or `E_AMBIGUOUS`.

### 3.2 Schema

The schema is the catalog's (`path_location`, `image`, `variant`, `collection`, `collection_image`,
`keyword`, `edit`, plus the library meta table), with three things specific to sessions:

- Library meta: `kind='session'`, `origin='native'`, `schema_version`, `session_uuid` (minted at
  creation), `created_by_app_version`.
- `session_folder(role TEXT PRIMARY KEY CHECK(role IN ('capture','selects','output','trash')),
  path_location_id)`. `capture` is the current capture/import destination, and can be any folder in
  the session, as in Capture One.
- `collection.kind='favorite'` for folder shortcuts, alongside `album`. Smart albums are not in v2.0.0.

**All paths are relative to the session root.** `path_location.is_relative=1`, and `relative_path`
uses `/` separators with the raw bytes kept. A folder outside the session is allowed: it is stored with
`is_relative=0`, an absolute root and the volume UUID, and is flagged `outside_session=1`, so the app
can warn that it will not travel with the folder. Image identity on the filesystem follows the existing
rule in `Sidecar.swift` (`Source.matches`), checked in this order:

1. Relative path. This survives copying the folder to another disk.
2. Volume + inode + size. This survives a Finder rename or move within one disk.
3. Never content. Byte-identical files are not the same photograph, a lesson `Sidecar.swift` records
   from failing tests.

The `edit` table is keyed by `variant_id`, so moving or renaming a file never touches its edit.

### 3.3 Per-image JSON sidecars beside images: recommendation **no** (v2.0.0)

Do not write per-image JSON beside images, even inside SpektraLab's own session folder. The db, which
lives *in* the session folder, is the single store. Reasons:

1. **The owner has already rejected sidecars beside images.** `Sidecar.swift` records that writing
   `_DSC2949.NEF.spektra.json` next to RAWs was objected to "in plain terms", which is why the store
   moved to Application Support. A session folder is still a folder of the user's photographs.
2. **The portability sidecars would buy is already there.** Capture One needs `.cos` files because its
   images often live outside any session (System Folders browsing [D]). A SpektraLab session's db
   travels with the folder, and its paths are relative. Moving or copying the whole folder keeps
   everything.
3. **One source of truth.** Sidecars plus a db means two copies of every edit and a precedence rule
   between them. Every crash between the two writes is a conflict, and every Finder copy of a single
   file duplicates an edit identity. The catalog design deliberately avoids that by keeping edits
   opaque in one table.
4. **The case sidecars handle best is moving one subfolder out of the session in Finder.** The edits
   are not lost in that case: they stay in the db and the images become `origin_state='removed'`.
   Moving the files back restores them, by path or by inode.

The cost: a single image or subfolder copied out of the session in Finder arrives elsewhere with no
SpektraLab edits. If hand-off of individual frames matters later, add an explicit
`session.export_sidecars {variant_ids, dest}` that writes self-contained JSON next to *exported copies*.
That is a deliberate act, never a background write. Images that already have legacy sidecars in
`~/Library/Application Support/SpektraLab/Sidecars/` (keyed by absolute path) are adopted once, when
first indexed, into an `edit` row if none exists. That migration belongs to the app and the catalog
design together; it is listed in §6.

### 3.4 Concurrency and portability of the `.spksession` db

- Use `journal_mode=DELETE` with `synchronous=FULL`, not WAL. Session folders often sit on external
  drives (exFAT) and move between Macs. WAL needs a shared-memory `-shm` file, leaves `-wal` and `-shm`
  files beside the db, and is unsafe on network filesystems. Rollback-journal mode leaves one file at
  rest. SpektraLab's own Application Support store can still use WAL.
- Locking uses SQLite's locks plus an `flock` on the db file while the session is open, and a
  `session_open(host, pid, app_version, opened_at)` row written at open and cleared at close.
  `session.open` refuses with `E_IN_USE {host, opened_at}` when the flock is held. If the row exists
  but the flock is free, the previous run crashed or the session was opened over a network: the open
  goes ahead and reports `stale_open`. This is Capture One's writelock idea, kept inside the db file.
- Network volumes are best-effort, matching Capture One's own stance [D]. `session.info` reports
  `volume_kind` so the app can warn.

---

## 4. Linking a Capture One session

### 4.1 Where the mirror lives, keyed by what

Placeholder layout (match the catalog's actual store layout):

```
~/Library/Application Support/SpektraLab/Library/
  linked/<link_id>/
    mirror.spklib                      same schema; kind='session', origin='capture_one'
    snapshots/<utc>-<size>.cosessiondb last good + current attempt only
```

Nothing goes in the Capture One session folder (§2.1). `link_id` is a UUID minted by spklib at link
time and is the primary key. The link's identity, stored in the mirror's meta table, records:

- `c1_document_uuid`: `ZDOCUMENTUUID`, if sessions have it [A]
- `last_root_path`, `db_file_name`
- `volume_uuid`
- `bookmark`: an opaque blob supplied by the app (a macOS bookmark; spklib only stores it)
- `last_snapshot`: size, mtime, sha256, `ZFORMAT`
- `last_full_sync_at`, `last_files_sync_at`

**Why not key by path:** sessions are made to be moved and renamed [D]. The flow when the app opens a
session:

1. When the app opens a `.cosessiondb`, it calls `session.link_capture_one`.
2. spklib takes a snapshot and reads the document UUID.
3. If an existing link has the same UUID at a different path, spklib returns
   `E_ALREADY_LINKED {link_id, last_root_path}` instead of creating a second mirror.
4. The app then calls `session.relink`, which updates the path and keeps every local rating and edit.

Without a UUID, the fallback match is volume UUID + db file inode. The last resort is the user choosing
the link.

### 4.2 Sync = snapshot pass + files pass

`session.sync {link_id, mode}` does one-way, origin-owned updates exactly like the catalog sync, with
two modes:

- **`mode:"files"`** (cheap, needs no db snapshot, safe while Capture One is writing):
  1. Run the fs scan of §2.3 over the role folders and Favorites from the last good snapshot (or over
     the default names if there is none yet).
  2. Insert new stable files as images with one synthetic variant, `origin_state='present'`, no
     Capture One UUID.
  3. Read changed `.cos` files (by mtime and size) and update `c1_rating`, `c1_color_tag` and
     `c1_meta_source`.
  4. Re-match files that changed path: path first, then inode + size. Matched files keep their
     variant_id and so their edits.
- **`mode:"full"`**: the snapshot pass (§2.1) followed by the files pass, then reconcile:
  - Adopt Capture One UUIDs onto fs-only images, matched by resolved path.
  - Update roles, Favorites and albums.
  - Fill `c1_*` values from the db wherever `.cos` gave nothing.
  - Mark images, variants and collections that are gone as `origin_state='removed'`. Never delete them.

**Missing does not always mean removed.** If the session root or a location's volume is not mounted,
the affected rows become `origin_state='offline'`. They do not become `removed`, and nothing is
re-matched. Files under the Trash role stay `present` with `c1_trashed=1`. Only a file that is gone from
disk and that no path or inode match recovers becomes `removed`.

### 4.3 Shooting into a linked session

While the user shoots tethered, Capture One has the db open and writes it after every capture, so
snapshots often hit `E_C1_BUSY`. The design does not depend on them:

- The **app** watches the current Capture folder (FSEvents; spklib does not run watchers) and calls
  `session.sync {mode:"files", folders:[capture]}`, debounced to about 1 s. New frames appear in
  SpektraLab within a second or two of landing, with no db read.
- The app calls `mode:"full"` when the session is opened, when the user asks, every few minutes while
  idle, and when Capture One closes (the writelock disappears or the journal is gone). A failed
  snapshot keeps the previous structure, and the files pass has already brought the images in.
- If the user changes the Capture folder in Capture One, spklib only learns of it at the next good
  snapshot. Until then the files pass also scans every role folder and Favorite, so frames in a new
  subfolder of `Capture/` are still found (the scan is recursive under the session root).
- A Multi-User Sessions collaborator copy [D] behaves the same way. It is just a session whose files
  arrive over the network instead of from a camera.

---

## 5. API surface (stdio JSON / C ABI)

Conventions follow the catalog API. `id` is a library handle that works for catalogs and sessions
alike. Listing, query, rating, colour tag and edit methods are the catalog's, unchanged
(`image.list`, `variant.set_rating`, `variant.set_color_tag`, `edit.get/put`, …). Edits and ratings
write only the library db: the session's own `.spksession`, or the Application Support mirror for a
linked Capture One session. The **Writes** column says what each method may modify:

- **store**: SpektraLab's Application Support store only
- **db**: the native session's `.spksession`
- **FS**: files or folders in the session folder; **native sessions only**, refused with
  `E_READ_ONLY_ORIGIN` on a Capture One–origin library

| Method | Params → result | Writes |
|---|---|---|
| `session.create` | `{parent_dir, name, folders?:{capture,selects,output,trash}}` → `{id, root, db_path}`; fails if `<parent>/<name>` exists | **FS** (mkdir root + 4 folders, create db) |
| `session.open` | `{path}` (the folder or its `.spksession`) → `{id, kind, origin, root, stale_open?}` | db (open row) |
| `session.close` | `{id}` | db |
| `session.info` | `{id}` → name, root, origin, schema/C1 version, roles, `c1_open`, `volume_kind`, last sync | — |
| `session.link_capture_one` | `{cosessiondb_path, bookmark?}` → `{id, link_id}` or `E_ALREADY_LINKED`; runs a full sync | store |
| `session.relink` | `{link_id, cosessiondb_path, bookmark?}` | store |
| `session.unlink` | `{link_id, keep_mirror:bool}`. A kept mirror becomes read-only history with edits intact | store |
| `session.sync` | `{id, mode:"files"\|"full", folders?, options:{extensions[]}}` → `{added, updated, moved, removed, offline, skipped_unstable, snapshot:"ok"\|"busy"\|"failed", c1_open}`. On a native session it rescans to pick up Finder changes | store / db |
| `session.folders.list` | `{id, parent?}` → tree of `{folder_id, rel_path, role?, favorite, outside_session, exists, image_count}` | — |
| `session.folders.create` | `{id, parent_folder_id, name}` | **FS** |
| `session.folders.set_role` | `{id, folder_id, role}` | db (native only; a Capture One role change would be write-back) |
| `session.favorites.add/remove` | `{id, folder_id}` | db (native only) |
| `session.import` | `{id, sources[], dest_folder_id?=capture, mode:"copy"\|"move", on_conflict:"fail"\|"suffix"}` → per-file results | **FS** |
| `session.move` | `{id, image_ids[], to:"selects"\|"trash"\|"capture"\|folder_id, on_conflict}` → `{moved[], conflicts[]}` | **FS** |
| `session.restore` | `{id, image_ids[]}` moves images out of Trash back to their recorded previous folder | **FS** |
| `session.trash.plan` | `{id}` → absolute paths of the files in Trash | — |
| `session.trash.commit` | `{id, removed_paths[]}` drops the rows after the **app** moved those files to the macOS Trash (`FileManager.trashItem`). spklib never unlinks image files | db |

Filesystem semantics for the **FS** methods:

- **Moves** are `renameatx_np(..., RENAME_EXCL)` within one volume and never overwrite. A name
  collision returns a conflict, or a ` (1)` suffix with `on_conflict:"suffix"`. A cross-volume move,
  for example into an outside Capture folder on another disk, returns `E_CROSS_VOLUME` at v2.0.0.
- **Crash safety**: each FS operation records an intent row `(op, from, to, state)` before touching
  the disk and updates paths after it. `session.open` reconciles unfinished intents by checking which
  path exists.
- **Edits** stay attached to `variant_id`, so moving a frame to Selects or Trash carries its edit,
  rating and tag automatically.

Errors: `E_READ_ONLY_ORIGIN`, `E_C1_BUSY`, `E_C1_UNSUPPORTED_VERSION`, `E_NOT_A_SESSION`,
`E_AMBIGUOUS`, `E_ALREADY_LINKED`, `E_IN_USE`, `E_CROSS_VOLUME`, `E_CONFLICT`, `E_OFFLINE`.

---

## 6. Open questions and verification plan

What the docs do not settle:

1. The `.cosessiondb` schema itself: whether `ZDOCUMENTTYPE` is set for sessions and to which value,
   whether `ZDOCUMENTUUID` exists, and which `ZCOLLECTION` entities carry Favorites and the four roles.
2. Whether `ZISRELATIVE` paths are relative to the session root, and which separator they use.
3. Where a session's writelock file lives.
4. The `.cos` key names for rating, colour tag and keywords, and how variants are laid out.
5. That `.cos` really overrides the db when the two disagree (documented by implication only).
6. Whether files added while Capture One is closed ever enter the db before Capture One reopens.
7. Whether old `Settings<NNN>` folders are left behind, and which one Capture One reads.
8. The legacy Application Support sidecar migration (§3.3), which is owned with the catalog design.
9. Whether a linked session may later be merged into a SpektraLab catalog. Capture One allows
   session → catalog [D]. This is out of v2.0.0.

**Verification script.** Run it in Capture One 16.8 on the owner's Mac with a **throwaway** session,
never a real one. Every db read uses the snapshot procedure.

1. File > New Session `SpkProbe` in `~/Pictures`, Blank template. Record
   `find ~/Pictures/SpkProbe -maxdepth 4` → layout, writelock location while open (Q3).
2. Import 6 files into Capture: 3 RAW, 1 JPEG, 1 TIFF, 1 HEIC. Click through all of them so the `.cos`
   files exist. Rate image A 2 stars. Give B a red colour tag. Add keyword `probe` to C. Create a
   second variant of D and rate that variant 4. Move E to Selects (Cmd+J). Delete F to the session
   Trash. Create the album `Alb` with A and B. Create `Capture/Day1`, make it a Favorite and set it as
   the Capture folder.
3. With Capture One still open: list the root for `-journal`, `-wal` and writelock files and note how
   often they appear. Then **quit** Capture One.
4. Snapshot the db and dump: `ZDOCUMENTCONTENT`, `ZVERSIONINFO`, `ZENTITIES` (or `Z_PRIMARYKEY`), all
   `ZCOLLECTION` rows with `Z_ENT` and name, `ZPATHLOCATION`, `ZIMAGE` (are all 6 present? where are E
   and F?), `ZVARIANT` for D, and `ZVARIANTMETADATA` rating and colour for A, B and D on each layer
   type (Q1, Q2, Q5).
5. Check the inferred mappings: A rated 2 → which column, on which layer. B red → which
   `ZCOLOR_TAG_INDEX`. F → `ZISTRASHED` or only its location.
6. `.cos` files: `ls -R` every `CaptureOne/` folder.
   - Grep A's `.cos` for the key holding `2`, B's for colour and C's for `probe`, giving the key names
     (Q4).
   - For D, see whether one `.cos` holds two variants or a second file exists (Q4).
   - Confirm E's and F's `.cos` files moved with them.
7. Precedence (Q5), only on a **duplicate** of the probe folder:
   - With Capture One closed, change A's rating value in the duplicate's `.cos`, reopen the duplicate,
     and see which value Capture One shows.
   - Separately, replace the duplicate's db with a blank one (Capture One's own repair procedure [D])
     and confirm the ratings survive.
8. Q6: with Capture One closed, copy 2 files into `Capture/` in Finder and snapshot (are they in
   `ZIMAGE`?). Open Capture One, quit, and snapshot again.
9. Portability: move the probe folder to another volume and rename the `.cosessiondb`. Snapshot and
   confirm that the paths resolve against the new root and that `ZDOCUMENTUUID` is unchanged (Q1, Q2,
   §4.1).
10. Q7: open a session made by an older Capture One, if one is available, in 16.8 and list its
    `Settings*` folders.

Each answer replaces an **[A]** above. Steps 4–6 are a prerequisite for writing the `.cos` parser and
the role mapping. Until then, those parts use the documented fallbacks: default folder names, and db
values only.

---

## 7. Sources

Capture One official (support.captureone.com, fetched through its help-centre API on 2026-10-18):

- The Complete Guide to Sessions — https://support.captureone.com/hc/en-us/articles/360003115137
- Session sidecar files explained: What's inside the CaptureOne folder — https://support.captureone.com/hc/en-us/articles/360002862137
- Metadata in XMP sidecar files (".cos files (when in a Session)") — https://support.captureone.com/hc/en-us/articles/360002544898
- How can I replace a corrupted/broken Session? — https://support.captureone.com/hc/en-us/articles/360002418977
- How can I rename a Session or a Catalog? — https://support.captureone.com/hc/en-us/articles/360013934438
- Sessions vs Catalogs: how they work and where to store them — https://support.captureone.com/hc/en-us/articles/30041173920029
- How Catalog and Session backups work — https://support.captureone.com/hc/en-us/articles/27502751010333
- Revert to a Previous Version and Restore Catalogs or Sessions — https://support.captureone.com/hc/en-us/articles/360002427218
- Fix the "Database connection failed" error — https://support.captureone.com/hc/en-us/articles/30493722534941
- "The catalog is currently in use by" (writelock) — https://support.captureone.com/hc/en-us/articles/360002387017
- Folders in Sessions — https://support.captureone.com/hc/en-us/articles/360002525898
- Session Favorites — https://support.captureone.com/hc/en-us/articles/360002554838
- Selects Folder and Selects Collection — https://support.captureone.com/hc/en-us/articles/360002524157
- Library Tool (16.6 Session Folders / Shortcuts) — https://support.captureone.com/hc/en-us/articles/360002797618
- Changing location for storing captured images — https://support.captureone.com/hc/en-us/articles/360002554438
- Creating capture folders from the Finder (Sessions) — https://support.captureone.com/hc/en-us/articles/360002553877
- Importing images into a Session — https://support.captureone.com/hc/en-us/articles/360002521938
- Accessing images stored on a network drive (Sessions) — https://support.captureone.com/hc/en-us/articles/360002523438
- Deleting images and their variants from Collections — https://support.captureone.com/hc/en-us/articles/360002547197
- Deleting images and variants from the Session Trash — https://support.captureone.com/hc/en-us/articles/360002546518
- The Cache folder in Capture One — https://support.captureone.com/hc/en-us/articles/30111725200413
- Complete Guide to Using Catalogs (Importing a Session) — https://support.captureone.com/hc/en-us/articles/360003108698
- Session and Catalog documents in Capture One mobile — https://support.captureone.com/hc/en-us/articles/30269627417373
- Multi-User Sessions (Beta) — https://support.captureone.com/hc/en-us/articles/36870089862813
- Capture One 16.6.5 release notes (Include Output Folder) — https://support.captureone.com/hc/en-us/articles/29341888893469
- Enhanced Image Package (EIP) overview — https://support.captureone.com/hc/en-us/articles/360002478617

Community / third party:

- "CP 7.1 move of files = loss of rating" (ratings in `.cos`) — https://support.captureone.com/hc/en-us/community/posts/360012141657
- "missing *.cosessiondb file" (only File > New Session creates the db) — https://support.captureone.com/hc/en-us/community/posts/360015025577
- "Any way to export collective metadata from C1?" (sqlite3 on ZIMAGE) — https://support.captureone.com/hc/en-us/community/posts/360012681597
- ExifTool `CaptureOne.pm` (COS is XML with `K`/`V` attributes; EIP is a ZIP with `CaptureOne/**/*.cos`) — https://github.com/exiftool/exiftool/blob/master/lib/Image/ExifTool/CaptureOne.pm
- PRONOM fmt/1725 and fmt/1857 (COS signature `<?xml version="1.0"?>\n<IMG>` … `<E K="Author"`) — https://www.nationalarchives.gov.uk/PRONOM/fmt/1725, https://www.nationalarchives.gov.uk/PRONOM/fmt/1857

In-repo: `filmify/modern_UI/Spektrafilm/Spektrafilm/Model/Sidecar.swift` (store location, the owner's
objection to sidecars beside images, and the file-identity rule).
