#!/bin/sh
# Regenerates include/sweetmtl_bridge.h from src/ with cbindgen
# (`cargo install cbindgen`, 0.29 or later), then commit the result. The CI job regenerates it too
# and fails when the committed header has drifted from the sources.
#
# cbindgen warns once about `backend::API_VERSION`, a private constant it will not put in the
# header; that is expected.
set -eu
H=$(cd "$(dirname "$0")" && pwd)
cd "$H"
cbindgen --crate sweetmtl-bridge --config cbindgen.toml --output include/sweetmtl_bridge.h
echo "wrote include/sweetmtl_bridge.h"
