#!/bin/bash
set -e

# Define paths and versions
ROOT_DIR="$(pwd)"
MESA_DIR="${ROOT_DIR}/mesa"
NDK_VERSION="r27d"
NDK_DIR="${ROOT_DIR}/android-ndk-${NDK_VERSION}"
CROSS_FILE="${ROOT_DIR}/android-aarch64.ini"
API_LEVEL="31"

# 1. Prepare Ubuntu 24.04 Environment
sudo apt-get update
sudo apt-get install -y wget unzip python3-pip ninja-build pkg-config \
    python3-mako flex bison libelf-dev

# 2. Download and Extract Android NDK r27d
if [ ! -d "${NDK_DIR}" ]; then
    echo "Downloading Android NDK ${NDK_VERSION}..."
    wget "https://dl.google.com/android/repository/android-ndk-${NDK_VERSION}-linux.zip" -O ndk.zip
    echo "Extracting NDK..."
    unzip -q ndk.zip
    rm ndk.zip
fi

# 3. Generate Meson Cross-File for AArch64
# The NDK provides pre-built compiler wrappers targeting specific API levels.
TOOLCHAIN_BIN="${NDK_DIR}/toolchains/llvm/prebuilt/linux-x86_64/bin"

cat <<EOF > "${CROSS_FILE}"
[binaries]
c = '${TOOLCHAIN_BIN}/aarch64-linux-android${API_LEVEL}-clang'
cpp = '${TOOLCHAIN_BIN}/aarch64-linux-android${API_LEVEL}-clang++'
ar = '${TOOLCHAIN_BIN}/llvm-ar'
strip = '${TOOLCHAIN_BIN}/llvm-strip'
# Point pkg-config to an empty directory to prevent host library leakage
pkg-config = ['env', 'PKG_CONFIG_LIBDIR=', '/usr/bin/pkg-config']

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8-a'
endian = 'little'
EOF

# 4. Configure and Build Mesa
cd "${MESA_DIR}"

# Remove existing build directory to ensure a clean state
rm -rf build

# Execute Meson setup utilizing the generated cross-file
meson setup build \
    --cross-file "${CROSS_FILE}" \
    --force-fallback-for=libdrm \
    -Dplatforms=android \
    -Dgallium-drivers=virgl \
    -Dvulkan-drivers=[] \
    -Degl=enabled \
    -Dgles1=disabled \
    -Dgles2=enabled \
    -Dglx=disabled \
    -Dllvm=disabled \
    -Dplatform-sdk-version=${API_LEVEL} \
    -Dandroid-stub=true \
    -Dandroid-libbacktrace=disabled \
    -Dcpp_rtti=false \
    -Dgbm=enabled \
    -Dshared-glapi=enabled \
    -Dlibdrm:intel=disabled \
    -Dlibdrm:radeon=disabled \
    -Dlibdrm:amdgpu=disabled \
    -Dlibdrm:nouveau=disabled \
    -Dlibdrm:vmwgfx=disabled \
    -Dlibdrm:freedreno=disabled \
    -Dlibdrm:vc4=disabled \
    -Dlibdrm:etnaviv=disabled

# Compile
meson compile -C build
