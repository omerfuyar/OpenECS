#!/bin/sh
# Installs what the Linux build needs: gcc 14 as gcc, which shuild calls; cmake and ninja for SDL; and the system libraries SDL uses.
set -eu

sudo apt-get update
sudo apt-get install -y gcc-14 cmake ninja-build pkg-config \
    libasound2-dev libpulse-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev \
    libxkbcommon-dev libdrm-dev libgbm-dev libgl1-mesa-dev libgles2-mesa-dev libegl1-mesa-dev libdbus-1-dev libibus-1.0-dev \
    libudev-dev libwayland-dev libdecor-0-dev
sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-14 100
