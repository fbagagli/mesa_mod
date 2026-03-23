#!/bin/bash
set -e

# Default variables
export MESA_DIR="${MESA_DIR:-$(pwd)/mesa}"
export MESA_BUILD_DIR_LINUX="${MESA_BUILD_DIR_LINUX:-$(pwd)/build}"
export MESA_INSTALL_DIR="${MESA_INSTALL_DIR:-$(pwd)/install}"

echo "Building Mesa in $MESA_DIR"
echo "Build directory: $MESA_BUILD_DIR_LINUX"
echo "Install directory: $MESA_INSTALL_DIR"

pushd "${MESA_DIR}"

rm -rf "${MESA_BUILD_DIR_LINUX}"
mkdir -p "${MESA_BUILD_DIR_LINUX}"

meson setup "${MESA_BUILD_DIR_LINUX}" \
    --prefix="${MESA_INSTALL_DIR}" \
    -Dbuildtype=debug \
    -Doptimization=0 \
    -Dgallium-drivers=virgl \
    -Dvulkan-drivers=[] \
    -Dplatforms=x11 \
    -Dglx=dri \
    -Degl=enabled \
    -Dgbm=enabled \
    -Dgles1=disabled \
    -Dgles2=enabled \
    -Dllvm=disabled \
    -Dglvnd=false \
    -Dstrip=false

ninja -C "${MESA_BUILD_DIR_LINUX}"
ninja -C "${MESA_BUILD_DIR_LINUX}" install

popd
echo "Build and install successful!"
