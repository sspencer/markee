#!/usr/bin/env bash
set -euo pipefail

# Build a native macOS Markee.app bundle in build/Markee.app
#
# Usage:
#   ./packaging/build-macos-app.sh [--version 1.0.0] [--agent] [--standalone]
#
# Options:
#   --version <ver>  Bundle version string (default: 1.0.0)
#   --agent          Set LSUIElement=true (hide Dock icon; ideal for borderless widget mode)
#   --standalone     Copy libraylib dylib into Contents/Frameworks and rewrite install name

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="1.0.0"
LSUI_ELEMENT="false"
STANDALONE="false"
OUT_APP="${ROOT_DIR}/build/Markee.app"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --version) VERSION="$2"; shift 2 ;;
    --agent) LSUI_ELEMENT="true"; shift ;;
    --standalone) STANDALONE="true"; shift ;;
    --out) OUT_APP="$2"; shift 2 ;;
    -h|--help)
      sed -n '3,12p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *) echo "Unknown option: $1" >&2; exit 1 ;;
  esac
done

if [[ ! -x "${ROOT_DIR}/markee" ]]; then
  echo "Building markee..."
  make -C "${ROOT_DIR}" release
fi

rm -rf "${OUT_APP}"
mkdir -p "${OUT_APP}/Contents/MacOS" "${OUT_APP}/Contents/Resources" "${OUT_APP}/Contents/Frameworks"

cp "${ROOT_DIR}/markee" "${OUT_APP}/Contents/MacOS/markee"
chmod 0755 "${OUT_APP}/Contents/MacOS/markee"

if [[ "${STANDALONE}" == "true" ]]; then
  RAYLIB_DYLIB="$(otool -L "${OUT_APP}/Contents/MacOS/markee" | awk '/libraylib/ {print $1; exit}')"
  if [[ -n "${RAYLIB_DYLIB}" && -f "${RAYLIB_DYLIB}" ]]; then
    DYLIB_BASE="$(basename "${RAYLIB_DYLIB}")"
    cp "${RAYLIB_DYLIB}" "${OUT_APP}/Contents/Frameworks/${DYLIB_BASE}"
    chmod 0644 "${OUT_APP}/Contents/Frameworks/${DYLIB_BASE}"
    install_name_tool -id "@executable_path/../Frameworks/${DYLIB_BASE}" "${OUT_APP}/Contents/Frameworks/${DYLIB_BASE}"
    install_name_tool -change "${RAYLIB_DYLIB}" "@executable_path/../Frameworks/${DYLIB_BASE}" "${OUT_APP}/Contents/MacOS/markee"
    echo "Bundled ${DYLIB_BASE} into ${OUT_APP}/Contents/Frameworks/"
  fi
fi

cat > "${OUT_APP}/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key>
  <string>en</string>
  <key>CFBundleExecutable</key>
  <string>markee</string>
  <key>CFBundleIdentifier</key>
  <string>com.markee.display</string>
  <key>CFBundleInfoDictionaryVersion</key>
  <string>6.0</string>
  <key>CFBundleName</key>
  <string>Markee</string>
  <key>CFBundleDisplayName</key>
  <string>Markee</string>
  <key>CFBundlePackageType</key>
  <string>APPL</string>
  <key>CFBundleShortVersionString</key>
  <string>${VERSION}</string>
  <key>CFBundleVersion</key>
  <string>${VERSION}</string>
  <key>LSMinimumSystemVersion</key>
  <string>12.0</string>
  <key>NSHighResolutionCapable</key>
  <true/>
  <key>LSUIElement</key>
  <${LSUI_ELEMENT}/>
</dict>
</plist>
EOF

cp "${ROOT_DIR}/packaging/com.markee.display.plist" "${OUT_APP}/Contents/Resources/"
echo "Built ${OUT_APP} (version ${VERSION}, LSUIElement=${LSUI_ELEMENT})"
