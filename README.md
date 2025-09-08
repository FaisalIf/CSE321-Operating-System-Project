# MiniVSFS — A Minimal Virtual File System

This project implements a **simple custom filesystem** called **MiniVSFS**.  
It consists of two tools:

- **mkfs_builder** → creates a new empty filesystem image.
- **mkfs_adder** → adds files into an existing filesystem image.

The filesystem image (`.img`) is stored as a normal file on your host OS, but internally it mimics a real filesystem with inodes, directories, bitmaps, and data blocks.

---

## Features

- Block size: **4096 bytes**.
- Inode size: **128 bytes**.
- Root inode: **1** (always the root directory `/`).
- Max direct pointers per inode: **12** → max file size = 48 KB.
- Directory entries: 64 bytes each.
- Checksums for:
  - Superblock (CRC32).
  - Inodes (CRC32 of first 120 bytes).
  - Directory entries (XOR of first 63 bytes).

## Filesystem Layout

The `.img` file is divided into blocks:

| Block | Content                                      |
| ----- | -------------------------------------------- |
| 0     | Superblock (metadata)                        |
| 1     | Inode bitmap                                 |
| 2     | Data bitmap                                  |
| 3..N  | Inode table (array of inodes, 128B each)     |
| ...   | Data region (file contents + directory data) |

### ASCII Diagram of Layout

```
+------------------+ Block 0
| Superblock | (filesystem metadata, checksums, layout info)
+------------------+ Block 1
| Inode Bitmap | (tracks free/used inodes)
+------------------+ Block 2
| Data Bitmap | (tracks free/used data blocks)
+------------------+ Block 3..N
| Inode Table | (array of fixed-size 128B inodes)
| [inode #1 = /] |
| [inode #2 = f1] |
| [inode #3 = f2] |
+------------------+ Remaining blocks
| Data Region | (actual file and directory contents)
| |
+------------------+

Example root directory after creation:
. → inode 1
.. → inode 1

After adding files:
. → inode 1
.. → inode 1
file_14.txt → inode 2
file_23.txt → inode 3
file_27.txt → inode 4
```

## Build

Compile both tools with GCC:

```bash
gcc -O2 -std=c17 -Wall -Wextra mkfs_builder.c -o mkfs_builder
gcc -O2 -std=c17 -Wall -Wextra mkfs_adder.c  -o mkfs_adder
```

## Usage

1. Create a new filesystem image

```bash
./mkfs_builder --image fs.img --size-kib 512 --inodes 128
```

- Creates `fs.img` of size 512 KiB with space for 128 inodes.
- Root directory (`/`) is created with `.` and `..`.

2. Add a file to the filesystem

```bash
./mkfs_adder --input fs.img --output fs2.img --file hello.txt
```

- Loads `fs.img`.
- Allocates a free inode + data blocks.
- Copies `hello.txt` into the FS.
- Adds a directory entry under `/`.
- Saves the result to `fs2.img`.

3. Add more files

```bash
./mkfs_adder --input fs2.img --output fs3.img --file file_14.txt
./mkfs_adder --input fs3.img --output fs4.img --file file_23.txt
./mkfs_adder --input fs4.img --output fs5.img --file file_27.txt
```

## Inspecting the Image

Since this is a **custom FS**, you cannot mount it.
But you can inspect it with `hexdump` or `xxd`:

```bash
hexdump -C fs2.img | less
```

Look for your filenames:

```bash
00003040  02 00 00 00 01 66 69 6c 65 5f 31 34 2e 74 78 74 ...
                ^ inode=2   ^ type=file   ^ "file_14.txt"

```

And file contents:

```bash
00005000  48 65 6c 6c 6f 20 4d 69 6e 69 56 53 46 53 ...
           H  e  l  l  o     M  i  n  i  V  S  F  S

```

## Limits

- Max file size: 48 KB (12 direct blocks × 4 KB)
- Max filename length: 58 characters (truncated if longer)
- Root directory can hold up to 64 entries (4096 ÷ 64 B)
- Empty files still consume one block

## Example Session

```bash
# Build a new image
./mkfs_builder --image fs.img --size-kib 512 --inodes 128

# Add a file
echo "Hello MiniVSFS" > hello.txt
./mkfs_adder --input fs.img --output fs2.img --file hello.txt

# Add another
echo "Another file" > file2.txt
./mkfs_adder --input fs2.img --output fs3.img --file file2.txt

# Inspect the image
hexdump -C fs3.img | grep hello.txt
hexdump -C fs3.img | grep file2.txt
```
