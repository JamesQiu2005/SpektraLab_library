# What a Capture One catalog holds, and what SpektraLab 2.0 reads

Diagnosed 2026-10-18 on a real working catalog of about 10,000 images, written by
**Capture One 16.8.6.25 Pro Mac** (`ZVERSIONINFO`: format `16.8.6.25 Pro Mac`, compatible version
160800; an earlier row says the catalog was made by 16.4.5.29 Studio). Every number below was read from a
copy of the database; the catalog was not opened. `tests/tests.cpp` part 2 re-checks all of it on every
run (`SPKLIB_C1_CATALOG=… ./build.sh test`).

## The package

```
Capture One Catalog.cocatalog/
  Capture One Catalog.cocatalogdb          64 MB   SQLite 3, rollback journal (journal_mode=delete)
  Capture One Catalog.cocatalogdb.backup   62 MB   left by a Capture One upgrade; ignored
  Originals/<yyyy>/<mm>/<dd>/<n>/…        3.1 GB   images imported *into* the catalog (56 of 10,149)
  Adjustments/LAM/…, Adjustments/ICC/       97 MB   local-adjustment masks, embedded profiles; ignored
  Cache/Previews/…, Cache/Thumbnails/*.cot  18 GB   Capture One's previews; never read (the app renders its own)
```

The database is a Core Data–style store: `Z_PK` primary keys, `Z_ENT` entity numbers named in
`ZENTITIES`, foreign keys as plain integer columns. Two things are not Core Data's defaults, and both
cost a wrong answer if assumed:

- **Timestamps are Unix seconds**, not the 2001 reference date. `ZDOCUMENTCONTENT.ZDATECREATED`
  1729735821 is 2024-10-24.
- **Capture time is the camera's wall clock, stored as if UTC.** `ZIMAGE.ZEXP_DATE` for `DSC_0050.NEF`
  reads 2024-01-08 23:39:04Z, and Spotlight reads the same file's EXIF as 15:39:04Z, the same wall
  clock in UTC+8. Import and creation dates *are* real instants. An import album named
  "2025年11月4日 15:39" has `ZDATECREATED` 2025-11-05 04:39 in China time, which is 15:39 in New York,
  where the import happened. spklib returns capture time as a zone-less ISO string
  (`2024-01-08T23:39:04`) and import time with a `Z`.

The `Z_ENT` numbers are per model version. spklib maps entities by the **names** in `ZENTITIES`, never
by number.

## Essential for v2.0.0: read and mirrored

| Table | Columns | Why |
|---|---|---|
| `ZPATHLOCATION` (77) | `ZMACROOT`, `ZRELATIVEPATH`, `ZISRELATIVE`, `ZVOLUME`, `ZWINROOT` | *Where the images are.* Folder = `ZMACROOT` + `/` + `ZRELATIVEPATH` (leading `/` stripped); relative rows (`ZISRELATIVE=1`) are relative to the package (`Originals/2025/11/26/6816`). Volume roots keep their trailing spaces: `"/Volumes/NIKON Z 8   "` is a real mount-point name. |
| `ZIMAGE` (10,149) | `ZIMAGEUUID`, `ZIMAGEFILENAME`, `ZDISPLAYNAME`, `ZIMAGELOCATION`→location, `ZISINSIDECATALOG`, `ZISTRASHED`, `ZIMAGECLASSIFICATION`, `ZWIDTH`/`ZHEIGHT`, `ZFILE_SIZE`, `ZEXP_DATE`, `ZIMPORTDATE`, `ZCAMERA_MAKE`/`MODEL`/`LENS`, `ZISO`, `ZEXP_APERTURE`/`SHUTTERSPEED`/`FOCALLENGTH`/`EXPOSURECOMPENSATION`, `ZGPS*` | One row per file. UUIDs are unique and never null (10,149 distinct). Classification: **17 = RAW** (NEF 8,466, ARW 413, DNG 127, one JPG), **19 = raster** (JPG, PSD 95, TIFF, PNG), **6 = movie** (MOV, MP4); other values fall back to the extension. |
| `ZVARIANT` (10,152) | `ZVARIANTUUID`, `ZIMAGE`, `ZINDEX`, `ZCOMBINEDSETTINGS`, `ZADJUSTMENTLAYER`, `ZISMODIFIED` | Capture One's variants: SpektraLab edits are per variant, so each one gets an edit slot. `ZINDEX` is 128·n − 1 (127, 255, 383); spklib stores the position 1, 2, 3. Two images here have more than one variant. |
| `ZVARIANTLAYER` | `Z_PK`, `Z_ENT` | Only to reach the metadata. Each variant has a default (`VariantDefaultLayer`), adjustment (`VariantAdjustmentLayer`) and **combined** (`VariantCombinedLayer`) layer; 87 variants also have local-adjustment layers and 145 a style layer. |
| `ZVARIANTMETADATA` | `ZBASIC_RATING`, `ZCOLOR_TAG_INDEX`, `ZSTATUS_TITLE`, `ZCONTENT_HEADLINE`, `ZCONTENT_DESCRIPTION`, `ZCONTENT_KEYWORDS`, `ZCONTACT_CREATOR`, `ZSTATUS_COPYRIGHTNOTICE`, `ZBASIC_LABEL` | Joined on `ZLAYER` = the variant's **combined** layer, which carries what Capture One shows. (Rated variants: 264 on the combined layer, 261 on the adjustment layer, 3 on the default layer; the default layer also carries the camera's creator field on 9,722.) Ratings here: 0 on 9,751, 1★ 19, 2★ 5, 3★ 31, 4★ 189, 5★ 20, null on 137. No colour tags: 13 rows say 0, which spklib reads as "none". |
| `ZCOLLECTION` (35) | `ZUUID`, `Z_ENT`, `ZNAME`, `ZPARENT`, `ZCOLLECTIONINDEX`, `ZFOLDERLOCATION`, `ZSMARTCRITERIA`, `ZSORTORDER`; on the root row `ZLASTIMPORTSCOLLECTION`, `ZLASTCAPTURESCOLLECTION` | The tree the Library tool shows. See below. |
| `ZIMAGEINCOLLECTION` (10,894) | `ZCOLLECTION`, `ZIMAGE` | Copied for albums only (801 rows). Folder and virtual membership is derived; the numbers agree exactly (e.g. folder `/pictures/Nikon Z8` holds 4,619 rows, and 4,619 images have that location). |
| `ZDOCUMENTCONTENT` (1) | `ZDOCUMENTUUID`, `ZDOCUMENTTYPE`, `ZROOTCOLLECTION`, dates | The catalog's identity. The SpektraLab mirror is keyed by the document UUID, so a moved or renamed catalog finds its library again, and a different catalog put at the same path is refused. |
| `ZVERSIONINFO` | `ZFORMAT`, `ZCOMPATIBLEVERSION` | Reported. Support is decided by schema, not version: `capture_one::required_schema()` lists the 25 columns the reader needs, and a catalog missing any is refused with the list. |
| `ZKEYWORD` (0 here) | `ZNAME`, `ZPARENT` (nested set `ZLEFT`/`ZRIGHT`) | Mirrored when present. This catalog has none; keywords also live as text in `ZCONTENT_KEYWORDS`, all empty here. |

### The collection tree, decoded

| Entity (name in `ZENTITIES`) | spklib kind | In this catalog |
|---|---|---|
| `ProjectCollection`, the row `ZDOCUMENTCONTENT.ZROOTCOLLECTION` points at | `root` | "root" |
| `CatalogAllImagesCollection` | `all_images` | "全部图像": every image not in the trash (9,657) |
| `VirtualFolderCollection` the root's `ZLASTIMPORTSCOLLECTION` names | `recent_imports` | "最近导入", holding 10 import albums |
| `VirtualFolderCollection` the root's `ZLASTCAPTURESCOLLECTION` names | `recent_captures` | "最近拍摄", holding 2 albums |
| `TrashCollection` | `trash` | "回收箱": `ZISTRASHED = 1` (492) |
| `CatalogInternalImagesCollection` | `in_library` | "Catalog": `ZISINSIDECATALOG = 1` (56) |
| `AlbumCollection` | `album` | the 12 dated import/capture albums |
| `CatalogFolderCollection` | `folder` | 17, one per folder, with `ZFOLDERLOCATION` |
| `ProjectCollection` / `VirtualFolderCollection` (others) | `project` / `group` | none here; supported |
| `SmartCollection` | `smart_album` | none here; criteria stored, **not evaluated** |

The names are in the UI language. Roles come from the entity and the root's pointers, never from a name.

**Three folder rows point at parents that no longer exist** (5352, 5354, 5355; e.g.
`<disk>/backup/DCIM/<card folder>`). Capture One leaves these behind when an intermediate folder
row goes. spklib hangs each from its nearest ancestor folder by path, which is where Capture One shows
it, and reports `parents_repaired: 3`.

### Where the files are, today

9,581 of 10,149 images resolve to a file that exists. The 568 that do not are in folders no longer
there (552 in two folders on an external disk that were reorganised, and a few on a camera card and
the Desktop). spklib reports them (`exists: false`, `locations.list` → `online: false`). `location.relink`
points SpektraLab at a folder's new place without touching Capture One.

## Optional: read, kept raw, not mapped

| What | Where | Status |
|---|---|---|
| Crop, rotation, flip | combined layer `ZCROP` (`cx;cy;w;h`, 142 non-zero), `ZROTATION`, `ZFLIP`, `ZCROPASPECTRATIONAME` | Kept as `variant.capture_one.geometry`. **Not mapped to SpektraLab's crop.** The format is unverified: the centre looks like sensor pixels while width/height follow the display orientation. `ZROTATION` folds the camera orientation in (−90 on 4,181 portrait frames, −91.61 = portrait + 1.61° straighten). Mapping it is a separate, testable feature: compare against a Capture One export. |
| IPTC text | metadata `ZSTATUS_*`, `ZCONTENT_*`, `ZCONTACT_*` | Kept as `variant.capture_one.metadata`; read-only. |
| "Adjusted in Capture One" | `ZVARIANT.ZISMODIFIED` | Kept, so the app can show which frames were already worked on. |

## Ignored, deliberately

| What | Why |
|---|---|
| Every adjustment column in `ZVARIANTLAYER` (exposure, curves, WB multipliers `1.889;1.0;1.166`, colour editor, styles, masks, retouching) | Capture One's render pipeline, not SpektraLab's. SpektraLab is a physical simulation, and importing a digital grade would be a lie about what is on the "film". |
| `Cache/` (`.cot`, `.cop`, `.cof`), `ZVARIANT.ZCANVASSIZE`, `ZIMAGE.ZBITMAPHASH`, `ZSIDECARPATH` | Previews and caches stay the app's (the boundary). |
| `Adjustments/LAM`, `Adjustments/ICC`, `ZRETOUCHINGLAYER`, `ZVARIANTLAYER` local layers | Capture One masks and profiles. |
| `ZPROCESSHISTORY` (552), `ZENABLEDOUTPUTRECIPE`, `ZCAPTUREPILOT`, `ZSELECTEDVARIANTS`, `ZCOMPAREVARIANTS`, `ZDOCUMENTSETTING`, `ZSIDECAR` | Capture One's own export history and UI state. |
| `ZIMAGE.ZMULTISHOT_*`, `ZASSISTEDREVIEW_*`, `ZRAWFILE*HASH` | Not needed for 2.0. The quick hash could later identify a file across disks, but `Sidecar.swift` records why content is not identity: byte-identical files are not the same photograph. |

## How it is read without risk

1. `capture_one::locate` finds `<Name>.cocatalogdb` in the package.
2. `take_snapshot` refuses while a non-empty `-journal` exists (Capture One mid-write). It copies the
   file with plain `read()`s, re-checks size, mtime and the SQLite header's file-change counter, and
   runs `PRAGMA quick_check` on the copy. It retries up to 5 times, then answers `source_busy`.
3. The copy is opened `mode=ro&immutable=1` plus `PRAGMA query_only`, and deleted after the sync.
4. `capture_one.copy` and `capture_one.update` (0.3.0) read the catalog the same way, through the same
   snapshot; a hot journal answers `source_busy` to them too.
5. Nothing is written to or created in the package. The test proves it by stat-and-hash census of all
   41,342 entries before and after (`the Capture One package is exactly as it was`).

A sync copies 67 MB and mirrors 10k images in about 0.3 s (read 170 ms, write 90 ms on this Mac). An
unchanged catalog (same size, mtime and change counter) is skipped without a copy.
