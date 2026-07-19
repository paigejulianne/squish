# libsquish — context-mixing compressor
#
# Targets:
#   make            build libsquish.so, libsquish.a, and the squish CLI
#   make test       build + run the test suite
#   make dll        cross-compile squish.dll + squish.exe (needs mingw-w64)
#   make windows-dll   build squish.dll + squish.exe with MSVC (needs cl.exe
#                       on PATH, e.g. a VS "Developer Command Prompt")
#   make install    install to $(PREFIX) (default /usr/local)
#   make deb        build a .deb installer (needs dpkg-deb)   -> build/
#   make rpm        build a .rpm installer (needs rpmbuild)   -> build/
#   make packages   build both .deb and .rpm
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
DEB_PKG         = $(PKGDIR)/squish_$(VERSION)_$(DEB_ARCH).deb
RPM_PKG         = $(PKGDIR)/squish-$(VERSION)-$(PKG_RELEASE).$(RPM_ARCH).rpm

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
# Both are built from the same `make install' tree, so their file layout stays
# in sync with a plain install. Output lands in build/. See packaging/README.md.
packages: deb rpm

deb: all
	@command -v dpkg-deb >/dev/null 2>&1 || { echo "error: dpkg-deb not found; install it (apt-get install dpkg)." >&2; exit 1; }
	rm -rf $(PKGROOT)/deb
	$(MAKE) install DESTDIR=$(PKGROOT)/deb PREFIX=/usr LIBDIR=/usr/lib
	install -d $(PKGROOT)/deb/DEBIAN
	sed -e 's|@VERSION@|$(VERSION)|g' -e 's|@ARCH@|$(DEB_ARCH)|g' \
	    -e 's|@MAINTAINER@|$(PKG_MAINTAINER)|g' \
	    packaging/deb/control.in > $(PKGROOT)/deb/DEBIAN/control
	install -m 755 packaging/deb/postinst $(PKGROOT)/deb/DEBIAN/postinst
	install -m 755 packaging/deb/postrm   $(PKGROOT)/deb/DEBIAN/postrm
	install -d $(PKGROOT)/deb/usr/share/doc/squish
	install -m 644 packaging/deb/copyright $(PKGROOT)/deb/usr/share/doc/squish/copyright
	strip --strip-unneeded $(PKGROOT)/deb/usr/lib/libsquish.so.$(VERSION)
	strip $(PKGROOT)/deb/usr/bin/squish
	dpkg-deb --root-owner-group --build $(PKGROOT)/deb $(DEB_PKG)
	@echo "built $(DEB_PKG)"

rpm: all
	@command -v rpmbuild >/dev/null 2>&1 || { echo "error: rpmbuild not found; install it (apt-get install rpm / dnf install rpm-build)." >&2; exit 1; }
	rm -rf $(PKGROOT)/rpm
	install -d $(PKGROOT)/rpm
	sed -e 's|@VERSION@|$(VERSION)|g' -e 's|@RELEASE@|$(PKG_RELEASE)|g' \
	    -e 's|@SRCDIR@|$(CURDIR)|g' \
	    packaging/squish.spec.in > $(PKGROOT)/rpm/squish.spec
	rpmbuild -bb --target $(RPM_ARCH) \
	    --define "_topdir $(PKGROOT)/rpm" \
	    --define "_rpmdir $(PKGROOT)" \
	    --define "_rpmfilename squish-$(VERSION)-$(PKG_RELEASE).$(RPM_ARCH).rpm" \
	    $(PKGROOT)/rpm/squish.spec
	@echo "built $(RPM_PKG)"

clean:
	rm -rf $(PKGDIR)
	rm -f libsquish.so libsquish.so.* libsquish.a squish.o squish \
	      squish.dll libsquish.dll.a squish-win.o libsquish-win.a squish.exe \
	      squish.obj squish_cli.obj squish.lib squish.exp \
	      tests/test_squish examples/example

.PHONY: all dll windows-dll test example install packages deb rpm clean
