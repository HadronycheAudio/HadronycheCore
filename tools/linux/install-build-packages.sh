#!/bin/bash
# Installs what building a plug-in needs on Debian or Ubuntu, as listed in VSTGUI's own CMakeLists.txt.
#
# gtkmm and SQLite are only needed to configure. The SDK always adds VSTGUI's standalone library, which looks for them,
# but nothing here builds that library, so the plug-ins don't link them. The tests need xvfb where there's no display.
set -e

sudo apt-get update
sudo apt-get install -y \
    ninja-build pkg-config \
    libx11-dev libxcb1-dev libxcb-util-dev libxcb-cursor-dev libxcb-keysyms1-dev libxcb-xkb-dev \
    libxkbcommon-dev libxkbcommon-x11-dev libwayland-dev wayland-protocols \
    libcairo2-dev libpango1.0-dev libfreetype-dev libfontconfig1-dev libglib2.0-dev libexpat1-dev \
    libgtkmm-3.0-dev libsqlite3-dev \
    xvfb
