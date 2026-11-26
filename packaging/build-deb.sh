#!/usr/bin/env bash
set -euo pipefail

# Build a Debian/Ubuntu .deb package in build/markee_<version>_<arch>.deb
#
# Usage:
#   ./packaging/build-deb.sh [--version 1.0.0]

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="1.0.0"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --version) VERSION="$2"; shift 2 ;;
    -h|--help)
      sed -n '3,6p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *) echo "Unknown option: $1" >&2; exit 1 ;;
  esac
done

if ! command -v dpkg-deb >/dev/null 2>&1; then
  echo "Error: dpkg-deb is required (run on Debian/Ubuntu)." >&2
  exit 1
fi

ARCH="$(dpkg --print-architecture)"
PKG_DIR="${ROOT_DIR}/build/deb-staging"
OUT_DEB="${ROOT_DIR}/build/markee_${VERSION}_${ARCH}.deb"

if [[ ! -x "${ROOT_DIR}/markee" ]]; then
  make -C "${ROOT_DIR}" release
fi

rm -rf "${PKG_DIR}"
mkdir -p "${PKG_DIR}/DEBIAN" \
         "${PKG_DIR}/usr/bin" \
         "${PKG_DIR}/usr/share/man/man1" \
         "${PKG_DIR}/usr/share/doc/markee/examples" \
         "${PKG_DIR}/usr/share/bash-completion/completions" \
         "${PKG_DIR}/usr/share/zsh/vendor-completions" \
         "${PKG_DIR}/usr/share/fish/vendor_completions.d" \
         "${PKG_DIR}/usr/lib/systemd/user"

install -m 0755 "${ROOT_DIR}/markee" "${PKG_DIR}/usr/bin/markee"
install -m 0644 "${ROOT_DIR}/docs/markee.1" "${PKG_DIR}/usr/share/man/man1/markee.1"
gzip -9fn "${PKG_DIR}/usr/share/man/man1/markee.1"
install -m 0644 "${ROOT_DIR}/README.md" "${ROOT_DIR}/LICENSE" "${ROOT_DIR}/docs/API.md" "${PKG_DIR}/usr/share/doc/markee/"
install -m 0644 "${ROOT_DIR}/completions/markee.bash" "${PKG_DIR}/usr/share/bash-completion/completions/markee"
install -m 0644 "${ROOT_DIR}/completions/_markee" "${PKG_DIR}/usr/share/zsh/vendor-completions/_markee"
install -m 0644 "${ROOT_DIR}/completions/markee.fish" "${PKG_DIR}/usr/share/fish/vendor_completions.d/markee.fish"
cp -R "${ROOT_DIR}/examples/." "${PKG_DIR}/usr/share/doc/markee/examples/"

cat > "${PKG_DIR}/usr/lib/systemd/user/markee.service" <<'EOF'
[Unit]
Description=markee LED matrix display simulator
After=graphical-session.target

[Service]
ExecStart=/usr/bin/markee --always-on-top --borderless --corner top-right --save-position
Restart=on-failure

[Install]
WantedBy=default.target
EOF

cat > "${PKG_DIR}/DEBIAN/control" <<EOF
Package: markee
Version: ${VERSION}
Section: utils
Priority: optional
Architecture: ${ARCH}
Depends: libc6, libraylib4 | libraylib5 | libraylib-dev
Maintainer: Steven Spencer <steve@example.com>
Description: Simulated 72x16 24-bit RGB LED matrix display with HTTP API
 A C11 + raylib desktop LED matrix widget and HTTP/1.1 API server supporting
 atomic shape/text/widget drawing, animated GIFs, gentle alert chimes,
 built-in Web Playground, and client CLI subcommands.
EOF

dpkg-deb --build --root-owner-group "${PKG_DIR}" "${OUT_DEB}"
echo "Built ${OUT_DEB}"
