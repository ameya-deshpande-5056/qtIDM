# The build script rewrites Version from CMakeLists.txt; keep it in step by hand
# only when building this spec directly with rpmbuild.
Name:           qtidm
Version:        0.3.4
Release:        1%{?dist}
Summary:        Fast, native Internet Download Manager for Linux

License:        MIT
URL:            https://github.com/ameya-deshpande-5056/qtIDM
Source0:        %{name}-%{version}.tar.gz

# Built natively on either architecture, or cross-built with
# build-rpm.sh --arch/--toolchain, which also sets --target.
ExclusiveArch:  x86_64 aarch64

BuildRequires:  cmake
BuildRequires:  ninja-build
BuildRequires:  gcc-c++
BuildRequires:  qt6-qtbase-devel
BuildRequires:  libcurl-devel
BuildRequires:  sqlite-devel
BuildRequires:  openssl-devel

Requires:       qt6-qtbase
Requires:       libcurl
Requires:       sqlite-libs
Requires:       openssl-libs
Recommends:     ffmpeg-free
Recommends:     p7zip
Recommends:     libsecret
Suggests:       nodejs

%description
qtIDM is a Linux-native download manager built with Qt 6 and libcurl. It
supports segmented HTTP/HTTPS/FTP transfers, resume after a crash, queues and
scheduling, HLS/DASH and social-media downloads, archive extraction, Chrome and
Firefox integration, and a D-Bus automation interface driven by qtidm-cli.

%prep
%setup -q -n %{name}-%{version}

%build
%cmake -G Ninja \
%if %{defined qtidm_toolchain}
    -DCMAKE_TOOLCHAIN_FILE=%{qtidm_toolchain} \
%endif
    -DCMAKE_BUILD_TYPE=Release \
    -DQTIDM_BUILD_TESTS=OFF \
    -DQTIDM_INSTALL_NATIVE_MESSAGING_HOSTS=ON
%cmake_build

%install
%cmake_install

%files
%license LICENSE
%doc README.md CHANGELOG.md
/usr/bin/qtIDM
/usr/bin/qtIDM-native-host
/usr/bin/qtidm-cli
%{_datadir}/applications/io.qtidm.Qtidm.desktop
%{_datadir}/icons/hicolor/scalable/apps/io.qtidm.Qtidm.svg
%{_datadir}/icons/hicolor/*/apps/io.qtidm.Qtidm.png
%{_datadir}/metainfo/io.qtidm.Qtidm.appdata.xml
%{_datadir}/mime/packages/io.qtidm.Qtidm.xml
%{_datadir}/qtidm/browser
/usr/lib/mozilla/native-messaging-hosts/io.qtidm.native.json
%config(noreplace) /etc/opt/chrome/native-messaging-hosts/io.qtidm.native.json
%config(noreplace) /etc/chromium/native-messaging-hosts/io.qtidm.native.json

%changelog
* Wed Nov 18 2026 qtIDM Maintainers <maintainers@example.invalid> - 0.3.4-1
- Native multi-threaded HLS chunk downloader, DNS-over-HTTPS, 4 KiB-aligned
  segment planner, site-adapter expired-URL recovery, per-domain rules,
  magic-byte semantic naming, browser-like TLS/header camouflage, and deep
  browser sniffing with ServiceWorker stream pulling.

* Sun Nov 15 2026 qtIDM Maintainers <maintainers@example.invalid> - 0.3.3-1
- Multi-architecture packaging across all formats and Deno runtime bundling for YouTube.

* Sun Nov 15 2026 qtIDM Maintainers <maintainers@example.invalid> - 0.3.2-1
- Build for arm64 (aarch64) as well as amd64 (x86_64).

* Sun Nov 15 2026 qtIDM Maintainers <maintainers@example.invalid> - 0.3.1-1
- Automation channel and qtidm-cli, portable data directory, startup resume of
  interrupted transfers, and RPM packaging.

* Sun Nov 15 2026 qtIDM Maintainers <maintainers@example.invalid> - 0.3.0-1
- Initial package.
