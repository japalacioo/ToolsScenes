#!/usr/bin/env bash
# Compila tools-scenes.plugin (Tools Scenes) para macOS como binario universal
# (Apple Silicon + Intel) y genera el instalador .pkg y el ZIP manual en dist/.
#
# Requisitos: Xcode Command Line Tools, Homebrew (cmake ninja), OBS.app en
# /Applications (Qt y cabeceras de enlace) y en .deps/: obs-studio (31.0.0),
# qt6-mac (obs-deps qt6 universal) y deps-mac (obs-deps, para simde).
set -euo pipefail
export LC_ALL=en_US.UTF-8 LANG=en_US.UTF-8
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEPS="$ROOT/.deps"
OBS_APP="${OBS_APP:-/Applications/OBS.app}"
FW="$OBS_APP/Contents/Frameworks"
VERSION="$(sed -n 's/^project(tools-scenes VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
BUILD="$ROOT/build-macos-release"
STUBS="$BUILD/stubs"
DIST="$ROOT/dist"
IDENT="io.github.japalacioo.tools-scenes"

# --- 1) Stubs .tbd universales de libobs y obs-frontend-api ---------------
# El OBS.app local puede ser de una sola arquitectura; el plugin solo necesita
# saber qué símbolos exportan. En ejecución usa la libobs del OBS del usuario.
make_tbd() { # binario, install-name, salida
	local bin="$1" name="$2" out="$3"
	{
		echo "--- !tapi-tbd"
		echo "tbd-version: 4"
		echo "targets: [ x86_64-macos, arm64-macos ]"
		echo "install-name: '$name'"
		echo "current-version: 1"
		echo "compatibility-version: 1"
		echo "exports:"
		echo "  - targets: [ x86_64-macos, arm64-macos ]"
		echo "    symbols: ["
		nm -gU "$bin" | awk '{print $NF}' | sort -u | sed "s/.*/      '&',/" | sed '$ s/,$//'
		echo "    ]"
		echo "..."
	} > "$out"
}
rm -rf "$STUBS"
mkdir -p "$STUBS/libobs.framework"
make_tbd "$FW/libobs.framework/libobs" "@rpath/libobs.framework/Versions/A/libobs" "$STUBS/libobs.framework/libobs.tbd"
make_tbd "$FW/obs-frontend-api.dylib" "@rpath/obs-frontend-api.dylib" "$STUBS/obs-frontend-api.tbd"

# --- 2) Compilación universal ---------------------------------------------
cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
	"-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
	-DOBS_SOURCE_DIR="$DEPS/obs-studio" -DQT_DIR="$DEPS/qt6-mac" -DOBS_SIMDE_DIR="$DEPS/deps-mac/include" \
	-DOBS_APP_DIR="$OBS_APP" -DOBS_MAC_STUBS="$STUBS" -DSCENE_PREVIO_DEBUG=OFF
cmake --build "$BUILD"
PLUGIN="$BUILD/tools-scenes.plugin"
lipo -info "$PLUGIN/Contents/MacOS/tools-scenes"

# Firma ad-hoc de ambas arquitecturas (sin certificado de desarrollador).
codesign --force --deep --sign - "$PLUGIN"
codesign --verify --deep --strict "$PLUGIN"

# --- 3) Paquetes --------------------------------------------------------------
mkdir -p "$DIST"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# ZIP manual (copiar tools-scenes.plugin a ~/Library/Application Support/obs-studio/plugins)
ZIP="$DIST/ToolsScenes-$VERSION-macOS-manual.zip"
rm -f "$ZIP"
ditto -c -k --keepParent "$PLUGIN" "$ZIP"

# Instalador .pkg en la carpeta de plugins del usuario actual.
mkdir -p "$WORK/root"
ditto "$PLUGIN" "$WORK/root/tools-scenes.plugin"
pkgbuild --analyze --root "$WORK/root" "$WORK/component.plist" >/dev/null
# Evita que Installer "reubique" el plugin en otra copia encontrada en el disco.
plutil -replace 0.BundleIsRelocatable -bool NO "$WORK/component.plist"
plutil -replace 0.BundleOverwriteAction -string upgrade "$WORK/component.plist"
pkgbuild --root "$WORK/root" --component-plist "$WORK/component.plist" \
	--install-location "Library/Application Support/obs-studio/plugins" \
	--scripts "$ROOT/installer/macos/scripts" \
	--identifier "$IDENT" --version "$VERSION" "$WORK/tools-scenes.pkg" >/dev/null
# Instalación en la carpeta del usuario, como el usuario (sin contraseña de
# administrador y con los archivos a su nombre).
pkgutil --expand "$WORK/tools-scenes.pkg" "$WORK/expanded"
sed -i '' 's/auth="root"/auth="none"/' "$WORK/expanded/PackageInfo"
rm -f "$WORK/tools-scenes.pkg"
pkgutil --flatten "$WORK/expanded" "$WORK/tools-scenes.pkg"
sed "s/@VERSION@/$VERSION/g" "$ROOT/installer/macos/distribution.xml" > "$WORK/distribution.xml"
mkdir -p "$WORK/resources"
for f in welcome.html conclusion.html; do
	sed "s/@VERSION@/$VERSION/g" "$ROOT/installer/macos/$f" > "$WORK/resources/$f"
done
cp "$ROOT/LICENSE" "$WORK/resources/license.txt"
PKG="$DIST/ToolsScenes-$VERSION-macOS-Instalador.pkg"
rm -f "$PKG"
productbuild --distribution "$WORK/distribution.xml" --resources "$WORK/resources" \
	--package-path "$WORK" "$PKG"

ls -la "$ZIP" "$PKG"
shasum -a 256 "$PKG" "$ZIP"
