#!/bin/sh
# Builds the portable Linux test package (dist/miipc-linux-x86_64.tar.gz).
# zig cc targets glibc 2.17 and SDL2 is linked statically, so the binary only
# needs libc; SDL2 loads X11 / PulseAudio / ALSA at runtime. Tested target:
# Linux Mint 19+ / LMDE, Ubuntu 18.04+, Debian 10+, Fedora, Arch.
#
# Needs: python3 (for the ziglang wheel), cmake, ninja, git, and the X11,
# PulseAudio and ALSA development headers (libx11-dev libxext-dev
# libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxfixes-dev
# libpulse-dev libasound2-dev).
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
work=${WORK:-$HOME/.cache/miipc-build}
mkdir -p "$work/bin" "$work/inc"

# 1. zig toolchain (pip wheel, pinned)
[ -x "$work/venv/bin/python" ] || python3 -m venv "$work/venv"
"$work/venv/bin/pip" install -q ziglang==0.13.0
for t in cc ar ranlib; do
  extra=""; [ $t = cc ] && extra="-target x86_64-linux-gnu.2.17"
  printf '#!/bin/sh\nexec "%s" -m ziglang %s %s "$@"\n' "$work/venv/bin/python" $t "$extra" > "$work/bin/z$t"
  chmod +x "$work/bin/z$t"
done

# 2. static SDL2 (backends loaded at runtime)
if [ ! -f "$work/sdl2/lib/libSDL2.a" ]; then
  [ -d "$work/SDL" ] || git clone -q --depth 1 -b release-2.30.9 https://github.com/libsdl-org/SDL "$work/SDL"
  cp -r /usr/include/X11 /usr/include/pulse /usr/include/alsa "$work/inc/"
  rm -rf "$work/sdlbuild" && mkdir "$work/sdlbuild" && cd "$work/sdlbuild"
  cmake -G Ninja "$work/SDL" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$work/sdl2" \
    -DCMAKE_C_COMPILER="$work/bin/zcc" -DCMAKE_AR="$work/bin/zar" -DCMAKE_RANLIB="$work/bin/zranlib" \
    -DCMAKE_C_FLAGS="-isystem $work/inc" \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_STATIC_PIC=ON -DSDL_TEST=OFF \
    -DSDL_X11=ON -DSDL_X11_SHARED=ON -DSDL_WAYLAND=OFF \
    -DSDL_PULSEAUDIO=ON -DSDL_PULSEAUDIO_SHARED=ON -DSDL_ALSA=ON -DSDL_ALSA_SHARED=ON \
    -DSDL_PIPEWIRE=OFF -DSDL_JACK=OFF -DSDL_SNDIO=OFF -DSDL_ESD=OFF -DSDL_ARTS=OFF -DSDL_NAS=OFF \
    -DSDL_LIBSAMPLERATE=OFF -DSDL_KMSDRM=OFF -DSDL_DBUS=OFF -DSDL_IBUS=OFF -DSDL_LIBUDEV=OFF \
    -DSDL_VULKAN=OFF -DSDL_OPENGLES=OFF -DSDL_RPATH=OFF >/dev/null
  ninja >/dev/null && ninja install >/dev/null
fi

# 3. the app
cd "$here"
make linux-pkg ZIGCC="$work/bin/zcc" SDL2_PREFIX="$work/sdl2"
echo "-> dist/miipc-linux-x86_64.tar.gz"
