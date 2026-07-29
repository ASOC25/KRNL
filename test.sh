#!/bin/sh

#Flags:
#-r run only
#-b build only
#-c clean only
#-s setup

#Check if the flag is clean
if [ "$1" = "--clean" ]; then
    echo "Cleaning programs..."
    #Iterate packages in env/bootstrap/sys-apps.yml and execute make clean in their source dirs
    grep '^\s*- name:' env/bootstrap/sys-apps.yml | awk '{print $3}' | while IFS= read -r name; do
        if [ -d "alt/sources/$name" ]; then
            echo "Cleaning alt/sources/$name..."
            make -C "alt/sources/$name" clean
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
    echo "Build libraries..."
    #Rebuild mlibc (force rebuild since sysdep sources may have changed)
    xbstrap build --reset mlibc
    if [ -d "packages/mlibc" ]; then
        echo "Installing packages/mlibc to sysroot..."
        cp -r "packages/mlibc/." sysroot/
    fi

    echo "Build programs..."
    #Extract the name of each package in env/bootstrap/sys-apps.yml and execute xbstrap build (name)
    grep '^\s*- name:' env/bootstrap/sys-apps.yml | awk '{print $3}' | while IFS= read -r name; do
        echo "Building $name..."
        xbstrap build "$name"
        #Copy every .elf file produced in alt/sources/$name/build/ to sysroot/
        for elf in "alt/sources/$name/build/"*.elf; do
            if [ -f "$elf" ]; then
                echo "Copying $elf to sysroot/$(basename "$elf")..."
                cp "$elf" "sysroot/$(basename "$elf")"
            fi
        done
        #Merge package tree (FHS layout) into sysroot/
        if [ -d "packages/$name" ]; then
            echo "Installing packages/$name to sysroot..."
            cp -r "packages/$name/." sysroot/
        fi
    done
    echo "Copying symbols for each binary in sysroot..."
    #Generate symbols for all ELF files in sysroot (binaries, .elf, .so)
    find sysroot/ -type f \( -name "*.elf" -o -name "*.so*" -o -perm /111 \) \
        -not -name "*.sym" | while read -r file; do
        objcopy --only-keep-debug "$file" "$file.sym" 2>/dev/null || true
    done
    
    #If build does not exist, create it
    if [ ! -d "build" ]; then
        mkdir build
    fi
    # Choose filesystem: FS=ext2 or FS=x1fs (default)
    FS="${FS:-x1fs}"
    if [ "$FS" = "ext2" ]; then
        echo "Bundling ext2 filesystem onto a real AHCI-attached drive image..."
        sh ./env/scripts/create-ext2.sh ./sysroot ./build/ahci-disk.img
        # No embedded ramdisk needed for this boot mode; keep the Makefile's
        # mandatory RAMDISK variable satisfied with a 1-byte placeholder
        # (objcopy -I binary rejects a truly empty input file).
        printf '\0' > ./build/ramdisk.img
    else
        echo "Bundling x1fs ramdisk..."
        python3 ./env/scripts/create-ramdisk.py ./sysroot ./build/ramdisk.img
        # Remove any stale AHCI disk image from a previous FS=ext2 build so it
        # doesn't get attached to QEMU by accident.
        rm -f ./build/ahci-disk.img
    fi
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
    ./env/scripts/make-efi-img.sh --force
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
    echo "Rebuilding mlibc..."
    xbstrap build --reset mlibc || exit 1
    if [ -d "packages/mlibc" ]; then
        cp -r "packages/mlibc/." sysroot/
    fi
    echo "Generating mlibc symbols..."
    find sysroot/ -type f \( -name "*.so*" \) -not -name "*.sym" | while read -r file; do
        objcopy --only-keep-debug "$file" "$file.sym" 2>/dev/null || true
    done
    echo "Rebuilding kernel..."
    xbstrap build kernel || exit 1
    cp kernel/build/kernel.elf build/kernel.elf
    echo "Rebuilding ramdisk..."
    FS="${FS:-x1fs}"
    if [ "$FS" = "ext2" ]; then
        sh ./env/scripts/create-ext2.sh ./sysroot ./build/ahci-disk.img
        printf '\0' > ./build/ramdisk.img
    else
        python3 ./env/scripts/create-ramdisk.py ./sysroot ./build/ramdisk.img
        rm -f ./build/ahci-disk.img
    fi
    ./env/scripts/make-efi-img.sh --force
    echo "Running QEMU with debug options..."
    ./env/scripts/debug-qemu.sh
fi
if [ "$1" = "--run" ]; then
    echo "Running QEMU..."
    ./env/scripts/run-qemu.sh
fi
