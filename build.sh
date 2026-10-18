#!/bin/bash
# build.sh -- the SDK, its host and its tests. No CMake, no package manager:
# clang++ and the system's SQLite.
#
#   ./build.sh            libspklib.a + spklib-host + spklib-tests (native arch)
#   ./build.sh universal  the same for arm64 + x86_64, lipo'd
#   ./build.sh test       build, then run the tests (SPKLIB_C1_CATALOG for the real one)
set -euo pipefail
cd "$(dirname "$0")"
MODE="${1:-native}"
OUT=build
mkdir -p "$OUT"
VERSION="$(cat VERSION)"
CXXFLAGS=(-std=c++20 -O2 -g -Wall -Wextra -Wno-unused-parameter -Iinclude -Isrc -Ithird_party
          -DSPKLIB_VERSION="\"$VERSION\"" -DSPKLIB_FIXTURES="\"$PWD/tests/fixtures\"" -mmacosx-version-min=13.0)
SRCS=(src/sqlite.cpp src/capture_one.cpp src/library.cpp src/api.cpp)
LIBS=(-lsqlite3 -framework CoreFoundation)

build_arch() {
  local arch="$1" dir="$OUT/$1"
  mkdir -p "$dir/obj"
  local objs=()
  for s in "${SRCS[@]}"; do
    local o="$dir/obj/$(basename "${s%.cpp}").o"
    clang++ -arch "$arch" "${CXXFLAGS[@]}" -c "$s" -o "$o"
    objs+=("$o")
  done
  rm -f "$dir/libspklib.a"
  libtool -static -o "$dir/libspklib.a" "${objs[@]}" 2>/dev/null
  clang++ -arch "$arch" "${CXXFLAGS[@]}" host/main.cpp "$dir/libspklib.a" "${LIBS[@]}" -o "$dir/spklib-host"
  clang++ -arch "$arch" "${CXXFLAGS[@]}" tests/tests.cpp "$dir/libspklib.a" "${LIBS[@]}" -o "$dir/spklib-tests"
}

case "$MODE" in
  native|test)
    build_arch "$(uname -m)"
    ln -sf "$(uname -m)/libspklib.a" "$OUT/libspklib.a"
    ln -sf "$(uname -m)/spklib-host" "$OUT/spklib-host"
    ln -sf "$(uname -m)/spklib-tests" "$OUT/spklib-tests"
    if [ "$MODE" = test ]; then "$OUT/spklib-tests"; fi
    ;;
  universal)
    build_arch arm64
    build_arch x86_64
    mkdir -p "$OUT/universal"
    lipo -create "$OUT/arm64/libspklib.a" "$OUT/x86_64/libspklib.a" -output "$OUT/universal/libspklib.a"
    lipo -create "$OUT/arm64/spklib-host" "$OUT/x86_64/spklib-host" -output "$OUT/universal/spklib-host"
    lipo -info "$OUT/universal/spklib-host"
    ;;
  *) echo "usage: $0 [native|universal|test]" >&2; exit 2 ;;
esac
