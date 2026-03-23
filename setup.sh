#!/bin/bash
set -e

sudo apt-get update
sudo apt-get install -y \
    build-essential \
    meson \
    ninja-build \
    pkg-config \
    python3 \
    python3-mako \
    python3-pip \
    python3-setuptools \
    python3-packaging \
    python3-yaml \
    flex \
    bison \
    libdrm-dev \
    libx11-dev \
    libxext-dev \
    libxdamage-dev \
    libxfixes-dev \
    libx11-xcb-dev \
    libxcb-glx0-dev \
    libxcb-dri2-0-dev \
    libxcb-dri3-dev \
    libxcb-present-dev \
    libxcb-shm0-dev \
    libxshmfence-dev \
    libxxf86vm-dev \
    libxrandr-dev \
    libwayland-dev \
    wayland-protocols \
    libvulkan-dev \
    glslang-tools \
    zlib1g-dev \
    libexpat1-dev
