#!/usr/bin/env bash
# Build a static macOS aria2c for one arch against prebuilt deps in build-release/arch.
# Usage: build-static-osx.sh <arch>   (arch = arm64 | x86_64)
set -euo pipefail
ARCH="${1:?arch required}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BR="$ROOT/build-release"
DEST="$BR/aria2.$ARCH"
export CC=cc CXX="c++ -stdlib=libc++"
COMMON="-mmacosx-version-min=11.0 -Os -flto -ffunction-sections -fdata-sections -arch $ARCH -I$BR/arch/include"
rm -rf "$DEST"; mkdir -p "$DEST"; cd "$DEST"
../../configure --prefix="$BR/out" --bindir="$DEST" --sysconfdir=/etc \
  --with-cppunit-prefix="$BR/arch" \
  --enable-static --disable-shared --enable-metalink --enable-bittorrent \
  --disable-nls --with-appletls --with-libgmp --with-sqlite3 --with-libz \
  --with-libexpat --with-libcares --with-libgcrypt --with-libssh2 \
  --without-libuv --without-gnutls --without-openssl --without-libnettle --without-libxml2 \
  CFLAGS="$COMMON" CXXFLAGS="$COMMON" \
  LDFLAGS="-Wl,-dead_strip -mmacosx-version-min=11.0 -Os -flto -ffunction-sections -fdata-sections -L$BR/arch/lib" \
  PKG_CONFIG_PATH="$BR/arch/lib/pkgconfig"
make -sj"$(sysctl -n hw.ncpu)"
otool -hv src/aria2c | grep -q PIE
strip src/aria2c
codesign --force --sign - src/aria2c
otool -L src/aria2c
echo "built: $DEST/src/aria2c"
