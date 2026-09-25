#!/bin/sh
# Rebuild the pinned macOS SDK with coordinate AXPress disabled.
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
command -v cargo >/dev/null || { echo 'Rust/cargo is required.' >&2; exit 1; }
cua_build_dir=$(mktemp -d "${TMPDIR:-/tmp}/cua-build.XXXXXX")
trap 'rm -rf "$cua_build_dir"' EXIT HUP INT TERM
curl -fL https://codeload.github.com/trycua/cua/tar.gz/refs/tags/cua-driver-rs-v0.28.2 -o "$cua_build_dir/source.tar.gz"
mkdir "$cua_build_dir/source"
tar -xzf "$cua_build_dir/source.tar.gz" -C "$cua_build_dir/source" --strip-components=1
cd "$cua_build_dir/source"
patch -p1 < "$project_dir/vendor/cua/patches/0001-pixel-click-no-axpress.patch"
cd libs/cua-driver/rust
cargo build --release --locked -p cua-driver-sdk
cp target/release/libcua_driver_sdk.dylib "$project_dir/vendor/cua/lib/libcua_driver_sdk.dylib"
echo 'Rebuilt native SDK for the current architecture. Rebuild cua-shot before use.'
