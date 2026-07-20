# libsquish — context-mixing compressor
#
# Targets:
#   make            build libsquish.so, libsquish.a, and the squish CLI
#   make test       build + run the test suite
#   make dll        cross-compile squish.dll + squish.exe (needs mingw-w64)
#   make windows-dll   build squish.dll + squish.exe with MSVC (needs cl.exe
#                       on PATH, e.g. a VS "Developer Command Prompt")
#   make install    install to $(PREFIX) (default /usr/local)
#   make deb        build .deb packages (needs dpkg-deb)      -> build/
#   make rpm        build .rpm packages (needs rpmbuild)      -> build/
#   make packages   build both (libsquish1, libsquish-dev/-devel, squish)
#   make clean

CC       ?= gcc
OPT      ?= -O3 -funroll-loops
WARN      = -Wall -Wextra
THREADS  ?= -pthread
CFLAGS   ?= $(OPT)
PREFIX   ?= /usr/local
LIBDIR   ?= $(PREFIX)/lib
DESTDIR  ?=

VERSION   = 1.0.0
SOMAJOR   = 1
SONAME    = libsquish.so.$(SOMAJOR)

# ---- Linux package metadata (.deb / .rpm; see packaging/) --------------------
PKG_MAINTAINER ?= Paige Julianne Sullivan <wiley14@gmail.com>
PKG_RELEASE    ?= 1
DEB_ARCH       ?= amd64
RPM_ARCH       ?= x86_64
# PKGDIR may be relative or absolute; PKGROOT is its absolute form (rpmbuild's
# _topdir and an install DESTDIR both want an absolute path). Override PKGDIR to
# stage on a real filesystem when the checkout lives on one that forces 0777
# (e.g. a WSL /mnt drvfs mount, which dpkg-deb and rpm reject).
PKGDIR          = build
PKGROOT         = $(abspath $(PKGDIR))
# The library and tool split into three packages per format: a runtime shared
# library (libsquish1), its development files (libsquish-dev / -devel), and the
# CLI (squish).
DEB_LIB         = $(PKGDIR)/libsquish1_$(VERSION)_$(DEB_ARCH).deb
DEB_DEV         = $(PKGDIR)/libsquish-dev_$(VERSION)_$(DEB_ARCH).deb
DEB_CLI         = $(PKGDIR)/squish_$(VERSION)_$(DEB_ARCH).deb
RPM_LIB         = $(PKGDIR)/libsquish1-$(VERSION)-$(PKG_RELEASE).$(RPM_ARCH).rpm
RPM_DEV         = $(PKGDIR)/libsquish-devel-$(VERSION)-$(PKG_RELEASE).$(RPM_ARCH).rpm
RPM_CLI         = $(PKGDIR)/squish-$(VERSION)-$(PKG_RELEASE).$(RPM_ARCH).rpm

MINGW    ?= x86_64-w64-mingw32-gcc
MINGW_AR ?= x86_64-w64-mingw32-ar

CL       ?= cl.exe
CLFLAGS  ?= /nologo /O2 /W3

# Azure Trusted Signing (opt-in; used by the windows-dll target). Set all three
# — via the environment or make args — to sign the built binaries with a cloud
# certificate profile (no local .pfx). Auth uses the Azure credential chain
# (az login / OIDC / service principal). Signer: the `sign` .NET global tool
# (https://github.com/dotnet/sign), auto-installed if missing (needs .NET SDK).
TRUSTED_SIGNING_ENDPOINT ?=
TRUSTED_SIGNING_ACCOUNT  ?=
TRUSTED_SIGNING_PROFILE  ?=

all: libsquish.so squish

# ---- shared library ---------------------------------------------------------
libsquish.so.$(VERSION): squish.c squish.h
	$(CC) $(CFLAGS) $(THREADS) $(WARN) -fPIC -fvisibility=hidden -DSQUISH_BUILD \
	    -shared -Wl,-soname,$(SONAME) -o $@ squish.c -lm

libsquish.so: libsquish.so.$(VERSION)
	ln -sf libsquish.so.$(VERSION) $(SONAME)
	ln -sf $(SONAME) libsquish.so

# ---- static library ---------------------------------------------------------
squish.o: squish.c squish.h
	$(CC) $(CFLAGS) $(THREADS) $(WARN) -c -o $@ squish.c

libsquish.a: squish.o
	ar rcs $@ $^

# ---- CLI (statically linked against the lib) ---------------------------------
squish: squish_cli.c libsquish.a
	$(CC) $(CFLAGS) $(THREADS) $(WARN) -o $@ squish_cli.c libsquish.a -lm

# ---- Windows DLL + CLI (cross-compile; or use cl.exe /DSQUISH_BUILD_DLL) ------
dll: squish.dll squish.exe

squish.dll: squish.c squish.h
	$(MINGW) $(CFLAGS) $(WARN) -DSQUISH_BUILD_DLL -shared \
	    -o $@ squish.c -Wl,--out-implib,libsquish.dll.a

squish-win.o: squish.c squish.h
	$(MINGW) $(CFLAGS) $(WARN) -c -o $@ squish.c

libsquish-win.a: squish-win.o
	$(MINGW_AR) rcs $@ $^

squish.exe: squish_cli.c libsquish-win.a
	$(MINGW) $(CFLAGS) $(WARN) -o $@ squish_cli.c libsquish-win.a -lm

# ---- Windows DLL + CLI via MSVC (native build; run under a Developer Command
#      Prompt or after vcvarsall.bat so cl.exe is on PATH) ----------------------
# Not a squish.dll/squish.exe file rule: it would collide with the mingw
# rules above. Producing the same output filenames from either toolchain is
# intentional — pick whichever is available on the host.
# squish.exe links the library statically (compiling squish.c into it, like the
# mingw rule above) so it stands alone and runs without squish.dll present.
# When the TRUSTED_SIGNING_* variables are set the binaries are then signed with
# Azure Trusted Signing; otherwise they are left unsigned (opt-in).
windows-dll:
	$(CL) $(CLFLAGS) /LD /DSQUISH_BUILD_DLL /Fe:squish.dll squish.c
	$(CL) $(CLFLAGS) /Fe:squish.exe squish_cli.c squish.c
	@if [ -z "$(TRUSTED_SIGNING_ENDPOINT)" ] || [ -z "$(TRUSTED_SIGNING_ACCOUNT)" ] || [ -z "$(TRUSTED_SIGNING_PROFILE)" ]; then \
	    echo "Note: Azure Trusted Signing not configured (TRUSTED_SIGNING_* unset); binaries are unsigned."; \
	else \
	    if ! command -v sign >/dev/null 2>&1; then \
	        command -v dotnet >/dev/null 2>&1 || { echo "error: Trusted Signing configured but neither 'sign' nor 'dotnet' is on PATH; install the .NET SDK or run 'dotnet tool install --global sign'." >&2; exit 1; }; \
	        echo "Installing the 'sign' .NET global tool..."; \
	        dotnet tool install --global sign; \
	        PATH="$$PATH:$$HOME/.dotnet/tools"; \
	    fi; \
	    echo "Signing squish.dll and squish.exe with Azure Trusted Signing..."; \
	    sign code trusted-signing squish.dll squish.exe \
	        --trusted-signing-endpoint "$(TRUSTED_SIGNING_ENDPOINT)" \
	        --trusted-signing-account "$(TRUSTED_SIGNING_ACCOUNT)" \
	        --trusted-signing-certificate-profile "$(TRUSTED_SIGNING_PROFILE)"; \
	fi

# ---- tests / examples ---------------------------------------------------------
test: tests/test_squish
	./tests/test_squish

tests/test_squish: tests/test_squish.c libsquish.a
	$(CC) $(CFLAGS) $(THREADS) $(WARN) -I. -o $@ tests/test_squish.c libsquish.a -lm

example: examples/example
examples/example: examples/example.c libsquish.so
	$(CC) $(CFLAGS) $(THREADS) $(WARN) -I. -o $@ examples/example.c -L. -lsquish -lm \
	    -Wl,-rpath,'$$ORIGIN/..'

# ---- install ------------------------------------------------------------------
install: libsquish.so libsquish.a squish
	install -d $(DESTDIR)$(LIBDIR) $(DESTDIR)$(PREFIX)/include \
	           $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(LIBDIR)/pkgconfig
	install -m 755 libsquish.so.$(VERSION) $(DESTDIR)$(LIBDIR)/
	ln -sf libsquish.so.$(VERSION) $(DESTDIR)$(LIBDIR)/$(SONAME)
	ln -sf $(SONAME) $(DESTDIR)$(LIBDIR)/libsquish.so
	install -m 644 libsquish.a $(DESTDIR)$(LIBDIR)/
	install -m 644 squish.h $(DESTDIR)$(PREFIX)/include/
	install -m 755 squish $(DESTDIR)$(PREFIX)/bin/
	printf 'prefix=%s\nlibdir=%s\nincludedir=$${prefix}/include\n\nName: squish\nDescription: context-mixing compressor\nVersion: %s\nLibs: -L$${libdir} -lsquish -lm -pthread\nCflags: -I$${includedir}\n' \
	    "$(PREFIX)" "$(LIBDIR)" "$(VERSION)" > $(DESTDIR)$(LIBDIR)/pkgconfig/squish.pc

# ---- Linux packages (.deb / .rpm) --------------------------------------------
# Each format yields three packages, partitioned from one `make install' tree so
# the file layout stays in sync with a plain install: libsquish1 (runtime .so),
# libsquish-dev/-devel (header, static lib, .so symlink, .pc), and squish (CLI).
# Output lands in build/. See packaging/README.md.
packages: deb rpm

# Full install tree the packages are carved from (built once, shared by deb/rpm).
stage:
	rm -rf $(PKGROOT)/stage
	$(MAKE) install DESTDIR=$(PKGROOT)/stage PREFIX=/usr LIBDIR=/usr/lib
	strip --strip-unneeded $(PKGROOT)/stage/usr/lib/libsquish.so.$(VERSION)
	strip $(PKGROOT)/stage/usr/bin/squish

deb: all stage
	@command -v dpkg-deb >/dev/null 2>&1 || { echo "error: dpkg-deb not found; install it (apt-get install dpkg)." >&2; exit 1; }
	rm -rf $(PKGROOT)/deb
	sh packaging/deb/build-debs.sh \
	    $(PKGROOT)/stage $(PKGROOT) $(PKGROOT)/deb \
	    $(VERSION) $(SOMAJOR) $(DEB_ARCH) "$(PKG_MAINTAINER)"
	@echo "built $(DEB_LIB) $(DEB_DEV) $(DEB_CLI)"

rpm: all
	@command -v rpmbuild >/dev/null 2>&1 || { echo "error: rpmbuild not found; install it (apt-get install rpm / dnf install rpm-build)." >&2; exit 1; }
	rm -rf $(PKGROOT)/rpm
	install -d $(PKGROOT)/rpm
	sed -e 's|@VERSION@|$(VERSION)|g' -e 's|@RELEASE@|$(PKG_RELEASE)|g' \
	    -e 's|@SRCDIR@|$(CURDIR)|g' \
	    packaging/squish.spec.in > $(PKGROOT)/rpm/squish.spec
	rpmbuild -bb --target $(RPM_ARCH) \
	    --define "_topdir $(PKGROOT)/rpm" \
	    --define "_rpmdir $(PKGROOT)/rpm/out" \
	    $(PKGROOT)/rpm/squish.spec
	mv $(PKGROOT)/rpm/out/*/*.rpm $(PKGROOT)/
	@echo "built $(RPM_LIB) $(RPM_DEV) $(RPM_CLI)"

clean:
	rm -rf $(PKGDIR)
	rm -f libsquish.so libsquish.so.* libsquish.a squish.o squish \
	      squish.dll libsquish.dll.a squish-win.o libsquish-win.a squish.exe \
	      squish.obj squish_cli.obj squish.lib squish.exp \
	      tests/test_squish examples/example

.PHONY: all dll windows-dll test example install packages stage deb rpm clean
