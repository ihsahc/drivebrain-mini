#!/usr/bin/env sh
set -e

conan profile detect --force
conan install . --build=missing -of=build-local
cmake -S . -B build-local -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=conan_toolchain.cmake
cmake --build build-local -j"$(nproc)"
exec ./build-local/mini-drivebrain
