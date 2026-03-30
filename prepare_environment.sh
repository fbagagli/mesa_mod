#!/bin/bash
set -e

# 1. Prepare Ubuntu 24.04 Environment
sudo apt-get update
sudo apt-get install -y wget unzip python3-pip ninja-build pkg-config \
    python3-mako flex bison libelf-dev
