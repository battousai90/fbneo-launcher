#!/usr/bin/env bash
# Build the downloadable packages for fbneo-launcher.
#
#   ./scripts/package.sh              # everything that can be built here
#   ./scripts/package.sh deb tgz      # only the named targets
#
# Targets: deb tgz appimage flatpak
# Output:  dist/
#
# The same script backs the GitHub Actions release job, so what you get locally
# is what users download.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build-package"
DIST="$ROOT/dist"
APP_ID="io.github.battousai90.FbneoLauncher"
ARCH="$(uname -m)"

VERSION="$(sed -n 's/^project(fbneo-launcher VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
[ -n "$VERSION" ] || { echo "!! cannot read version from CMakeLists.txt" >&2; exit 1; }

TARGETS=("$@")
[ ${#TARGETS[@]} -eq 0 ] && TARGETS=(deb tgz appimage flatpak)

say()  { printf '\n\033[1;35m==>\033[0m \033[1m%s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m!!\033[0m %s\n' "$*"; }
has()  { command -v "$1" >/dev/null 2>&1; }
wants() { for t in "${TARGETS[@]}"; do [ "$t" = "$1" ] && return 0; done; return 1; }

mkdir -p "$DIST"

# ── Configure & build once; every target consumes this tree ──────────────────
say "Building fbneo-launcher $VERSION ($ARCH)"
cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr >/dev/null
cmake --build "$BUILD" -j"$(nproc)"

# ── .deb and .tar.gz come straight from CPack ────────────────────────────────
if wants deb || wants tgz; then
  say "Packaging with CPack"
  gens=()
  wants deb && gens+=(DEB)
  wants tgz && gens+=(TGZ)
  ( cd "$BUILD" && cpack -G "$(IFS=';'; echo "${gens[*]}")" )
  find "$BUILD" -maxdepth 1 \( -name '*.deb' -o -name '*.tar.gz' \) -exec cp {} "$DIST/" \;
fi

# ── AppImage ─────────────────────────────────────────────────────────────────
# linuxdeploy bundles the binary with every shared library it needs; the GTK
# plugin additionally pulls in the pixbuf loaders, GIO modules and the icon
# theme, without which the app starts but renders nothing.
if wants appimage; then
  say "Building AppImage"
  TOOLS="$ROOT/.cache/appimage-tools"
  mkdir -p "$TOOLS"
  fetch() { # url dest
    [ -x "$2" ] && return 0
    echo "  fetching $(basename "$2")"
    curl -fsSL -o "$2" "$1" && chmod +x "$2"
  }
  BASE="https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous"
  GTKB="https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master"
  if fetch "$BASE/linuxdeploy-$ARCH.AppImage" "$TOOLS/linuxdeploy" &&
     fetch "$GTKB/linuxdeploy-plugin-gtk.sh"  "$TOOLS/linuxdeploy-plugin-gtk.sh"
  then
    APPDIR="$BUILD/AppDir"
    rm -rf "$APPDIR"
    DESTDIR="$APPDIR" cmake --install "$BUILD" >/dev/null

    # Runtime data sits at usr/share/fbneo-launcher inside the AppDir, which is
    # exactly the <bin>/../share layout AppContext already probes for.
    PATH="$TOOLS:$PATH" "$TOOLS/linuxdeploy" \
        --appdir "$APPDIR" \
        --plugin gtk \
        --desktop-file "$APPDIR/usr/share/applications/$APP_ID.desktop" \
        --icon-file "$APPDIR/usr/share/icons/hicolor/scalable/apps/$APP_ID.svg" \
        --output appimage 2>&1 | grep -viE 'warning|^\s*$' || true

    out="$(find "$ROOT" "$BUILD" -maxdepth 1 -name '*.AppImage' -newer "$BUILD/CMakeCache.txt" | head -1)"
    if [ -n "$out" ]; then
      mv "$out" "$DIST/fbneo-launcher-$VERSION-$ARCH.AppImage"
      chmod +x "$DIST/fbneo-launcher-$VERSION-$ARCH.AppImage"
    else
      warn "AppImage was not produced"
    fi
  else
    warn "could not download linuxdeploy — skipping AppImage"
  fi
fi

# ── Flatpak ──────────────────────────────────────────────────────────────────
# Built from the local checkout, so it needs no network for the app itself —
# only the GNOME runtime, which flatpak fetches once.
if wants flatpak; then
  say "Building Flatpak bundle"
  if has flatpak-builder; then
    flatpak install -y --noninteractive flathub \
        org.gnome.Platform//46 org.gnome.Sdk//46 >/dev/null 2>&1 || true
    flatpak-builder --force-clean --repo="$BUILD/fp-repo" \
        "$BUILD/fp-build" "$ROOT/packaging/$APP_ID.yml"
    flatpak build-bundle "$BUILD/fp-repo" \
        "$DIST/fbneo-launcher-$VERSION-$ARCH.flatpak" "$APP_ID"
  else
    warn "flatpak-builder not installed — skipping Flatpak"
    warn "  sudo apt install flatpak-builder"
  fi
fi

say "Done — dist/"
ls -lh "$DIST" | awk 'NR>1 {printf "    %-52s %s\n", $9, $5}'

# A checksum file so users can verify what they downloaded.
( cd "$DIST" && sha256sum -- * > SHA256SUMS 2>/dev/null || true )
