#!/usr/bin/env bash
set -euo pipefail
sudo apt install -y cmake curl git meson ninja-build patchelf perl pkg-config python3 python3-pip python3-venv zstd
qemuSrc="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
buildDir="$qemuSrc/build"
srcDir="$buildDir/src"
outDir="$buildDir/out"
prefix="$buildDir/sysroot"
qemuBuild="$buildDir/qemu"
sysLib="$prefix/lib"
libucontextSrc="$srcDir/libucontext"
liburingSrc="$srcDir/liburing"
sdlSrc="$srcDir/SDL2"
termuxDebDir="$srcDir/termux"
termuxPackages="$termuxDebDir/Packages"
qemuDir="$outDir/qemu-gzvm"
qemuFw="$qemuDir/fw"
qemuLib="$qemuDir/lib"
apiLevel="36"
glibVer="2.83.0"
libffiVer="3.4.4"
pcre2Ver="10.44"
pixmanVer="0.42.2"
targetTriple="aarch64-linux-android"
cmakeAbi="arm64-v8a"
mesonCpu="aarch64"
hostOs=$(uname -s | tr '[:upper:]' '[:lower:]')
nCpu="$(nproc || sysctl -n hw.ncpu)"
ndkPath="${ndkPath:-$HOME/android-ndk-r30-beta2}"
libucontextGitUrl="https://github.com/kaniini/libucontext.git"
liburingGitUrl="https://github.com/axboe/liburing.git"
sdlGitUrl="https://github.com/libsdl-org/SDL.git"
termuxRepo="https://packages.termux.dev/apt/termux-main"
qemuRawUrl="https://gitlab.com/qemu-project/qemu/-/raw/master"
case "$hostOs" in
  darwin) hostTag="darwin-x86_64" ;;
  linux) hostTag="linux-x86_64" ;;
  *) echo "不支持的系统: $hostOs" >&2; exit 1 ;;
esac
toolchain="$ndkPath/toolchains/llvm/prebuilt/$hostTag"
readelf="$toolchain/bin/llvm-readelf"
strip="$toolchain/bin/llvm-strip"
hostCC="${HOST_CC:-$(command -v cc || true)}"
if [ -z "$hostCC" ]; then
  echo "缺少宿主机编译器" >&2
  exit 1
fi
fetch() {
  local url="$1" out="$2"
  if [ ! -f "$out" ]; then
    echo "下载 $url"
    curl -L --fail --retry 3 -o "$out" "$url"
  fi
}
fetchGit() {
  local url="$1" dir="$2"
  shift 2
  if [ ! -d "$dir" ]; then
    echo "克隆 $url -> $dir"
    git clone --depth 1 --single-branch --no-tags --filter=blob:none --recurse-submodules --shallow-submodules --also-filter-submodules --jobs "$nCpu" "$@" "$url" "$dir"
  fi
}
termuxPackagePath() {
  local packageName=$1
  if [ ! -s "$termuxPackages" ]; then
    mkdir -p "$termuxDebDir"
    curl -L --fail --retry 3 -o "$termuxPackages" "$termuxRepo/dists/stable/main/binary-aarch64/Packages"
  fi
  awk -v p="$packageName" '
    BEGIN { RS=""; FS="\n" }
    {
      name = file = arch = ""
      for (i = 1; i <= NF; i++) {
        if ($i ~ /^Package: /) name = substr($i, 10)
        if ($i ~ /^Architecture: /) arch = substr($i, 15)
        if ($i ~ /^Filename: /) file = substr($i, 11)
      }
      if (name == p && (arch == "aarch64" || arch == "all")) print file
    }' "$termuxPackages" | tail -n1
}
fetchTermuxDeb() {
  local packageName=$1 packagePath debName
  packagePath="$(termuxPackagePath "$packageName")"
  if [ -z "$packagePath" ]; then
    echo "缺少 Termux 包: $packageName" >&2
    return 1
  fi
  debName="$(basename "$packagePath")"
  mkdir -p "$termuxDebDir"
  if [ ! -f "$termuxDebDir/$debName" ]; then
    curl -L --fail --retry 3 -C - -o "$termuxDebDir/$debName" "$termuxRepo/$packagePath"
  fi
  echo "$termuxDebDir/$debName"
}
writeMesonCross() {
  local file=$1 extraC=${2:-} extraLink=${3:-}
  cat > "$file" <<EOF
[binaries]
c = '$CC'
cpp = '$CXX'
ar = '$AR'
strip = '$STRIP'
pkg-config = '${PKG_CONFIG:-pkg-config}'
[built-in options]
c_args = ['-fPIC','-fPIE','-ftls-model=global-dynamic'$extraC]
cpp_args = ['-fPIC','-fPIE','-ftls-model=global-dynamic'$extraC]
c_link_args = ['-pie'$extraLink]
cpp_link_args = ['-pie'$extraLink]
[host_machine]
system = 'linux'
cpu_family = '$mesonCpu'
cpu = '$mesonCpu'
endian = 'little'
EOF
}
x11SocketPlaceholders() {
  local file x11From ximFrom
  x11From="$(printf '/data/data/com.termux/files/usr/tmp/%s' '.X11-unix/X')"
  ximFrom="$(printf '/data/data/com.termux/files/usr/tmp/%s' '.XIM-unix/XIM')"
  for file in "$@"; do
    [ -f "$file" ] || continue
    X11_FROM="$x11From" XIM_FROM="$ximFrom" perl -0pi -e '
      sub fit { $_[1] . "\0" x (length($_[0]) - length($_[1])) }
      my $x11_to = "X11_TMPDIR_PLACEHOLDER/.X11-unix/X";
      my $xim_to = "X11_TMPDIR_PLACEHOLDER/.XIM-unix/XIM";
      s/\Q$ENV{X11_FROM}\E/fit($ENV{X11_FROM}, $x11_to)/eg;
      s/\Q$ENV{XIM_FROM}\E/fit($ENV{XIM_FROM}, $xim_to)/eg;
    ' "$file"
  done
}
buildX11PathShim() {
  if [ -f "$prefix/lib/libX11-dir.so" ]; then
    return 0
  fi
  local src="$outDir/x11-dir.c"
  cat > "$src" <<'EOF'
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
typedef int (*connect_func)(int, const struct sockaddr *, socklen_t);
static connect_func real_connect_func(void)
{
    static connect_func fn;
    if (!fn) {
        fn = (connect_func)dlsym(RTLD_NEXT, "connect");
    }
    return fn;
}
static int make_path(char *out, size_t out_len, const char *dir,
                     const char *leaf)
{
    size_t dir_len;
    size_t leaf_len;
    int add_slash;
    if (!dir || !*dir) {
        return 0;
    }
    dir_len = strlen(dir);
    leaf_len = strlen(leaf);
    add_slash = dir[dir_len - 1] != '/';
    if (dir_len + add_slash + leaf_len + 1 > out_len) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(out, dir, dir_len);
    if (add_slash) {
        out[dir_len++] = '/';
    }
    memcpy(out + dir_len, leaf, leaf_len + 1);
    return 1;
}
static int rewrite_path(const char *path, char *out, size_t out_len)
{
    const char *p;
    char dir[sizeof(((struct sockaddr_un *)0)->sun_path)];
    p = strstr(path, "/.X11-unix/");
    if (p) {
        const char *leaf = p + strlen("/.X11-unix/");
        const char *socket_dir = getenv("X11_SOCKET_DIR");
        const char *tmpdir = getenv("X11_TMPDIR");
        if (socket_dir && *socket_dir) {
            return make_path(out, out_len, socket_dir, leaf);
        }
        if (tmpdir && *tmpdir) {
            if (snprintf(dir, sizeof(dir), "%s/.X11-unix", tmpdir) >=
                (int)sizeof(dir)) {
                errno = ENAMETOOLONG;
                return -1;
            }
            return make_path(out, out_len, dir, leaf);
        }
        return 0;
    }
    p = strstr(path, "/.XIM-unix/");
    if (p) {
        const char *leaf = p + strlen("/.XIM-unix/");
        const char *socket_dir = getenv("XIM_SOCKET_DIR");
        const char *tmpdir = getenv("X11_TMPDIR");

        if (socket_dir && *socket_dir) {
            return make_path(out, out_len, socket_dir, leaf);
        }
        if (tmpdir && *tmpdir) {
            if (snprintf(dir, sizeof(dir), "%s/.XIM-unix", tmpdir) >=
                (int)sizeof(dir)) {
                errno = ENAMETOOLONG;
                return -1;
            }
            return make_path(out, out_len, dir, leaf);
        }
    }
    return 0;
}
int connect(int fd, const struct sockaddr *addr, socklen_t addrlen)
{
    struct sockaddr_un rewritten;
    char new_path[sizeof(rewritten.sun_path)];
    connect_func real_connect = real_connect_func();
    const struct sockaddr_un *un;
    int ret;
    if (!real_connect) {
        errno = ENOSYS;
        return -1;
    }
    if (!addr || addr->sa_family != AF_UNIX ||
        addrlen <= offsetof(struct sockaddr_un, sun_path)) {
        return real_connect(fd, addr, addrlen);
    }
    un = (const struct sockaddr_un *)addr;
    if (!un->sun_path[0]) {
        return real_connect(fd, addr, addrlen);
    }
    ret = rewrite_path(un->sun_path, new_path, sizeof(new_path));
    if (ret <= 0) {
        return ret < 0 ? -1 : real_connect(fd, addr, addrlen);
    }
    memset(&rewritten, 0, sizeof(rewritten));
    rewritten.sun_family = AF_UNIX;
    memcpy(rewritten.sun_path, new_path, strlen(new_path) + 1);
    return real_connect(fd, (const struct sockaddr *)&rewritten,
                        offsetof(struct sockaddr_un, sun_path) +
                        strlen(rewritten.sun_path) + 1);
}
EOF
  "$CC" -shared -fPIC -Os -Wl,--gc-sections -Wl,-s -o "$prefix/lib/libX11-dir.so" "$src" -ldl
}
isSystemLib() {
  case "$1" in
    libc.so|libm.so|libdl.so|liblog.so|libz.so|libandroid.so|libaaudio.so|libOpenSLES.so) return 0 ;;
    *) return 1 ;;
  esac
}
neededLibs() {
  "$readelf" -d "$1" | awk 'index($0, "Shared library: [") {
    name = $0
    sub(/^.*Shared library: [[]/, "", name)
    sub(/[]].*$/, "", name)
    print name
  }'
}
findLib() {
  local neededName=$1 baseName=$1
  if [ -f "$sysLib/$neededName" ]; then
    echo "$sysLib/$neededName"
    return 0
  fi
  while [[ "$baseName" == *.so.* ]]; do
    baseName="${baseName%.*}"
    if [ -f "$sysLib/$baseName" ]; then
      echo "$sysLib/$baseName"
      return 0
    fi
  done
  return 1
}
copyLib() {
  local neededName=$1 destPath sourcePath
  destPath="$qemuLib/$neededName"
  if isSystemLib "$neededName"; then
    return 0
  fi
  if ! sourcePath="$(findLib "$neededName")"; then
    echo "缺少依赖: $neededName" >&2
    return 1
  fi
  if [ ! -f "$destPath" ]; then
    cp -Lf "$sourcePath" "$destPath"
    patchelf --set-soname "$neededName" "$destPath"
    pendingElfs+=("$destPath")
  fi
}
collectLib() {
  local elfPath neededName queueIndex=0
  pendingElfs=("$@")
  while [ "$queueIndex" -lt "${#pendingElfs[@]}" ]; do
    elfPath="${pendingElfs[$queueIndex]}"
    queueIndex=$((queueIndex + 1))
    while IFS= read -r neededName; do
      [ -z "$neededName" ] && continue
      if [ "$neededName" = "libandroid-support.so" ]; then
        patchelf --remove-needed "$neededName" "$elfPath"
        continue
      fi
      copyLib "$neededName"
    done < <(neededLibs "$elfPath")
  done
}
setupToolchain() {
  export AR="$toolchain/bin/llvm-ar"
  export CC="$toolchain/bin/${targetTriple}${apiLevel}-clang"
  export CXX="$toolchain/bin/${targetTriple}${apiLevel}-clang++"
  export LD="$toolchain/bin/ld.lld"
  export NM="$toolchain/bin/llvm-nm"
  export OBJCOPY="$toolchain/bin/llvm-objcopy"
  export RANLIB="$toolchain/bin/llvm-ranlib"
  export STRIP="$toolchain/bin/llvm-strip"
  export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig"
  export PKG_CONFIG_PATH="$prefix/lib/pkgconfig"
}
fetchSources() {
  mkdir -p "$srcDir" "$outDir" "$prefix/lib" "$prefix/include"
  cd "$srcDir"
  fetchGit "$libucontextGitUrl" "$libucontextSrc"
  fetchGit "$liburingGitUrl" "$liburingSrc" --branch liburing-2.8
  fetchGit "$sdlGitUrl" "$sdlSrc" --branch SDL2
  fetch "https://github.com/libffi/libffi/releases/download/v${libffiVer}/libffi-${libffiVer}.tar.gz" "$srcDir/libffi-${libffiVer}.tar.gz"
  fetch "https://github.com/PhilipHazel/pcre2/releases/download/pcre2-${pcre2Ver}/pcre2-${pcre2Ver}.tar.bz2" "$srcDir/pcre2-${pcre2Ver}.tar.bz2"
  fetch "https://download.gnome.org/sources/glib/${glibVer%.*}/glib-${glibVer}.tar.xz" "$srcDir/glib-${glibVer}.tar.xz"
  fetch "https://www.cairographics.org/releases/pixman-${pixmanVer}.tar.gz" "$srcDir/pixman-${pixmanVer}.tar.gz"
  for pkg in libandroid-shmem libx11 libxau libxcb libxcursor libxdmcp libxext libxfixes libxi libxrandr libxrender xorgproto; do
    fetchTermuxDeb "$pkg" > /dev/null
  done
  if [ -f "$qemuSrc/subprojects/dtc.wrap" ] || [ -f "$qemuSrc/subprojects/keycodemapdb.wrap" ]; then
    echo "准备 QEMU Meson 子项目"
    (cd "$qemuSrc" && meson subprojects download dtc keycodemapdb)
  fi
}
buildLibffi() {
  if [ -f "$prefix/lib/libffi.so" ]; then
    return 0
  fi
  cd "$srcDir"
  [ -d "libffi-${libffiVer}" ] || tar xf "libffi-${libffiVer}.tar.gz"
  mkdir -p "$outDir/libffi"
  cd "$outDir/libffi"
  if [ -f Makefile ]; then
    make distclean
  fi
  echo "配置 libffi ${libffiVer}"
  "$srcDir/libffi-${libffiVer}/configure" --host="$targetTriple" --prefix="$prefix" --enable-shared --disable-static --disable-exec-static-tramp
  echo "编译 libffi"
  make -j"$nCpu"
  make install
}
buildPcre2() {
  if [ -f "$prefix/lib/libpcre2-8.so" ]; then
    return 0
  fi
  cd "$srcDir"
  [ -d "pcre2-${pcre2Ver}" ] || tar xf "pcre2-${pcre2Ver}.tar.bz2"
  mkdir -p "$outDir/pcre2"
  cd "$outDir/pcre2"
  echo "配置 PCRE2 ${pcre2Ver}"
  cmake -G Ninja "$srcDir/pcre2-${pcre2Ver}" -DCMAKE_TOOLCHAIN_FILE="$ndkPath/build/cmake/android.toolchain.cmake" -DANDROID_ABI="$cmakeAbi" -DANDROID_PLATFORM="android-${apiLevel}" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_C_FLAGS="-ftls-model=global-dynamic" -DBUILD_SHARED_LIBS=ON -DPCRE2_BUILD_PCRE2_8=ON -DPCRE2_BUILD_PCRE2_16=OFF -DPCRE2_BUILD_PCRE2_32=OFF -DPCRE2_SUPPORT_JIT=OFF
  echo "编译 PCRE2"
  cmake --build . -j"$nCpu"
  cmake --install .
}
buildGlib() {
  if [ -f "$prefix/lib/libglib-2.0.so" ]; then
    return 0
  fi
  cd "$srcDir"
  [ -d "glib-${glibVer}" ] || tar xf "glib-${glibVer}.tar.xz"
  perl -0pi -e "s/\\n  'lchmod',//" "glib-${glibVer}/meson.build"
  perl -0pi -e "s/if cc\\.has_header_symbol\\('pthread\\.h', 'pthread_getaffinity_np', prefix : pthread_prefix\\)/if false and cc.has_header_symbol('pthread.h', 'pthread_getaffinity_np', prefix : pthread_prefix)/" "glib-${glibVer}/meson.build"
  writeMesonCross "$outDir/glib.cross"
  mkdir -p "$outDir/glib"
  cd "$outDir/glib"
  if [ -f build.ninja ]; then
    rm -rf ./*
  fi
  echo "配置 GLib ${glibVer}"
  meson setup . "$srcDir/glib-${glibVer}" --cross-file "$outDir/glib.cross" --prefix "$prefix" -Ddefault_library=shared -Doptimization=2 -Ddebug=false -Dglib_debug=disabled -Dtests=false -Dman-pages=disabled -Ddocumentation=false -Dselinux=disabled -Dlibmount=disabled -Dnls=disabled
  echo "编译 GLib"
  meson compile -j"$nCpu"
  meson install
}
buildPixman() {
  if [ -f "$prefix/lib/libpixman-1.so" ]; then
    return 0
  fi
  cd "$srcDir"
  [ -d "pixman-${pixmanVer}" ] || tar xf "pixman-${pixmanVer}.tar.gz"
  mkdir -p "$outDir/pixman"
  cd "$outDir/pixman"
  if [ -f Makefile ]; then
    make distclean
  fi
  echo "配置 pixman ${pixmanVer}"
  "$srcDir/pixman-${pixmanVer}/configure" --host="$targetTriple" --prefix="$prefix" --disable-static --disable-arm-a64-neon
  echo "编译 pixman"
  make -j"$nCpu"
  make install
}
buildLibucontext() {
  if [ -f "$prefix/lib/libucontext.a" ]; then
    return 0
  fi
  cd "$libucontextSrc"
  make clean || true
  make ARCH=aarch64 CC="$CC" AR="$AR" RANLIB="$RANLIB" FREESTANDING=yes EXPORT_UNPREFIXED=yes -j"$nCpu" libucontext.a libucontext.pc
  mkdir -p "$prefix/lib/pkgconfig" "$prefix/include/libucontext"
  cp -f libucontext.a "$prefix/lib/"
  cp -f libucontext.pc "$prefix/lib/pkgconfig/"
  cp -f include/libucontext/libucontext.h "$prefix/include/libucontext/"
}
buildLiburing() {
  if [ -f "$prefix/lib/liburing.a" ] && [ -f "$prefix/lib/pkgconfig/liburing.pc" ]; then
    return 0
  fi
  cd "$liburingSrc"
  make clean || true
  ./configure --prefix="$prefix" --cc="$CC" --cxx="$CXX"
  make library ENABLE_SHARED=0 -j"$nCpu"
  make install ENABLE_SHARED=0
  rm -f "$prefix/lib"/liburing.so* "$prefix/lib"/liburing-ffi.so*
}
writeUcontextShims() {
  mkdir -p "$prefix/include/libucontext"
  cat > "$prefix/include/ucontext.h" <<'EOF'
#ifndef _ANDROID_UCONTEXT_SHIM_H
#define _ANDROID_UCONTEXT_SHIM_H
#include <sys/ucontext.h>
#include <libucontext/libucontext.h>
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
  if grep -Fq 'void (*)()' "$prefix/include/libucontext/libucontext.h"; then
    perl -0pi -e 's[void [(][*][)][(][)]][void (*)(void)]g' "$prefix/include/libucontext/libucontext.h"
  fi
}
installTermuxX11() {
  local staleAlsRoot debName tmpDir d
  staleAlsRoot="$(printf '/data/local/tmp/%s' 'als')"
  if [ -f "$prefix/lib/libX11.so" ] && grep -a -l "$staleAlsRoot" "$prefix/lib/libX11.so" "$prefix/lib/libxcb.so" >/dev/null 2>&1; then
    rm -f "$prefix/lib"/libX11.so* "$prefix/lib"/libxcb.so*
  fi
  if [ -f "$prefix/lib/libX11.so" ] && [ -f "$prefix/lib/libandroid-shmem.so" ]; then
    return 0
  fi
  tmpDir="$outDir/x11_tmp"
  rm -rf "$tmpDir"
  mkdir -p "$tmpDir"
  cd "$tmpDir"
  for pkg in libandroid-shmem libx11 libxau libxcb libxdmcp libxext libxrender xorgproto; do
    debName="$(fetchTermuxDeb "$pkg")"
    cp -f "$debName" .
  done
  for deb in *.deb; do
    ar x "$deb"
    if [ -f data.tar.zst ]; then
      tar --zstd -xf data.tar.zst
    elif [ -f data.tar.xz ]; then
      tar -xf data.tar.xz
    fi
    rm -f "$deb" data.tar.* control.tar.* debian-binary
  done
  for d in usr data/data/com.termux/files/usr; do
    if [ -d "$d/include" ]; then
      cp -rf "$d/include/"* "$prefix/include/"
    fi
    if [ -d "$d/lib" ]; then
      cp -rf "$d/lib/"* "$prefix/lib/"
    fi
  done
  find "$prefix/lib/pkgconfig" -name "*.pc" -type f -exec sed -i "s|/data/data/com.termux/files/usr|$prefix|g" {} +
  cd "$outDir"
  rm -rf "$tmpDir"
}
buildSdl() {
  if [ -f "$prefix/lib/libSDL2.so" ]; then
    return 0
  fi
  local sdlConfigH="$sdlSrc/include/SDL_config_android.h"
  local sdlXinput2H="$sdlSrc/src/video/x11/SDL_x11xinput2.h"
  if [ -e "$sdlSrc/.git" ]; then
    git -C "$sdlSrc" checkout -- CMakeLists.txt include/SDL_config_android.h src/SDL.c src/video/x11/SDL_x11opengles.c src/video/x11/SDL_x11xinput2.h || true
  fi
  if [ -f "$sdlConfigH" ]; then
    sed -i '/SDL_VIDEO_DRIVER_X11/d;/SDL_VIDEO_DRIVER_ANDROID/d' "$sdlConfigH"
    awk '{ print } index($0, "/* Enable various video drivers */") { print "#define SDL_VIDEO_DRIVER_X11 1" }' "$sdlConfigH" > "$sdlConfigH.tmp" && mv "$sdlConfigH.tmp" "$sdlConfigH"
    sed -i '/SDL_VIDEO_OPENGL_ES/d;/SDL_VIDEO_OPENGL_ES2/d;/SDL_VIDEO_OPENGL_EGL/d;/SDL_VIDEO_RENDER_OGL_ES/d;/SDL_VIDEO_RENDER_OGL_ES2/d' "$sdlConfigH"
  fi
  if [ -f "$sdlSrc/src/SDL.c" ]; then
    perl -0pi -e 's[if [(][!]SDL_MainIsReady[)]][if (0 && !SDL_MainIsReady)]g' "$sdlSrc/src/SDL.c"
  fi
  if [ -f "$sdlXinput2H" ]; then
    sed -i '/^#ifndef SDL_VIDEO_DRIVER_X11_SUPPORTS_GENERIC_EVENTS$/,/^#endif$/d' "$sdlXinput2H"
  fi
  if ! grep -q 'ANDROID_X11_LIBS' "$sdlSrc/CMakeLists.txt"; then
    awk -v prefix="$prefix" '
      !done && index($0, "if(ANDROID)") {
        print
        print "  link_directories(" prefix "/lib)"
        print "  set(HAVE_X11 TRUE)"
        print "  set(HAVE_SDL_VIDEO TRUE)"
        print "  set(SDL_VIDEO_DRIVER_X11 1)"
        print "  set(ANDROID_X11_LIBS X11 Xext xcb Xau Xdmcp Xrender X11-xcb android-shmem)"
        print "  file(GLOB X11_SOURCES ${SDL2_SOURCE_DIR}/src/video/x11/*.c)"
        print "  list(APPEND SOURCE_FILES ${X11_SOURCES})"
        print "  list(APPEND SOURCE_FILES ${SDL2_SOURCE_DIR}/src/core/unix/SDL_poll.c)"
        print "  foreach(_LIB ${ANDROID_X11_LIBS})"
        print "    list(APPEND EXTRA_LIBS " prefix "/lib/lib${_LIB}.so)"
        print "  endforeach()"
        done = 1
        next
      }
      { print }
    ' "$sdlSrc/CMakeLists.txt" > "$sdlSrc/CMakeLists.txt.tmp" && mv "$sdlSrc/CMakeLists.txt.tmp" "$sdlSrc/CMakeLists.txt"
  fi
  rm -rf "$sdlSrc/build-android"
  rm -f "$prefix/lib/libSDL2.so" "$prefix/lib/pkgconfig/sdl2.pc"
  mkdir -p "$sdlSrc/build-android"
  cd "$sdlSrc/build-android"
  cmake .. -DCMAKE_TOOLCHAIN_FILE="$ndkPath/build/cmake/android.toolchain.cmake" -DANDROID_ABI="$cmakeAbi" -DANDROID_PLATFORM="android-$apiLevel" -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_FIND_ROOT_PATH="$prefix" -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=BOTH -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH -DCMAKE_PREFIX_PATH="$prefix" -DCMAKE_INCLUDE_PATH="$prefix/include" -DCMAKE_LIBRARY_PATH="$prefix/lib" -DCMAKE_C_FLAGS="$qemuCFlags" -DCMAKE_CXX_FLAGS="$qemuCFlags" -DCMAKE_SHARED_LINKER_FLAGS="-L$prefix/lib -landroid-shmem" -DCMAKE_EXE_LINKER_FLAGS="-L$prefix/lib -landroid-shmem" -DCMAKE_VERBOSE_MAKEFILE=ON -DSDL_STATIC=OFF -DSDL_SHARED=ON -DSDL_RENDER=ON -DSDL_X11=OFF -DSDL_X11_SHARED=OFF -DSDL_VULKAN=OFF -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF -DSDL_ANDROID=ON -DHAVE_X11_XLIB_H=1 -DX11_X11_LIB="$prefix/lib/libX11.so" -DX11_Xext_LIB="$prefix/lib/libXext.so" -DX11_Xrender_LIB="$prefix/lib/libXrender.so"
  make -j"$nCpu" install
}
buildSysroot() {
  setupToolchain
  export CFLAGS="-fPIC -fPIE -ftls-model=global-dynamic"
  export CXXFLAGS="$CFLAGS"
  export LDFLAGS="-pie"
  buildLibffi
  buildPcre2
  buildGlib
  buildPixman
  buildLibucontext
  buildLiburing
  writeUcontextShims
  installTermuxX11
  x11SocketPlaceholders "$prefix/lib/libxcb.so" "$prefix/lib/libX11.so"
  buildX11PathShim
  buildSdl
}
buildQemu() {
  setupToolchain
  local wrapPc="$outDir/android-pkg-config"
  {
    echo '#!/usr/bin/env bash'
    echo "export PKG_CONFIG_PATH='$prefix/lib/pkgconfig:$prefix/share/pkgconfig'"
    echo "export PKG_CONFIG_LIBDIR='$prefix/lib/pkgconfig:$prefix/share/pkgconfig'"
    echo 'exec pkg-config "$@"'
  } > "$wrapPc"
  chmod +x "$wrapPc"
  export PKG_CONFIG="$wrapPc"
  export CFLAGS="$qemuCFlags"
  export CPPFLAGS="$qemuCFlags"
  export LDFLAGS="$qemuLdFlags"
  local pixmanOpt
  if "$wrapPc" --exists pixman-1; then
    pixmanOpt="--enable-pixman"
  else
    pixmanOpt="--disable-pixman"
  fi
  mkdir -p "$qemuBuild"
  cd "$qemuBuild"
  if [ ! -f build.ninja ]; then
    "$qemuSrc/configure" --prefix="$prefix" --host-cc="$hostCC" --cross-prefix="${targetTriple}-" --cc="$CC" --cxx="$CXX" --extra-cflags="$qemuCFlags" --extra-ldflags="$qemuLdFlags -lX11 -lXext -lxcb -lXau -lXdmcp -lXrender -lX11-xcb -landroid-shmem" --target-list="aarch64-softmmu" --audio-drv-list=aaudio --with-coroutine=ucontext --disable-capstone --disable-cocoa --disable-curses --disable-docs --disable-download --disable-gcrypt --disable-gnutls --disable-guest-agent --disable-libusb --disable-pie --disable-plugins --disable-slirp --disable-tpm --disable-usb-redir --disable-vhost-kernel --disable-vhost-net --disable-vhost-user --disable-vhost-vdpa --disable-virtfs "$pixmanOpt" -Dattr=disabled -Dbochs=disabled -Dcloop=disabled -Dcoroutine_backend=sigaltstack -Dcoroutine_pool=false -Ddbus_display=disabled -Ddmg=disabled -Dgzvm=enabled -Dl2tpv3=disabled -Dlinux_io_uring=enabled -Dmultiprocess=disabled -Dparallels=disabled -Dqcow1=disabled -Dqed=disabled -Dreplication=disabled -Dsdl=enabled -Dtcg=disabled -Dtools=disabled -Dvdi=disabled -Dvhdx=disabled -Dvmdk=disabled -Dvpc=disabled -Dvvfat=disabled -Dxen=disabled -Dxen_pci_passthrough=disabled -Dzstd=disabled
  fi
  local meson="$qemuBuild/pyvenv/bin/meson"
  if [ ! -x "$meson" ]; then
    meson="$(command -v meson)"
  fi
  "$meson" compile -C "$qemuBuild" qemu-system-aarch64 -j"$nCpu"
}
packageQemu() {
  mkdir -p "$qemuLib" "$qemuFw/keymaps"
  fetch "$qemuRawUrl/pc-bios/efi-virtio.rom" "$qemuFw/efi-virtio.rom"
  fetch "$qemuRawUrl/pc-bios/keymaps/en-us" "$qemuFw/keymaps/en-us"
  if [ "${DEBUG:-0}" = "1" ]; then
    cp -f "$qemuBuild/qemu-system-aarch64" "$qemuDir/qemu-system-aarch64"
  else
    "$strip" --strip-all "$qemuBuild/qemu-system-aarch64" -o "$qemuDir/qemu-system-aarch64"
  fi
  patchelf --set-rpath '$ORIGIN/lib' "$qemuDir/qemu-system-aarch64"
  collectLib "$qemuDir/qemu-system-aarch64"
  x11SocketPlaceholders "$qemuLib/libxcb.so" "$qemuLib/libX11.so"
  cp -f "$prefix/lib/libX11-dir.so" "$qemuLib/"
  echo "产物: $qemuDir"
}
if [ "${DEBUG:-0}" = "1" ]; then
  qemuOptFlags="-Os -g -fno-omit-frame-pointer"
  qemuExtraLdFlags=""
else
  qemuOptFlags="-Os -fomit-frame-pointer -fno-unwind-tables -fno-asynchronous-unwind-tables"
  qemuExtraLdFlags=" -Wl,--icf=all -Wl,-s"
fi
qemuCFlags="-fPIC $qemuOptFlags -ffunction-sections -fdata-sections -fmerge-all-constants -mbranch-protection=none -ftls-model=global-dynamic -Wno-error -DSDL_MAIN_HANDLED -DANDROID_PLATFORM=android-${apiLevel} -I$prefix/include -I$prefix/include/pixman-1 -I$qemuSrc/linux-headers"
qemuLdFlags="-L$prefix/lib -Wl,--gc-sections$qemuExtraLdFlags -lucontext"
mkdir -p "$buildDir" "$srcDir" "$outDir" "$prefix"
fetchSources
buildSysroot
buildQemu
packageQemu