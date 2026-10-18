# spklib — SpektraLab's library SDK

MIT licensed (`LICENSE`). A separate repository from the GPL
[SpektraLab](https://github.com/JamesQiu2005/SpektraLab) app, which ships this SDK's helper as a bundled
binary.

spklib is the library behind SpektraLab 2.0: catalogs, sessions, and reading Capture One. It tells the
app where every image is and keeps what surrounds it: variants, ratings, colour tags, albums, folders,
and SpektraLab's own edits (the app's JSON, stored as-is). The app still decodes, renders and caches.
spklib never writes to a Capture One catalog.

```
./build.sh                 build/libspklib.a, build/spklib-host, build/spklib-tests (this Mac's arch)
./build.sh universal       arm64 + x86_64, lipo'd into build/universal/
./build.sh test            build and run the tests
SPKLIB_C1_CATALOG="/path/to/Capture One Catalog.cocatalog" ./build/spklib-tests
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
| `API-SPEC.md` | The API: every method, param, result and error, for the C ABI and the stdio helper |

## Using it from SpektraLab

The app bundles `spklib-host` in `Contents/Helpers/` and talks to it over pipes. `tests/host_smoke.swift`
is that client in miniature, and SpektraLab's `API-SPEC-library-helper.md` is the app's side of it.
Linking `libspklib.a` instead works too, through `include/spklib/spklib.h` and one call (the iOS app
cannot start a child process, so it links).

The app passes `{"store": "~/Library/Application Support/SpektraLab/Libraries"}` when it creates the
context. A linked Capture One catalog's mirror then lives at `Linked/<document UUID>.spkcatalog`
inside that folder.

## Not affiliated

Capture One is a trademark of Capture One A/S. spklib is not made, endorsed or supported by Capture One.
It reads the catalog files Capture One writes, for interoperability, and never writes them.
`tests/fixtures/capture_one_16_8_schema.sql` is the table structure of a 16.8 catalog (column names and
entity names); it holds no images and no user data.
