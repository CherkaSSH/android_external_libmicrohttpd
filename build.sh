#!/bin/bash
set -e

NDK_ZIP="android-ndk-r29-linux.zip"
NDK_URL="https://dl.google.com/android/repository/android-ndk-r29-linux.zip?hl=zh-cn"

if [ ! -f "$NDK_ZIP" ]; then
    echo "Downloading $NDK_ZIP ..."
    wget -O "$NDK_ZIP" "$NDK_URL"
else
    echo "$NDK_ZIP already exists, skip download."
fi

if [ ! -d "android-ndk-r29" ]; then
    echo "Unzipping NDK..."
    unzip -q "$NDK_ZIP"
else
    echo "android-ndk-r29 directory already exists, skip unzip."
fi

NDK=$(pwd)/android-ndk-r29
SYSROOT=$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot
TOOLCHAIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
HOST=aarch64-linux-android
API=35
PREFIX=$(pwd)/build

export PATH="$TOOLCHAIN:$PATH"

MHD_VERSION=1.0.2
MHD_TAR="libmicrohttpd-$MHD_VERSION.tar.gz"
MHD_URL="https://ftp.gnu.org/gnu/libmicrohttpd/$MHD_TAR"

if [ ! -f "$MHD_TAR" ]; then
    echo "Downloading $MHD_TAR..."
    wget "$MHD_URL"
else
    echo "$MHD_TAR already exists, skip download."
fi

if [ ! -d "libmicrohttpd-$MHD_VERSION" ]; then
    echo "Extracting $MHD_TAR..."
    tar xf "$MHD_TAR"
else
    echo "Source directory libmicrohttpd-$MHD_VERSION already exists, skip extract."
fi

cd libmicrohttpd-$MHD_VERSION

export CC="$TOOLCHAIN/${HOST}${API}-clang"
export CXX="$TOOLCHAIN/${HOST}${API}-clang++"
export AR="$TOOLCHAIN/llvm-ar"
export AS="$TOOLCHAIN/llvm-as"
export LD="$TOOLCHAIN/ld.lld"
export RANLIB="$TOOLCHAIN/llvm-ranlib"
export STRIP="$TOOLCHAIN/llvm-strip"

export CFLAGS="--sysroot=$SYSROOT -O2 -fPIC -static"
export LDFLAGS="--sysroot=$SYSROOT -static"

./configure \
    --host=$HOST \
    --prefix=$PREFIX \
    --disable-shared \
    --enable-static \
    AR="$AR" RANLIB="$RANLIB" CC="$CC"

make -j$(nproc)
make install

echo "==== libmicrohttpd build finished ===="
echo "Static library at: $PREFIX/lib/libmicrohttpd.a"

cd ..
export PATH="$TOOLCHAIN:$PATH"

aarch64-linux-android${API}-clang \
    server.c -I./build/include ./build/lib/libmicrohttpd.a \
    -pthread -static -o microhttpd

echo "==== Build complete ===="
