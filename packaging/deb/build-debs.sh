#!/bin/sh
# Build the three squish .deb packages by partitioning a staged install tree.
#
# Usage: build-debs.sh STAGE OUT WORK VERSION SOMAJOR ARCH "MAINTAINER"
#   STAGE   full `make install' tree (PREFIX=/usr, LIBDIR=/usr/lib)
#   OUT     directory the .deb files are written to
#   WORK    scratch directory for the per-package trees
#   VERSION package version (e.g. 1.0.0)
#   SOMAJOR shared-object major version (e.g. 1)
#   ARCH    Debian architecture (e.g. amd64)
#   MAINTAINER  "Name <email>"
set -eu

STAGE=$1; OUT=$2; WORK=$3; VERSION=$4; SOMAJOR=$5; ARCH=$6; MAINTAINER=$7
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

# Render a control.in template, substituting the packaging variables.
render_control() {
    sed -e "s|@VERSION@|$VERSION|g" \
        -e "s|@ARCH@|$ARCH|g" \
        -e "s|@MAINTAINER@|$MAINTAINER|g" \
        "$1"
}

# Assemble one package: <name> then its already-populated file tree under WORK.
# Adds DEBIAN metadata, the shared copyright, optional maintainer scripts, and
# builds the .deb.
assemble() {
    name=$1
    root=$WORK/$name
    install -d "$root/DEBIAN"
    render_control "$HERE/$name/control.in" > "$root/DEBIAN/control"
    for s in postinst postrm preinst prerm; do
        if [ -f "$HERE/$name/$s" ]; then
            install -m 755 "$HERE/$name/$s" "$root/DEBIAN/$s"
        fi
    done
    install -d "$root/usr/share/doc/$name"
    install -m 644 "$HERE/copyright" "$root/usr/share/doc/$name/copyright"
    dpkg-deb --root-owner-group --build "$root" "$OUT/${name}_${VERSION}_${ARCH}.deb"
}

# --- libsquish1: runtime shared object + SONAME symlink ----------------------
install -d "$WORK/libsquish1/usr/lib"
cp -a "$STAGE/usr/lib/libsquish.so.$VERSION"  "$WORK/libsquish1/usr/lib/"
cp -a "$STAGE/usr/lib/libsquish.so.$SOMAJOR"  "$WORK/libsquish1/usr/lib/"
assemble libsquish1

# --- libsquish-dev: header, static lib, dev symlink, pkg-config --------------
install -d "$WORK/libsquish-dev/usr/include" "$WORK/libsquish-dev/usr/lib/pkgconfig"
cp -a "$STAGE/usr/include/squish.h"           "$WORK/libsquish-dev/usr/include/"
cp -a "$STAGE/usr/lib/libsquish.a"            "$WORK/libsquish-dev/usr/lib/"
cp -a "$STAGE/usr/lib/libsquish.so"           "$WORK/libsquish-dev/usr/lib/"
cp -a "$STAGE/usr/lib/pkgconfig/squish.pc"    "$WORK/libsquish-dev/usr/lib/pkgconfig/"
assemble libsquish-dev

# --- squish: the command-line tool -------------------------------------------
install -d "$WORK/squish/usr/bin"
cp -a "$STAGE/usr/bin/squish"                 "$WORK/squish/usr/bin/"
assemble squish
