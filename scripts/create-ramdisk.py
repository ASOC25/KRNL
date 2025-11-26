#!/usr/bin/env python3

import os
import struct
import sys
import hashlib
# Note: The virtual path is the full path assuming the ramdisk is mounted at /
# - uint32_t SIGNATURE (X1FS) - 0x58314653
# - uint32_t NUM_ENTRIES
# - uint64_t OFFSET_TO_STRING_TABLE (calculated as 4 + 4 + 8 + 8 + NUM_ENTRIES * 64)
# - uint64_t OFFSET_TO_FILE_DATA (calculated as OFFSET_TO_STRING_TABLE + size of string table aligned to 512 bytes)
# - FsEntry[NUM_ENTRIES] (each entry is 64 bytes: [8 bytes] string table index, [16 bytes] md5 hash, [8 bytes] file address, [8 bytes] file size, [24 bytes] padding)
# - String Table (null-terminated strings for virtual file paths)
# - File Data (actual file contents)
def create_ramdisk_image(folder_path, output_image_path):
    SIGNATURE = 0x58314653  # 'X1FS'
    ENTRY_SIZE = 64
    HEADER_FIXED_SIZE = 4 + 4 + 8 + 8  # signature + num_entries + offset_to_string_table + offset_to_file_data
    entries = []
    string_table = b""
    file_data = b""

    for root, _, files in os.walk(folder_path):
        for file in files:
            full_path = os.path.join(root, file)
            virtual_path = os.path.relpath(full_path, folder_path).replace("\\", "/")
            with open(full_path, "rb") as f:
                content = f.read()
                file_size = len(content)
                file_address = len(file_data)
                file_data += content

                md5_hash = hashlib.md5(content).digest()

                string_index = len(string_table)
                string_table += virtual_path.encode('utf-8') + b'\x00'

                entries.append((string_index, md5_hash, file_address, file_size))

    num_entries = len(entries)
    offset_to_string_table = HEADER_FIXED_SIZE + num_entries * ENTRY_SIZE
    string_table_size_aligned = (len(string_table) + 511) & ~511
    offset_to_file_data = offset_to_string_table + string_table_size_aligned

    with open(output_image_path, "wb") as img:
        img.write(struct.pack("<I", SIGNATURE))
        img.write(struct.pack("<I", num_entries))
        img.write(struct.pack("<Q", offset_to_string_table))
        img.write(struct.pack("<Q", offset_to_file_data))
        for entry in entries:
            string_index, md5_hash, file_address, file_size = entry
            img.write(struct.pack("<Q", string_index))
            img.write(md5_hash)
            img.write(struct.pack("<Q", file_address))
            img.write(struct.pack("<Q", file_size))
            img.write(b'\x00' * 24)  # padding to ENTRY_SIZE (64 bytes)
        img.write(string_table)
        img.write(b'\x00' * (string_table_size_aligned - len(string_table)))  # align to 512 bytes
        img.write(file_data)
        
def ramdisk_print_header(image_path):
    with open(image_path, "rb") as img:
        signature = struct.unpack("<I", img.read(4))[0]
        num_entries = struct.unpack("<I", img.read(4))[0]
        offset_to_string_table = struct.unpack("<Q", img.read(8))[0]
        offset_to_file_data = struct.unpack("<Q", img.read(8))[0]

        print(f"Ramdisk Header:")
        print(f" Signature: {hex(signature)}")
        print(f" Number of Entries: {num_entries}")
        print(f" Offset to String Table: {offset_to_string_table}")
        print(f" Offset to File Data: {offset_to_file_data}")

        #Dump all entries, including their virtual paths and first 16 bytes of contents
        entries = []
        for _ in range(num_entries):
            string_index = struct.unpack("<Q", img.read(8))[0]
            md5_hash = img.read(16)
            file_address = struct.unpack("<Q", img.read(8))[0]
            file_size = struct.unpack("<Q", img.read(8))[0]
            img.read(24)  # padding
            entries.append((string_index, md5_hash, file_address, file_size))
        img.seek(offset_to_string_table)
        string_table_data = img.read(offset_to_file_data - offset_to_string_table)
        string_table = {}
        current_index = 0
        while current_index < len(string_table_data):
            end_index = string_table_data.find(b'\x00', current_index)
            if end_index == -1:
                break
            path = string_table_data[current_index:end_index].decode('utf-8')
            string_table[current_index] = path
            current_index = end_index + 1
        print(f"\nRamdisk Entries:")
        for entry in entries:
            string_index, md5_hash, file_address, file_size = entry
            path = string_table.get(string_index, None)
            img.seek(offset_to_file_data + file_address)
            content_preview = img.read(min(16, file_size))
            print(f" Path: {path}")
            print(f"  MD5: {md5_hash.hex()}")
            print(f"  Size: {file_size} bytes")
            print(f"  Content Preview: {content_preview.hex()}\n")
            

def ramdisk_print_file(image_path, virtual_path):
    with open(image_path, "rb") as img:
        signature = struct.unpack("<I", img.read(4))[0]
        num_entries = struct.unpack("<I", img.read(4))[0]
        offset_to_string_table = struct.unpack("<Q", img.read(8))[0]
        offset_to_file_data = struct.unpack("<Q", img.read(8))[0]

        entries = []
        for _ in range(num_entries):
            string_index = struct.unpack("<Q", img.read(8))[0]
            md5_hash = img.read(16)
            file_address = struct.unpack("<Q", img.read(8))[0]
            file_size = struct.unpack("<Q", img.read(8))[0]
            img.read(24)  # padding
            entries.append((string_index, md5_hash, file_address, file_size))

        img.seek(offset_to_string_table)
        string_table_data = img.read(offset_to_file_data - offset_to_string_table)
        string_table = {}
        current_index = 0
        while current_index < len(string_table_data):
            end_index = string_table_data.find(b'\x00', current_index)
            if end_index == -1:
                break
            path = string_table_data[current_index:end_index].decode('utf-8')
            string_table[current_index] = path
            current_index = end_index + 1

        for entry in entries:
            string_index, md5_hash, file_address, file_size = entry
            path = string_table.get(string_index, None)
            if path == virtual_path:
                img.seek(offset_to_file_data + file_address)
                content = img.read(file_size)
                print(f"Contents of {virtual_path}:\n")
                print(content.decode('utf-8', errors='replace'))
                return

        print(f"File {virtual_path} not found in ramdisk.")

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: python create-ramdisk.py <folder_path> <output_image_path>")
        sys.exit(1)

    folder_path = sys.argv[1]
    output_image_path = sys.argv[2]

    create_ramdisk_image(folder_path, output_image_path)
    print(f"Ramdisk image created at {output_image_path}")

    ramdisk_print_header(output_image_path)

    #file_to_print = input("Enter the virtual path of the file to print (or 'exit' to quit): ")
    #while file_to_print.lower() != 'exit':
    #    ramdisk_print_file(output_image_path, file_to_print)
    #    file_to_print = input("Enter the virtual path of the file to print (or 'exit' to quit): ")
