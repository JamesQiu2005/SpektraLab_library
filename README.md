# spklib — SpektraLab's library SDK

Private. Not open source, and not part of the GPL `SpektraLab` repository.

spklib is the library behind SpektraLab 2.0: catalogs, sessions, and reading Capture One. It tells the
app where every image is and keeps what surrounds it: variants, ratings, colour tags, albums, folders,
and SpektraLab's own edits (the app's JSON, stored as-is). The app still decodes, renders and caches.
spklib never writes to a Capture One catalog.

```
./build.sh                 build/libspklib.a, build/spklib-host, build/spklib-tests (this Mac's arch)
./build.sh universal       arm64 + x86_64, lipo'd into build/universal/
./build.sh test            build and run the tests
SPKLIB_C1_CATALOG="/Volumes/disk/Capture One Catalog.cocatalog" ./build/spklib-tests
swift tests/host_smoke.swift build/spklib-host "<catalog>.cocatalog"
build/spklib-host probe "<catalog>.cocatalog"
```

You need Xcode's clang and the system SQLite. CMake and package managers are not used.
nlohmann/json 3.12.0 is vendored in `third_party/` (MIT).

| Document | What it covers |
|---|---|
| `docs/DESIGN.md` | The boundary, the schema, who owns which column, licensing |
| `docs/CAPTURE-ONE-CATALOG.md` | Capture One's tables: what 2.0 needs, what it keeps raw, what it ignores |
| `docs/SESSION-DESIGN.md` | Sessions, Capture One's and SpektraLab's, from Capture One's documentation |
| `docs/PROTOCOL.md` | Every method, for the C ABI and the stdio helper |

## Using it from SpektraLab

The app bundles `spklib-host` in `Contents/Helpers/` and talks to it over pipes. `tests/host_smoke.swift`
is that client in miniature. Linking `libspklib.a` instead is possible, through `include/spklib/spklib.h`
and one call. Whether the GPL desktop may do that is a licence decision (docs/DESIGN.md, Licensing).
The mobile app can link it.

The app passes `{"store": "~/Library/Application Support/SpektraLab/Libraries"}` when it creates the
context. A linked Capture One catalog's mirror then lives at `Linked/<document UUID>.spkcatalog`
inside that folder.
