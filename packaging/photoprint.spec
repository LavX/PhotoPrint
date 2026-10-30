Name:           photoprint
Version:        1.0.1
Release:        1%{?dist}
Summary:        Simple local photo printing
License:        MIT
URL:            https://github.com/LavX/PhotoPrint
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
Requires:       lcms2
Requires:       cups-libs
Requires:       zlib

%description
A local desktop application for arranging photos on paper, cropping them and
printing them. It uses the existing CUPS queues and installs no driver.

%description -l hu
Helyi asztali alkalmazás fotók papírra rendezéséhez, kivágásához és
nyomtatásához. A meglévő CUPS-sorokat használja, nem telepít drivert.

%description -l de
Fotos anordnen, zuschneiden und auf Papier drucken.

%description -l es
Ordena, recorta e imprime fotos en papel.

%description -l fr
Disposez, recadrez et imprimez vos photos sur papier.

%description -l hi
फ़ोटो को कागज़ पर सजाएँ, काटें और प्रिंट करें।

%description -l it
Disponi, ritaglia e stampa le foto su carta.

%description -l ja
写真を並べて切り抜き、紙に印刷します。

%description -l pt
Organize, recorte e imprima fotos em papel.

%description -l ru
Раскладывайте, обрезайте и печатайте фотографии на бумаге.

%description -l zh_CN
把照片排版、裁剪并打印到纸上。

%prep
rm -rf %{_builddir}/photoprint-build
mkdir -p %{_builddir}/photoprint-build
for item in CMakeLists.txt CMakePresets.json src tests resources; do
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
* Fri Oct 30 2026 Laszlo Toth <lavx@lavx.hu> - 1.0.1-1
- Correctness, security and durability fixes from the code review
- Print resolution follows the CUPS queue up to 1200 ppi
- Borderless placement centred and applied to exported PDFs

* Fri Oct 30 2026 Laszlo Toth <lavx@lavx.hu> - 1.0.0-1
- First public build
