#!/bin/sh
#
# Build git for hosts that clone and fetch large repositories: zlib-ng
# instead of zlib, OpenSSL's SHA-1 for checksums (object names keep
# collision detection), and -O3
# with link-time optimization.
#
#   contrib/owner-build/build.sh <prefix> [<make argument>...]
#
# Remaining arguments go to make; with none, it runs "make install",
# e.g. "install test T=t5300-pack-object.sh".
#
# Needs a C compiler (GCC), make, cmake, curl, tar, cargo, and the
# OpenSSL, curl and expat development headers.

set -eu

prefix=${1:?usage: $0 <prefix> [<make argument>...]}
shift
test $# -gt 0 || set -- install

zlib_ng_version=2.3.1
zlib_ng_sha256=94cfa0a53a8265c813c8369d3963bf09ac6a357824ad1f93aee13486176e152e

cd "$(dirname "$0")/../.."
jobs=$(getconf _NPROCESSORS_ONLN)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

curl -fsSL -o "$work/zlib-ng.tar.gz" \
	"https://github.com/zlib-ng/zlib-ng/archive/refs/tags/$zlib_ng_version.tar.gz"
echo "$zlib_ng_sha256  $work/zlib-ng.tar.gz" | sha256sum -c - >/dev/null
tar -xzf "$work/zlib-ng.tar.gz" -C "$work"
cmake -S "$work/zlib-ng-$zlib_ng_version" -B "$work/zlib-ng-build" \
	-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$work/zlib-ng" \
	-DCMAKE_INSTALL_LIBDIR=lib \
	-DZLIB_COMPAT=OFF -DBUILD_SHARED_LIBS=OFF -DZLIB_ENABLE_TESTS=OFF \
	-DCMAKE_POSITION_INDEPENDENT_CODE=ON >/dev/null
cmake --build "$work/zlib-ng-build" -j "$jobs" >/dev/null
cmake --install "$work/zlib-ng-build" >/dev/null

make -j "$jobs" prefix="$prefix" \
	ZLIB_NG=YesPlease ZLIB_NG_PATH="$work/zlib-ng" \
	OPENSSL_SHA1_UNSAFE=YesPlease \
	CFLAGS="-O3 -g -flto=auto" LDFLAGS="-flto=auto" AR=gcc-ar \
	NO_GETTEXT=YesPlease NO_TCLTK=YesPlease RUNTIME_PREFIX=YesPlease \
	"$@"
