#!/bin/sh
# Compile the host FSELF wrapper and the clean-room libc builder.
# Needs g++ and a static zlib 1.3.2 (downloaded on first use).
# Build logs go to stderr. The tool path is the only stdout line.
set -eu
root=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
host="$root/build/host"
tool="$host/ps5-native-tool"
builder="$host/libc-builder"
if [ -x "$tool" ] && [ -x "$builder" ]; then
    printf '%s\n' "$tool"
    exit 0
fi
native="$root/third_party/ps5-native"
zlib="$host/zlib-root"
if [ ! -f "$zlib/include/zlib.h" ]; then
    archive="$host/zlib-1.3.2.tar.gz"
    mkdir -p "$host"
    if [ ! -f "$archive" ]; then
        if command -v curl >/dev/null 2>&1; then
            curl -fsSL -o "$archive" \
                https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz
        elif command -v wget >/dev/null 2>&1; then
            wget -q -O "$archive" \
                https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz
        else
            echo "need curl or wget to download zlib" >&2
            exit 1
        fi
    fi
    echo "bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16  $archive" | sha256sum -c >&2
    rm -rf "$host/zlib-1.3.2" "$zlib"
    tar --no-same-owner -xzf "$archive" -C "$host"
    (
        cd "$host/zlib-1.3.2"
        CC=gcc ./configure --static --prefix="$zlib"
        make -j"$(nproc)"
        make install
    ) >&2
fi
mkdir -p "$host/obj"
for name in elf_object self_container sce_module_writer native_app_builder libc_builder; do
    g++ -std=c++20 -O2 -I"$zlib/include" -I"$native" \
        -c "$native/$name.cpp" -o "$host/obj/$name.o"
done
g++ -o "$tool" \
    "$host/obj/elf_object.o" \
    "$host/obj/self_container.o" \
    "$host/obj/sce_module_writer.o" \
    "$host/obj/native_app_builder.o" \
    "$zlib/lib/libz.a"
g++ -o "$builder" "$host/obj/libc_builder.o"
printf '%s\n' "$tool"
