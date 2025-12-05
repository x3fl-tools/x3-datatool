# x3fuse

A cli tool to mount game vfs into specific folder, using all game rules.
Written using AI mostly

```
Usage: ./build/x3fuse <data-dir> <mountpoint> [fuse options]
Example: ./build/x3fuse ./x3fl /mnt/x3 -f
```


# x3_datatool

A command-line, Linux-focused tool for managing X3 data files. Works with paired `.cat` (catalog/index) and `.dat` (data) files that use XOR encryption.

## Overview

x3_datatool provides a complete suite of operations for X3 archive management:

- **List** archive contents with file sizes
- **Decrypt** catalog files to readable text format
- **Extract** individual files or entire archives
- **Build** new archive pairs from directory trees
- **Search** for files across multiple archives with precedence handling
- **Extract** from multi-archive directories (following X3's rules for file precedence)

The tool understands X3's archive precedence system where higher-numbered archives (e.g., `10.cat`) override files in lower-numbered archives (e.g., `1.cat`, `2.cat`). This makes it ideal for managing game mods and patches.

## Usage

```
x3tool <operation> [cat_file] [options]
```

### Operations

#### Single Archive Operations

**`t` / `dump-index`** - Print the index of an archive
```
x3tool dump-index <cat_file>
```

**`f` / `extract-file`** - Extract a single file from archive
```
x3tool extract-file <cat_file> -f <filename> [-o output-file]
```

**`x` / `extract-archive`** - Extract entire archive
```
x3tool extract-archive <cat_file> [-o output-path]
```

**`p` / `build-package`** - Create new archive from directory
```
x3tool build-package <cat_file> -i <input-path>
```

#### Multi-Archive Operations

**`s` / `search`** - Find which archive contains the "final version" of a file
```
x3tool search -i <search-directory> -f <filename>
```

**`a` / `extract-all`** - Extract the archives to a local directory using the same precedence rules that X3 does
```
x3tool extract-all -i <input-path> -o <output-path>
```

### Options

- `-o <path>` / `--output <path>` - Output file or directory path
- `-i <path>` / `--input <path>` - Input file or directory path
- `-f <name>` / `--file <name>` - File to search for or extract

## Examples

### List contents of an archive
```bash
x3tool dump-index 01.cat
```
Output:
```
01.cat
	scripts/init.lua                                                       1024
	models/ship.mdl                                                        2048
	textures/hull.tex                                                       512
```

### Extract a single file
```bash
x3tool extract-file 01.cat -f "scripts/init.lua" -o extracted_script.lua
```

### Extract entire archive
```bash
x3tool extract-archive 01.cat -o ./extracted_files
```
This creates `./extracted_files/scripts/init.lua`, `./extracted_files/models/ship.mdl`, etc.

### Build a new archive from a directory
```bash
x3tool build-package newmod.cat -i ./my_mod_files
```
Creates `newmod.cat` and `newmod.dat` containing all files from `./my_mod_files/`.

### Search for a file across multiple archives
```bash
x3tool search -i ~/games/x3/data -f "models/ship.mdl"
```
Output:
```
The file "models/ship.mdl" is most recently found in ~/games/x3/data/10.cat
```

### Extract game directory with proper precedence
```bash
x3tool extract-all -i ~/games/x3/data -o ./extracted_game
```

This extracts all archives in the directory, respecting precedence rules. If `ship.mdl` exists in both `01.cat` and `10.cat`, the version from `10.cat` (highest precedence) will be extracted.

### Decode a catalog file for inspection
```bash
x3tool decode-file 01.cat -o catalog_contents.txt
cat catalog_contents.txt
```
Output:
```
01.dat
scripts/init.lua 1024
models/ship.mdl 2048
textures/hull.tex 512
```

## Building

Requires g++ with C++20 support.

```bash
make              # Build release binary to build/x3tool
make debug        # Build debug binary with symbols
make test         # Build and compile unit tests
./build/xttest    # Run unit tests
make clean        # Remove build artifacts
```

## Archive Format

### CAT File (Catalog/Index)
- Encrypted with rolling XOR starting at 0xdb
- Text format after decryption:
  ```
  <datfilename>
  <filepath> <size>
  <filepath> <size>
  ...
  ```

### DAT File (Data)
- Contains concatenated file contents in order listed in CAT
- Encrypted with static XOR using byte 0x33
- Files are stored sequentially with no padding

### Archive Precedence
In game installations, archives are numbered (e.g., `1.cat`, `2.cat`, `10.cat`). Higher numbers take precedence:
- File in `10.cat` overrides same file in `2.cat` or `1.cat`
- File in `2.cat` overrides same file in `1.cat`
- Files unique to any archive are included

The `extract-all` command automatically handles this precedence.

# Note

I used AI to write this file. I did review and edit it myself. 
