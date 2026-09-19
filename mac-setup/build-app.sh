#!/usr/bin/env bash

set -euo pipefail

## Mac Setup script
## 1. do the build, will not be called from this script
## 2. call this script
## 3. an .app will be packed
#
# This script should be run from a jhbuild environment

if [ -z "${UNDER_JHBUILD:-}" ]; then
  echo 'WARNING: this command needs to be run within a jhbuild-like environment.'
  echo 'The build will proceed, but do not be surprised if it fails!'
fi

if [ $# -eq 0 ]; then
  echo 'Please provide the path of your gtk installation'
  exit 1
fi

# go to script directory
cd "$(dirname "$0")" || exit

# delete old app, if there
echo "clean old app"
rm -rf "./StudySzn Marker.app"
rm -rf "./StudySznMarker.app"
rm -rf "./.StudySzn Marker.app"
rm -rf "./.StudySznMarker.app"

echo "prepare gtk-mac-bundler"
GTK_MAC_BUNDLER_VENV="$PWD"/gtk-mac-bundler-venv
GTK_MAC_BUNDLER="$GTK_MAC_BUNDLER_VENV"/bin/gtk-mac-bundler
GTK_MAC_BUNDLER_REVISION="fbc6ffd1590cec6fef6e17ec19b6aa00ce01ca6d"
if [ ! -f "$GTK_MAC_BUNDLER" ]; then
  echo "Existing gtk-mac-bundler not found, installing a new one in $GTK_MAC_BUNDLER_VENV"
  python3 -m venv "$GTK_MAC_BUNDLER_VENV"

  if [ ! -d gtk-mac-bundler ]; then
    git init gtk-mac-bundler
    git -C gtk-mac-bundler remote add origin https://gitlab.gnome.org/GNOME/gtk-mac-bundler.git
  fi
  git -C gtk-mac-bundler fetch --depth=1 origin "$GTK_MAC_BUNDLER_REVISION"
  git -C gtk-mac-bundler checkout --detach FETCH_HEAD
  pushd gtk-mac-bundler
  # The install script assumes it will install to some global path
  # Instead, we isolate it to a virtualenv.
  cat <<EOF > bundler/launcher.py
from . import main as bundler_main
import sys

def main():
    bundler_main.main(sys.argv[1:])
EOF

  cat <<EOF > pyproject.toml
[build-system]
requires = ["setuptools == 84.0.0"]
build-backend = "setuptools.build_meta"

[project]
name = "gtk-mac-bundler"
version = "0.0.0"

[project.scripts]
gtk-mac-bundler = "bundler.launcher:main"
EOF

  echo 'include bundler/*.sh' > MANIFEST.in
  popd
  "$GTK_MAC_BUNDLER_VENV"/bin/pip install ./gtk-mac-bundler
fi

if [ ! -f "$GTK_MAC_BUNDLER" ]; then
  echo "error: gtk-mac-bundler does not appear to be installed!"
  echo "try deleting $GTK_MAC_BUNDLER_VENV and rerunning this script"
  exit 1
fi

echo "create package"

SOURCE_GTKDIR="$1/inst"
export BREWPREFIX="${BREWPREFIX:-$(brew --prefix)}"
[ ! -d "$SOURCE_GTKDIR" ] && echo "$SOURCE_GTKDIR doesn't exist!" && exit 1

# gtk-mac-bundler invokes helper shell scripts that do not quote every path.
# Stage the install tree under a private path without spaces and use a
# no-space CFBundleName while bundling, then restore the teacher-facing name.
BUNDLE_STAGING_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/studyszn-marker-bundle.XXXXXX")"
trap 'rm -rf "$BUNDLE_STAGING_ROOT"' EXIT
cp -R "$SOURCE_GTKDIR" "$BUNDLE_STAGING_ROOT/inst"
export GTKDIR="$BUNDLE_STAGING_ROOT/inst"

"$GTK_MAC_BUNDLER" xournalpp.bundle
mv "./StudySznMarker.app" "./StudySzn Marker.app"
/usr/libexec/PlistBuddy -c "Set :CFBundleName StudySzn Marker" \
  "./StudySzn Marker.app/Contents/Info.plist"

echo "Replace Ctrl by Meta in mainmenubar.xml"
sed -i -e 's/Ctrl/Meta/g' "./StudySzn Marker.app/Contents/Resources/ui/mainmenubar.xml"

echo "Relocate bundled libraries"
python3 ./relocate-bundle.py "./StudySzn Marker.app"

echo "Sign application bundle"
CODESIGN_IDENTITY="${STUDYSZN_CODESIGN_IDENTITY:--}"
python3 ./sign-bundle.py "./StudySzn Marker.app" "$CODESIGN_IDENTITY"

echo "Create zip"
rm -f "StudySzn-Marker.zip"
ditto -c -k --sequesterRsrc --keepParent "StudySzn Marker.app" "StudySzn-Marker.zip"

if [ -n "${APPLE_NOTARY_KEYCHAIN_PROFILE:-}" ]; then
  if [ "$CODESIGN_IDENTITY" = "-" ]; then
    echo "APPLE_NOTARY_KEYCHAIN_PROFILE requires a Developer ID signing identity." >&2
    exit 1
  fi
  echo "Notarize and staple application"
  xcrun notarytool submit "StudySzn-Marker.zip" \
    --keychain-profile "$APPLE_NOTARY_KEYCHAIN_PROFILE" --wait
  xcrun stapler staple "StudySzn Marker.app"
  rm -f "StudySzn-Marker.zip"
  ditto -c -k --sequesterRsrc --keepParent "StudySzn Marker.app" "StudySzn-Marker.zip"
  spctl --assess --type execute --verbose=2 "StudySzn Marker.app"
else
  echo "NOTE: prototype is not notarized; set APPLE_NOTARY_KEYCHAIN_PROFILE for a public release."
fi

echo "finished"
