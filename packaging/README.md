# Linux packages

`make deb` and `make rpm` build native installers for the squish CLI, the
shared/static libsquish libraries, the public header, and the pkg-config file.
Both packages install under `/usr` and are produced from the same `make install`
tree, so the file layout stays in sync with a plain `make install`.

```sh
make deb        # -> build/squish_1.0.0_amd64.deb
make rpm        # -> build/squish-1.0.0-1.x86_64.rpm
make packages   # both
```

Output lands in `build/` (git-ignored). The version tracks `VERSION` in the
top-level `Makefile`; override packaging details on the command line:

```sh
make deb  DEB_ARCH=arm64
make rpm  RPM_ARCH=aarch64 PKG_RELEASE=2
make packages PKG_MAINTAINER="Jane Doe <jane@example.com>"
```

## Build requirements

| Format | Tool        | Debian/Ubuntu           | Fedora/RHEL        |
|--------|-------------|-------------------------|--------------------|
| `.deb` | `dpkg-deb`  | `apt install dpkg`      | `dnf install dpkg` |
| `.rpm` | `rpmbuild`  | `apt install rpm`       | `dnf install rpm-build` |

Neither target needs root; `.deb` metadata is stamped root-owned via
`dpkg-deb --root-owner-group`, and `.rpm` builds into `build/rpm` as the current
user.

### WSL / drvfs mounts

`dpkg-deb` and `rpmbuild` refuse the `0777` permissions that a Windows drive
mount (`/mnt/c`, `/mnt/d`, …) forces on every file, so staging must happen on a
real Linux filesystem. Point `PKGDIR` at one:

```sh
make packages PKGDIR=/tmp/squish-pkg      # then find the .deb/.rpm in /tmp/squish-pkg
```

The build itself still runs from the checkout; only the package staging tree
moves.

## Files

- `deb/control.in` — Debian control template (`@VERSION@`, `@ARCH@`,
  `@MAINTAINER@` are substituted by the Makefile).
- `deb/postinst`, `deb/postrm` — run `ldconfig` when libsquish is added/removed.
- `deb/copyright` — machine-readable copyright/licensing.
- `squish.spec.in` — RPM spec template (`@VERSION@`, `@RELEASE@`, `@SRCDIR@`).

The RPM installs libraries to `%{_libdir}` (`/usr/lib64` on 64-bit rpm distros);
the `.deb` uses `/usr/lib`. Both are on the default linker search path.
