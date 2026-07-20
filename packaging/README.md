# Linux packages

`make deb` and `make rpm` build native installers for libsquish and the squish
CLI, split the conventional distro way into three packages per format:

| Role | `.deb` | `.rpm` | Contents |
|------|--------|--------|----------|
| Runtime library | `libsquish1` | `libsquish1` | `libsquish.so.1`, `libsquish.so.1.0.0` |
| Development files | `libsquish-dev` | `libsquish-devel` | `squish.h`, `libsquish.a`, `libsquish.so`, `squish.pc` |
| Command-line tool | `squish` | `squish` | `/usr/bin/squish` |

`libsquish-dev`/`-devel` depends on `libsquish1` (the dev `.so` symlink resolves
to the runtime object). The `squish` CLI is statically linked, so it depends
only on libc — it does **not** pull in `libsquish1`.

All three packages are partitioned from a single `make install` tree, so their
file layout stays in sync with a plain `make install`.

```sh
make deb        # -> build/libsquish1_1.0.0_amd64.deb
                #    build/libsquish-dev_1.0.0_amd64.deb
                #    build/squish_1.0.0_amd64.deb
make rpm        # -> build/libsquish1-1.0.0-1.x86_64.rpm
                #    build/libsquish-devel-1.0.0-1.x86_64.rpm
                #    build/squish-1.0.0-1.x86_64.rpm
make packages   # both formats
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
make packages PKGDIR=/tmp/squish-pkg      # then find the packages in /tmp/squish-pkg
```

The build itself still runs from the checkout; only the package staging tree
moves.

## Files

- `deb/<pkg>/control.in` — Debian control template per package (`@VERSION@`,
  `@ARCH@`, `@MAINTAINER@` are substituted at build time).
- `deb/libsquish1/postinst`, `postrm` — run `ldconfig` on install/remove.
- `deb/copyright` — machine-readable copyright/licensing (shipped in each package).
- `deb/build-debs.sh` — partitions the staged tree into the three `.deb` trees.
- `squish.spec.in` — RPM spec template with the three subpackages
  (`@VERSION@`, `@RELEASE@`, `@SRCDIR@`).

The RPM installs libraries to `%{_libdir}` (`/usr/lib64` on 64-bit rpm distros);
the `.deb` uses `/usr/lib`. Both are on the default linker search path.
