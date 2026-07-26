#!/usr/bin/env bash
sudo apt install -y acpica-tools build-essential cmake curl device-tree-compiler gcc-aarch64-linux-gnu git meson ninja-build pkg-config python3 python3-pip python3-venv uuid-dev xz-utils
apiLevel=36
glibVer=2.88.2
libffiVer=3.7.1
libucontextVer=1.5.2
libusbVer=1.0.30
pcre2Ver=10.47
pixmanVer=0.46.4
zlibVer=1.3.1
nCpu="$(nproc)"
ndkPath="${ndkPath:-$HOME/android-ndk-r30-beta2}"
targetTriple=aarch64-linux-android
scriptDir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
qemuSrc="$scriptDir"
buildDir="$qemuSrc/build"
outDir="$buildDir/out"
prefix="$buildDir/sysroot"
srcDir="$buildDir/src"
qemuBuild="$buildDir/qemu"
toolchain="$ndkPath/toolchains/llvm/prebuilt/linux-x86_64"
fetch() {
  [ -f "$2" ] || curl -L --fail -o "$2" "$1"
}
buildSysroot() {
  export CC="$toolchain/bin/${targetTriple}${apiLevel}-clang" CXX="$toolchain/bin/${targetTriple}${apiLevel}-clang++"
  export AR="$toolchain/bin/llvm-ar" NM="$toolchain/bin/llvm-nm" OBJCOPY="$toolchain/bin/llvm-objcopy" RANLIB="$toolchain/bin/llvm-ranlib" STRIP="$toolchain/bin/llvm-strip"
  export CFLAGS="-fPIC -fPIE -ftls-model=global-dynamic" CXXFLAGS="$CFLAGS" LDFLAGS="-pie"
  export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig" PKG_CONFIG_PATH="$prefix/lib/pkgconfig"
  cd "$srcDir"
  fetch "https://github.com/libffi/libffi/releases/download/v${libffiVer}/libffi-${libffiVer}.tar.gz" "libffi-${libffiVer}.tar.gz"
  [ -d "libffi-${libffiVer}" ] || tar xf "libffi-${libffiVer}.tar.gz"
  mkdir -p "$outDir/libffi"
  cd "$outDir/libffi"
  "$srcDir/libffi-${libffiVer}/configure" --host="$targetTriple" --prefix="$prefix" --enable-shared --disable-static --disable-exec-static-tramp
  make -j"$nCpu"
  make install
  cd "$srcDir"
  fetch "https://github.com/madler/zlib/releases/download/v${zlibVer}/zlib-${zlibVer}.tar.gz" "zlib-${zlibVer}.tar.gz"
  [ -d "zlib-${zlibVer}" ] || tar xf "zlib-${zlibVer}.tar.gz"
  mkdir -p "$outDir/zlib"
  cd "$outDir/zlib"
  cmake -G Ninja "$srcDir/zlib-${zlibVer}" -DCMAKE_TOOLCHAIN_FILE="$ndkPath/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM="android-${apiLevel}" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DINSTALL_PKGCONFIG_DIR="$prefix/lib/pkgconfig" -DBUILD_SHARED_LIBS=ON -DZLIB_BUILD_EXAMPLES=OFF -DCMAKE_C_FLAGS="-fPIC -fPIE -ftls-model=global-dynamic" -DCMAKE_SHARED_LINKER_FLAGS="-Wl,--undefined-version"
  cmake --build . -j"$nCpu"
  cmake --install .
  cd "$srcDir"
  fetch "https://github.com/PhilipHazel/pcre2/releases/download/pcre2-${pcre2Ver}/pcre2-${pcre2Ver}.tar.bz2" "pcre2-${pcre2Ver}.tar.bz2"
  [ -d "pcre2-${pcre2Ver}" ] || tar xf "pcre2-${pcre2Ver}.tar.bz2"
  mkdir -p "$outDir/pcre2"
  cd "$outDir/pcre2"
  cmake -G Ninja "$srcDir/pcre2-${pcre2Ver}" -DCMAKE_TOOLCHAIN_FILE="$ndkPath/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM="android-${apiLevel}" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DZLIB_LIBRARY="$prefix/lib/libz.so" -DZLIB_LIBRARY_RELEASE="$prefix/lib/libz.so" -DZLIB_LIBRARY_DEBUG="$prefix/lib/libz.so" -DZLIB_INCLUDE_DIR="$prefix/include" -DCMAKE_C_FLAGS="-ftls-model=global-dynamic" -DCMAKE_SHARED_LINKER_FLAGS="-Wl,--undefined-version" -DBUILD_SHARED_LIBS=ON -DPCRE2_BUILD_PCRE2_8=ON -DPCRE2_BUILD_PCRE2_16=OFF -DPCRE2_BUILD_PCRE2_32=OFF -DPCRE2_SUPPORT_JIT=OFF
  cmake --build . -j"$nCpu"
  cmake --install .
  cd "$srcDir"
  fetch "https://download.gnome.org/sources/glib/${glibVer%.*}/glib-${glibVer}.tar.xz" "glib-${glibVer}.tar.xz"
  [ -d "glib-${glibVer}" ] || tar xf "glib-${glibVer}.tar.xz"
  perl -i -ne "print unless /'lchmod',/" "glib-${glibVer}/meson.build"
  perl -i -ne "if (/pthread_getaffinity_np/) { s/if cc/if false and cc/ } print" "glib-${glibVer}/meson.build"
  cat > "$outDir/glib.cross" <<EOF
[binaries]
c = '$CC'
cpp = '$CXX'
ar = '$AR'
strip = '$STRIP'
pkg-config = 'pkg-config'
[built-in options]
c_args = ['-fPIC','-fPIE','-ftls-model=global-dynamic']
c_link_args = ['-pie']
[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
EOF
  mkdir -p "$outDir/glib"
  cd "$outDir/glib"
  meson setup . "$srcDir/glib-${glibVer}" --cross-file "$outDir/glib.cross" --prefix "$prefix" -Ddefault_library=shared -Doptimization=2 -Ddebug=false -Dglib_debug=disabled -Dtests=false -Dman-pages=disabled -Ddocumentation=false -Dselinux=disabled -Dlibmount=disabled -Dnls=disabled
  meson compile -j"$nCpu"
  meson install
  cd "$srcDir"
  fetch "https://www.cairographics.org/releases/pixman-${pixmanVer}.tar.gz" "pixman-${pixmanVer}.tar.gz"
  [ -d "pixman-${pixmanVer}" ] || tar xf "pixman-${pixmanVer}.tar.gz"
  mkdir -p "$outDir/pixman"
  cd "$outDir/pixman"
  meson setup . "$srcDir/pixman-${pixmanVer}" --cross-file "$outDir/glib.cross" --prefix "$prefix" -Ddefault_library=shared -Dtests=disabled
  meson compile -j"$nCpu"
  meson install
  cd "$srcDir"
  fetch "https://github.com/libusb/libusb/releases/download/v${libusbVer}/libusb-${libusbVer}.tar.bz2" "libusb-${libusbVer}.tar.bz2"
  [ -d "libusb-${libusbVer}" ] || tar xf "libusb-${libusbVer}.tar.bz2"
  mkdir -p "$outDir/libusb"
  cd "$outDir/libusb"
  "$srcDir/libusb-${libusbVer}/configure" --host="$targetTriple" --prefix="$prefix" --enable-shared --disable-static --disable-udev
  make -j"$nCpu"
  make install
  cd "$srcDir"
  fetch "https://github.com/kaniini/libucontext/archive/refs/tags/libucontext-${libucontextVer}.tar.gz" "libucontext-${libucontextVer}.tar.gz"
  [ -d "libucontext-libucontext-${libucontextVer}" ] || tar xf "libucontext-${libucontextVer}.tar.gz"
  cd "libucontext-libucontext-${libucontextVer}"
  make clean
  make ARCH=aarch64 CC="$CC" AR="$AR" RANLIB="$RANLIB" FREESTANDING=yes EXPORT_UNPREFIXED=yes -j"$nCpu" libucontext.a libucontext.pc
  mkdir -p "$prefix/lib/pkgconfig" "$prefix/include/libucontext"
  cp -f libucontext.a "$prefix/lib/"
  cp -f libucontext.pc "$prefix/lib/pkgconfig/"
  cp -f include/libucontext/libucontext.h "$prefix/include/libucontext/"
  cat > "$prefix/include/ucontext.h" <<'EOF'
#ifndef _ANDROID_UCONTEXT_SHIM_H
#define _ANDROID_UCONTEXT_SHIM_H
#include <sys/ucontext.h>
#include <libucontext/libucontext.h>
extern int getcontext(ucontext_t *);
extern int setcontext(const ucontext_t *);
extern void makecontext(ucontext_t *, void (*)(void), int, ...);
extern int swapcontext(ucontext_t *, const ucontext_t *);
#endif
EOF
  cat > "$prefix/include/libucontext/bits.h" <<'EOF'
#ifndef LIBUCONTEXT_BITS_H
#define LIBUCONTEXT_BITS_H
#include <stddef.h>
typedef struct {
  unsigned long long fault_address;
  unsigned long long regs[31];
  unsigned long long sp;
  unsigned long long pc;
  unsigned long long pstate;
  unsigned char __reserved[4096] __attribute__((__aligned__(16)));
} libucontext_mcontext_t;
typedef struct {
  void *ss_sp;
  int ss_flags;
  size_t ss_size;
} libucontext_stack_t;
typedef struct libucontext_ucontext {
  unsigned long uc_flags;
  struct libucontext_ucontext *uc_link;
  libucontext_stack_t uc_stack;
  unsigned char __pad[136];
  libucontext_mcontext_t uc_mcontext;
} libucontext_ucontext_t;
#endif
EOF
  perl -0pi -e 's[void [(][*][)][(][)]][void (*)(void)]g' "$prefix/include/libucontext/libucontext.h"
}
buildQemu() {
  cd "$qemuSrc"
  mkdir -p pyvenv subprojects/packagefiles/berkeley-softfloat-3 subprojects/packagefiles/berkeley-testfloat-3
  fetch "https://raw.githubusercontent.com/qemu/qemu/master/pyvenv/meson.build" pyvenv/meson.build
  fetch "https://raw.githubusercontent.com/qemu/qemu/master/subprojects/packagefiles/berkeley-softfloat-3/meson.build" subprojects/packagefiles/berkeley-softfloat-3/meson.build
  fetch "https://raw.githubusercontent.com/qemu/qemu/master/subprojects/packagefiles/berkeley-softfloat-3/meson_options.txt" subprojects/packagefiles/berkeley-softfloat-3/meson_options.txt
  fetch "https://raw.githubusercontent.com/qemu/qemu/master/subprojects/packagefiles/berkeley-testfloat-3/meson.build" subprojects/packagefiles/berkeley-testfloat-3/meson.build
  fetch "https://raw.githubusercontent.com/qemu/qemu/master/subprojects/packagefiles/berkeley-testfloat-3/meson_options.txt" subprojects/packagefiles/berkeley-testfloat-3/meson_options.txt
  export CC="$toolchain/bin/${targetTriple}${apiLevel}-clang" CXX="$toolchain/bin/${targetTriple}${apiLevel}-clang++"
  rm -rf "$qemuBuild"
  mkdir -p "$qemuBuild"
  cd "$qemuBuild"
  env PATH="$toolchain/bin:$prefix/bin:$PATH" PKG_CONFIG=/usr/bin/pkg-config PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig" PKG_CONFIG_PATH="$prefix/lib/pkgconfig" LDFLAGS="-L$prefix/lib -Wl,-rpath-link,$prefix/lib -lucontext" AR="$toolchain/bin/llvm-ar" NM="$toolchain/bin/llvm-nm" OBJCOPY="$toolchain/bin/llvm-objcopy" RANLIB="$toolchain/bin/llvm-ranlib" STRIP="$toolchain/bin/llvm-strip" "$qemuSrc/configure" --cpu=aarch64 --cross-prefix="${targetTriple}-" --target-list=aarch64-softmmu --enable-gzvm --enable-tcg --with-coroutine=ucontext --extra-cflags="-I$prefix/include -I$qemuSrc/linux-headers" --disable-dbus-display --disable-docs --disable-gio --disable-hvf --disable-kvm --disable-multiprocess --disable-nitro --disable-opengl --disable-tools --disable-guest-agent --disable-usb-redir --disable-virtfs --disable-vduse-blk-export --disable-libvduse --disable-vhost-crypto --disable-vhost-kernel --disable-vhost-net --disable-vhost-user --disable-vhost-user-blk-server --disable-vhost-vdpa --disable-virglrenderer --disable-werror --disable-tpm --disable-install-blobs
  ninja -C "$qemuBuild" -j"$nCpu" qemu-system-aarch64
}
packageQemu() {
  local dst="$outDir/qemu-gzvm"
  local deflateSize
  deflateSize=$("$toolchain/bin/llvm-readelf" --dyn-symbols --wide "$prefix/lib/libz.so" | awk '$8 ~ /^deflate(@|$)/ {print $3; exit}')
  if [ -z "$deflateSize" ] || [ "$deflateSize" -le 8 ]; then
    echo "invalid zlib implementation" >&2
    return 1
  fi
  rm -rf "$dst"
  mkdir -p "$dst"
  cp -L "$qemuBuild/qemu-system-aarch64" "$prefix/lib/libpixman-1.so.0" "$prefix/lib/libusb-1.0.so" "$prefix/lib/libglib-2.0.so.0" "$prefix/lib/libgmodule-2.0.so.0" "$prefix/lib/libintl.so.8" "$prefix/lib/libpcre2-8.so" "$prefix/lib/libz.so" "$dst"
  install -D "$qemuSrc/pc-bios/keymaps/en-us" "$dst/pc-bios/keymaps/en-us"
  install -D "$qemuSrc/pc-bios/efi-virtio.rom" "$dst/pc-bios/efi-virtio.rom"
  "$toolchain/bin/llvm-strip" --strip-all "$dst/qemu-system-aarch64" "$dst"/*.so*
}
mkdir -p "$outDir" "$prefix" "$srcDir"
buildSysroot
buildQemu
packageQemu