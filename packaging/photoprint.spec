Name:           photoprint
Version:        0.1.0
Release:        1%{?dist}
Summary:        Egyszerű helyi fotónyomtatás
License:        Proprietary
URL:            https://example.invalid/photoprint
Source0:        %{photoprint_source}
BuildRequires:  cmake
BuildRequires:  ninja-build
BuildRequires:  gcc-c++
BuildRequires:  qt6-qtbase-devel
BuildRequires:  qt6-linguist
BuildRequires:  lcms2-devel
BuildRequires:  cups-devel
BuildRequires:  zlib-devel
BuildRequires:  ImageMagick-devel
Requires:       ImageMagick-libs
Requires:       qt6-qtbase
Requires:       lcms2-libs
Requires:       cups-libs
Requires:       zlib

%description
Helyi asztali alkalmazás fotók papírra rendezéséhez, kivágásához és
nyomtatásához. A meglévő CUPS-sorokat használja, nem telepít drivert.

%prep
rm -rf %{_builddir}/photoprint-build
mkdir -p %{_builddir}/photoprint-build
for item in CMakeLists.txt CMakePresets.json src tests resources packaging docs; do
  cp -a %{photoprint_source}/$item %{_builddir}/photoprint-build/
done

%build
cd %{_builddir}/photoprint-build
%cmake -G Ninja -DCMAKE_BUILD_TYPE=Release
%cmake_build

%install
cd %{_builddir}/photoprint-build
install -D -m 0755 %{_vpath_builddir}/photoprint %{buildroot}%{_bindir}/photoprint
install -D -m 0644 resources/photoprint.desktop %{buildroot}%{_datadir}/applications/photoprint.desktop
install -D -m 0644 resources/photoprint.svg %{buildroot}%{_datadir}/icons/hicolor/scalable/apps/photoprint.svg

%files
%{_bindir}/photoprint
%{_datadir}/applications/photoprint.desktop
%{_datadir}/icons/hicolor/scalable/apps/photoprint.svg

%changelog
* Thu Oct 29 2026 Laszlo Toth <lavx@lavx.hu> - 0.1.0-1
- First local build
