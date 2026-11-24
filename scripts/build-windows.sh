#!/usr/bin/env bash
# Compila tools-scenes.dll (Tools Scenes) para Windows x64 desde macOS y genera el instalador.
# Requisitos (Homebrew): llvm lld cmake ninja makensis xwin
# Dependencias en .deps/ (ver README): obs-studio (31.0.0), qt6-win, xwin
set -euo pipefail
export LC_ALL=en_US.UTF-8 LANG=en_US.UTF-8  # makensis falla con locale vacío
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEPS="$ROOT/.deps"
VERSION="$(sed -n 's/^project(tools-scenes VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
LLVM="$(brew --prefix llvm)/bin"
LLD="$(brew --prefix lld)/bin"

cmake -S "$ROOT" -B "$ROOT/build-windows" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/windows/clang-cl-cross.cmake" \
  -DXWIN_DIR="$DEPS/xwin" -DLLVM_BIN="$LLVM" -DLLD_BIN="$LLD" \
  -DOBS_SOURCE_DIR="$DEPS/obs-studio" -DQT_DIR="$DEPS/qt6-win" \
  -DCMAKE_BUILD_TYPE=Release -DSCENE_PREVIO_DEBUG=OFF
cmake --build "$ROOT/build-windows"

DIST="$ROOT/dist"
mkdir -p "$DIST"
DLL="$ROOT/build-windows/tools-scenes.dll"

# Instalador .exe
makensis -V2 -DVERSION="$VERSION" -DDLL_PATH="$DLL" -DDATA_DIR="$ROOT/data" \
  -DOUT_FILE="$DIST/ToolsScenes-$VERSION-Windows-x64-Instalador.exe" \
  "$ROOT/installer/tools-scenes.nsi"

# ZIP para instalación manual (OBS 33+: estructura nueva, no "heredada")
STAGE="$(mktemp -d)"
mkdir -p "$STAGE/tools-scenes/data"
cp "$DLL" "$STAGE/tools-scenes/"
cp -R "$ROOT/data/" "$STAGE/tools-scenes/data/"
rm -f "$DIST/ToolsScenes-$VERSION-Windows-x64-manual.zip"
(cd "$STAGE" && zip -qr "$DIST/ToolsScenes-$VERSION-Windows-x64-manual.zip" tools-scenes)
rm -rf "$STAGE"

ls -la "$DIST"
