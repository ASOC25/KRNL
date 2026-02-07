#!/bin/bash
# Install in ubuntu 24.04 the dependencies
sudo apt-get update
sudo apt install -y wget gunzip
sudo wget -qO /usr/local/bin/ninja.gz https://github.com/ninja-build/ninja/releases/latest/download/ninja-linux.zip
sudo gunzip /usr/local/bin/ninja.gz
sudo chmod +x /usr/local/bin/ninja
sudo apt-get install -y gdb make gcc nasm parted dosfstools qemu-system
sudo apt-get install -y rsync qemu-utils meson python3 python3-pip python3-setuptools python3-wheel ninja-build cmake sed m4 texinfo libgmp-dev bison flex curl
sudo pip3 install --break-system-packages --upgrade pip 
sudo pip3 install --break-system-packages pillow
sudo pip3 install --break-system-packages meson
sudo pip3 install --break-system-packages xbstrap