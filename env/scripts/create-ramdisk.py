#!/usr/bin/env python3

import os
import struct
import sys
import hashlib

# X1FS ramdisk layout:
#   [4]  signature (0x58314653 = 'X1FS')
#   [4]  num_entries
#   [8]  offset_to_string_table
#   [8]  offset_to_file_data
#   [num_entries * 64]  entry table
#       [8]  string_table_index
#       [16] md5 hash
#       [8]  file_offset
#       [8]  file_size
#       [24] padding
#   [...]  string table (null-terminated paths, padded to 512-byte boundary)
#   [...]  file data

SIGNATURE  = 0x58314653
ENTRY_SIZE = 64
HEADER_SIZE = 4 + 4 + 8 + 8  # sig + count + strtab_off + data_off


def create_ramdisk_image(folder_path, output_image_path):
    # Pass 1: collect metadata without reading file content
    meta = []  # (virtual_path, full_path, file_size)
    for root, dirs, files in os.walk(folder_path):
        dirs.sort()
        for name in sorted(files):
            full_path = os.path.join(root, name)
            if not os.path.isfile(full_path):
                continue
            vpath = os.path.relpath(full_path, folder_path).replace("\\", "/")
            meta.append((vpath, full_path, os.path.getsize(full_path)))

    # Build string table
    strtab = bytearray()
    strtab_indices = []
    for vpath, _, _ in meta:
        strtab_indices.append(len(strtab))
        strtab.extend(vpath.encode("utf-8") + b"\x00")

    num_entries = len(meta)
    strtab_aligned_size = (len(strtab) + 511) & ~511
    offset_to_strtab = HEADER_SIZE + num_entries * ENTRY_SIZE
    offset_to_data   = offset_to_strtab + strtab_aligned_size

    # Pass 2: read each file once, compute md5, track offsets
    records = []  # (strtab_idx, md5, data_offset, size, content)
    cur_offset = 0
    for i, (vpath, full_path, size) in enumerate(meta):
        with open(full_path, "rb") as f:
            content = f.read()
        records.append((
            strtab_indices[i],
            hashlib.md5(content).digest(),
            cur_offset,
            len(content),
            content,
        ))
        cur_offset += len(content)

    # Write output in one pass
    with open(output_image_path, "wb") as img:
        # Header
        img.write(struct.pack("<II", SIGNATURE, num_entries))
        img.write(struct.pack("<QQ", offset_to_strtab, offset_to_data))

        # Entry table
        for stridx, md5, data_off, size, _ in records:
            img.write(struct.pack("<Q", stridx))
            img.write(md5)
            img.write(struct.pack("<QQ", data_off, size))
            img.write(b"\x00" * 24)

        # String table (padded)
        img.write(bytes(strtab))
        img.write(b"\x00" * (strtab_aligned_size - len(strtab)))

        # File data (written one file at a time — no giant concatenation)
        for _, _, _, _, content in records:
            img.write(content)

    print(f"Ramdisk: {num_entries} files, "
          f"{cur_offset // 1024} KB data, "
          f"image at {output_image_path}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: create-ramdisk.py <folder_path> <output_image_path>")
        sys.exit(1)
    create_ramdisk_image(sys.argv[1], sys.argv[2])
