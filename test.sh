#!/bin/sh

#Flags:
#-r run only
#-b build only
#-c clean only
#-s setup

#Check if the flag is clean
if [ "$1" = "--clean" ]; then
    echo "Cleaning programs..."
    #Iterate alt/sources and execute make clean in each directory
    for dir in alt/sources/*; do
        if [ -d "$dir" ]; then
            echo "Cleaning $dir..."
            make -C "$dir" clean
        fi
    done
    #Execute make clean in kernel directory
    echo "Cleaning kernel..."
    make -C kernel clean
    #Remove build folder
    rm -rf build
    exit 0
fi
if [ "$1" = "--build" ]; then
    echo "Build programs..."
    #Extract the name of each directory in alt/sources and execute xbstrap build (name) 
    for dir in alt/sources/*; do
        if [ -d "$dir" ]; then
            name=$(basename "$dir")
            echo "Building $name..."
            xbstrap build "$name"
            #For each program, copy the resulting binary from $dir/build/(name) to sysroot/(name).elf
            #Copy every .elf file in $dir/build/ to sysroot/
            for elf in "$dir/build/"*.elf; do
                if [ -f "$elf" ]; then
                    echo "Copying $elf to sysroot/$(basename "$elf")..."
                    cp "$elf" "sysroot/$(basename "$elf")"
                    echo "Extracting symbols from $elf to sysroot/$(basename "$elf" .elf).sym..."
                    #Extract symbols from the .elf file and save them to sysroot/(name).sym
                    objcopy --only-keep-debug "$elf" "sysroot/$(basename "$elf" .elf).sym"
                fi
            done
        fi
    done
    echo "Bundling ramdisk..."
    #If build does not exist, create it
    if [ ! -d "build" ]; then
        mkdir build
    fi
    python3 ./env/scripts/create-ramdisk.py ./sysroot ./build/ramdisk.img
    #Make sure build/ramdisk.img exists
    if [ ! -f "build/ramdisk.img" ]; then
        echo "Error: build/ramdisk.img not found after creation." >&2
        exit 1
    fi
    echo "Building kernel..."
    xbstrap build kernel
    #Copy kernel to build/kernel.elf
    cp kernel/build/kernel.elf build/kernel.elf
    echo "Kernel build complete: build/kernel.elf"
    sudo ./env/scripts/make-efi-img.sh --force
    echo "Build complete. Image at build/disk.img"
    exit 0
fi
if [ "$1" = "--clean-x" ]; then
    echo "Cleaning everything created by xbstrap..."
    rm -rf .xbstrap bundled tool-builds tools bootstrap.link packages pkg-builds sysroot
    #Remove every file with the .xbstrap extension recursively
    find . -type f -name "*.xbstrap" -delete
    exit 0
fi
if [ "$1" = "--init-x" ]; then
    echo "Initializing build environment..."
    xbstrap init .
    exit 0
fi
if [ "$1" = "--debug" ]; then
    echo "Running QEMU with debug options..."
    sudo ./env/scripts/debug-qemu.sh
fi
if [ "$1" = "--run" ]; then
    echo "Running QEMU..."
    sudo ./env/scripts/run-qemu.sh
fi
