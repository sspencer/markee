#!/usr/bin/env bash
set -euo pipefail

# Create a versioned release tarball in build/markee-<version>-<os>-<arch>.tar.gz
#
# Usage:
#   ./packaging/build-tarball.sh [--version 1.0.0]

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

OS_NAME="$(uname -s | tr '[:upper:]' '[:lower:]')"
ARCH_NAME="$(uname -m)"
STAGE_NAME="markee-${VERSION}-${OS_NAME}-${ARCH_NAME}"
STAGE_DIR="${ROOT_DIR}/build/${STAGE_NAME}"
OUT_TGZ="${ROOT_DIR}/build/${STAGE_NAME}.tar.gz"

if [[ ! -x "${ROOT_DIR}/markee" ]]; then
  make -C "${ROOT_DIR}" release
fi

rm -rf "${STAGE_DIR}"
mkdir -p "${STAGE_DIR}/docs" "${STAGE_DIR}/examples"
install -m 0755 "${ROOT_DIR}/markee" "${STAGE_DIR}/markee"
install -m 0644 "${ROOT_DIR}/README.md" "${ROOT_DIR}/LICENSE" "${STAGE_DIR}/"
install -m 0644 "${ROOT_DIR}/docs/API.md" "${STAGE_DIR}/docs/"
cp -R "${ROOT_DIR}/examples/." "${STAGE_DIR}/examples/"

tar -C "${ROOT_DIR}/build" -czf "${OUT_TGZ}" "${STAGE_NAME}"
rm -rf "${STAGE_DIR}"
echo "Built ${OUT_TGZ}"
