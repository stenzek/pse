#!/bin/bash

set -e

export MACOSX_DEPLOYMENT_TARGET=10.14
INSTALLDIR="$HOME/deps"
NPROCS="$(getconf _NPROCESSORS_ONLN)"
SDL=SDL2-2.0.22
QT=6.3.1
MOLTENVK=1.1.10
CURL=7.84.0

mkdir deps-build
cd deps-build

export PKG_CONFIG_PATH="$INSTALLDIR/lib/pkgconfig:$PKG_CONFIG_PATH"
export LDFLAGS="-L$INSTALLDIR/lib -dead_strip $LDFLAGS"
export CFLAGS="-I$INSTALLDIR/include -Os $CFLAGS"
export CXXFLAGS="-I$INSTALLDIR/include -Os $CXXFLAGS"

cat > SHASUMS <<EOF
fe7cbf3127882e3fc7259a75a0cb585620272c51745d3852ab9dd87960697f2e  $SDL.tar.gz
fac11c2501195c9ce042103685c7778e35484562e6c084963a22072dd0a602e0  v$MOLTENVK.tar.gz
3c6893d38d054d4e378267166858698899e9d87258e8ff1419d020c395384535  curl-$CURL.tar.gz
0a64421d9c2469c2c48490a032ab91d547017c9cc171f3f8070bc31888f24e03  qtbase-everywhere-src-$QT.tar.xz
7b19f418e6f7b8e23344082dd04440aacf5da23c5a73980ba22ae4eba4f87df7  qtsvg-everywhere-src-$QT.tar.xz
c412750f2aa3beb93fce5f30517c607f55daaeb7d0407af206a8adf917e126c1  qttools-everywhere-src-$QT.tar.xz
d7bdd55e2908ded901dcc262157100af2a490bf04d31e32995f6d91d78dfdb97  qttranslations-everywhere-src-$QT.tar.xz
EOF

curl -L \
	-O "https://libsdl.org/release/$SDL.tar.gz" \
        -O "https://github.com/KhronosGroup/MoltenVK/archive/refs/tags/v$MOLTENVK.tar.gz" \
        -O "https://curl.se/download/curl-$CURL.tar.gz" \
	-O "https://download.qt.io/official_releases/qt/${QT%.*}/$QT/submodules/qtbase-everywhere-src-$QT.tar.xz" \
	-O "https://download.qt.io/official_releases/qt/${QT%.*}/$QT/submodules/qtsvg-everywhere-src-$QT.tar.xz" \
	-O "https://download.qt.io/official_releases/qt/${QT%.*}/$QT/submodules/qttools-everywhere-src-$QT.tar.xz" \
	-O "https://download.qt.io/official_releases/qt/${QT%.*}/$QT/submodules/qttranslations-everywhere-src-$QT.tar.xz"

shasum -a 256 --check SHASUMS

echo "Installing SDL..."
tar xf "$SDL.tar.gz"
cd "$SDL"
./configure --prefix "$INSTALLDIR" --without-x
make "-j$NPROCS"
make install
cd ..

echo "Installing curl..."
tar xf "curl-$CURL.tar.gz"
cd "curl-$CURL"
./configure --prefix "$INSTALLDIR" --with-secure-transport
make "-j$NPROCS"
make install
cd ..

echo "Installing MoltenVK..."
tar xf "v$MOLTENVK.tar.gz"
cd "MoltenVK-${MOLTENVK}"
./fetchDependencies --macos
make macos
cp Package/Latest/MoltenVK/dylib/macOS/libMoltenVK.dylib $HOME/deps/lib/
cd ..

echo "Installing Qt Base..."
tar xf "qtbase-everywhere-src-$QT.tar.xz"
cd "qtbase-everywhere-src-$QT"
mkdir build
cd build
cmake -G Ninja -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" -DCMAKE_BUILD_TYPE=Release -DFEATURE_optimize_size=ON -DFEATURE_dbus=OFF -DFEATURE_framework=OFF -DFEATURE_icu=OFF -DFEATURE_opengl=OFF -DFEATURE_printsupport=OFF -DFEATURE_sql=OFF ..
cmake --build . --parallel
cmake --install .
cd ../../

echo "Installing Qt SVG..."
tar xf "qtsvg-everywhere-src-$QT.tar.xz"
cd "qtsvg-everywhere-src-$QT"
mkdir build
cd build
cmake -G Ninja -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" -DCMAKE_BUILD_TYPE=MinSizeRel ..
cmake --build . --parallel
cmake --install .
cd ../../

echo "Installing Qt Tools..."
tar xf "qttools-everywhere-src-$QT.tar.xz"
cd "qttools-everywhere-src-$QT"
# Linguist relies on a library in the Designer target, which takes 5-7 minutes to build on the CI
# Avoid it by not building Linguist, since we only need the tools that come with it
patch -u src/linguist/CMakeLists.txt <<EOF
--- src/linguist/CMakeLists.txt
+++ src/linguist/CMakeLists.txt
@@ -14,7 +14,7 @@
 add_subdirectory(lrelease-pro)
 add_subdirectory(lupdate)
 add_subdirectory(lupdate-pro)
-if(QT_FEATURE_process AND QT_FEATURE_pushbutton AND QT_FEATURE_toolbutton AND TARGET Qt::Widgets AND NOT no-png)
+if(QT_FEATURE_process AND QT_FEATURE_pushbutton AND QT_FEATURE_toolbutton AND TARGET Qt::Widgets AND TARGET Qt::PrintSupport AND NOT no-png)
     add_subdirectory(linguist)
 endif()
EOF
mkdir build
cd build
cmake -G Ninja -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" -DCMAKE_BUILD_TYPE=Release -DFEATURE_assistant=OFF -DFEATURE_clang=OFF -DFEATURE_designer=OFF -DFEATURE_kmap2qmap=OFF -DFEATURE_pixeltool=OFF -DFEATURE_pkg_config=OFF -DFEATURE_qev=OFF -DFEATURE_qtattributionsscanner=OFF -DFEATURE_qtdiag=OFF -DFEATURE_qtplugininfo=OFF ..
cmake --build . --parallel
cmake --install .
cd ../../

echo "Installing Qt Translations..."
tar xf "qttranslations-everywhere-src-$QT.tar.xz"
cd "qttranslations-everywhere-src-$QT"
mkdir build
cd build
cmake -G Ninja -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" -DCMAKE_BUILD_TYPE=Release ..
cmake --build . --parallel
cmake --install .
cd ../../

echo "Cleaning up..."
cd ..
rm -fr deps-build
