#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PKG_DIR="${SCRIPT_DIR}/build/pkg_root"
VERSION="1.2.2-1"
ARCH="amd64"
DEB_NAME="squeez_amd64.deb"

echo "=== Building Squeez from source ==="
cd "${SCRIPT_DIR}"
make -j"$(nproc)" linux

echo "=== Running test suite before packaging ==="
make test

echo "=== Packaging DEB ==="
rm -rf "${PKG_DIR}"
mkdir -p "${PKG_DIR}/usr/bin"
mkdir -p "${PKG_DIR}/usr/share/doc/squeez"
mkdir -p "${PKG_DIR}/DEBIAN"

# Install binary
cp -f "${SCRIPT_DIR}/build/linux/sqx" "${PKG_DIR}/usr/bin/sqx"
strip -s "${PKG_DIR}/usr/bin/sqx" || true
chmod 755 "${PKG_DIR}/usr/bin/sqx"

# Documentation
cp "${SCRIPT_DIR}/README.md" "${PKG_DIR}/usr/share/doc/squeez/"
cp "${SCRIPT_DIR}/FORMAT.md" "${PKG_DIR}/usr/share/doc/squeez/"
cp "${SCRIPT_DIR}/LICENSE" "${PKG_DIR}/usr/share/doc/squeez/"

# DEBIAN/control
cat << CTRL_EOF > "${PKG_DIR}/DEBIAN/control"
Package: squeez
Version: ${VERSION}
Section: utils
Priority: optional
Architecture: amd64
Maintainer: seb3773 <https://github.com/seb3773/Squeez-c>
Depends: libc6 (>= 2.15)
Provides: sqx
Description: Pure C reimplementation of historical Squeez SQX archiver
 Complete, standalone freestanding C tool combining both compressor and
 decompressor into a single binary (sqx), supporting the SQX archive format
 (versions 1.1 and 2.0).
 Features:
  - Methods 0 (Stored), 1-4 (LZH sliding-window 32KB to 32MB dictionary)
  - Method 5 (Lossless 8-bit and 16-bit PCM Audio 1.1 & 2.0 codecs)
  - Method 6 (PPMd var. I text compression)
  - Method 7 (Ultra LZ77 + carryless range coder + adaptive Fenwick models)
  - Solid multi-file archives with persistent dictionary and probability models
  - Self-Extracting (SFX) archive handling for Win32 PE executables
  - Security hardening: path traversal and Zip-Slip protection
  - Historical digital preservation & software interoperability (EU 2009/24/EC)
CTRL_EOF

# Build DEB
mkdir -p "${SCRIPT_DIR}/dist"
dpkg-deb --build --root-owner-group "${PKG_DIR}" "${SCRIPT_DIR}/dist/${DEB_NAME}"
echo "Package created: ${SCRIPT_DIR}/dist/${DEB_NAME}"

# Clean temp packaging dir
rm -rf "${PKG_DIR}"

echo ""
echo "=== Package Information ==="
dpkg-deb -I "${SCRIPT_DIR}/dist/${DEB_NAME}"
echo ""
echo "=== Package Contents ==="
dpkg-deb -c "${SCRIPT_DIR}/dist/${DEB_NAME}"

echo ""
echo "=== Squeez packaging finished successfully ==="

