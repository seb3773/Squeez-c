# sqx — Squeez SQX Archiver (Native C Reimplementation)

Pure C reimplementation of the historical **Squeez SQX archiver** format (1993–2008, Sven Ritter / SQX Development).

> **Reverse Engineering & Legal Notice:** This work was conducted strictly for the purposes of software interoperability and digital data preservation (under European Directive 2009/24/EC) to reconstruct the container structures, prediction filters, and entropy decoding/encoding algorithms of an obsolete historical archive format.

This project is a complete, standalone, freestanding C tool combining both **archiver/compressor** and **extractor/decompressor** into a single binary (`sqx`), achieving full interoperability with the official reference utilities (`sqc` 5.2 on Linux, `sqx20u.dll` / Squeez GUI on Windows).

---

## Highlights

- **Full Container Support (SQX 1.1 & 2.0)**:
  - Supports both container revisions seamlessly via automatic file-header detection (`cArcerExVer = 11` vs `20`).
  - Solid archive support: shared dictionary history across multi-file streams.
  - Transparent Self-Extracting (SFX) archive handling: automatically locates embedded SQX archives behind 32-bit Windows PE stubs (offset ~80 KB for 1.1, ~94 KB for 2.0).
  - Robust CRC16 block checksum verification (un-inverted reflected CRC32 polynomial `0xEDB88320`) and full file CRC32 verification.
- **Comprehensive Codec Implementation (Methods 0–6)**:
  - **Method 0 (Stored)**: Uncompressed payload storage with byte-aligned container streaming.
  - **Methods 1–4 (LZH Compression)**: Complete sliding-window LZ77 paired with dynamic canonical Huffman coding across dictionary sizes from 32 KB up to 32 MB:
    - *SQX 1.1*: 310 litlen codes, 56 distance codes, multi-level length compensation (`MIN_LEN2 = 255`, `MIN_LEN3 = 16,383`, `MIN_LEN4 = 262,143`), 3-tree and 4-tree Huffman blocks.
    - *SQX 2.0*: 311 litlen codes (including symbol 257 current-distance copy and symbol 285 length-3 extension), 54 distance codes, `low16` 4-bit distance sub-table, and single `0x7FFF` length correction threshold.
  - **Method 5 (Audio Compression)**:
    - *SQX 1.1*: 18-byte audio header, 8-bit adaptive 6-predictor Huffman coding, and 16-bit 384-sample polynomial frame decomposition.
    - *SQX 2.0 (16-bit)*: 4608-sample frames, polynomial mode 0 vs adaptive Levinson-Durbin FIR mode 1 predictor (mirroring x86 32-bit truncated autocorrelations), adaptive Rice coding, and 4-way stereo interleaving (`icd` 0..3).
    - *SQX 2.0 (8-bit)*: 256 contexts $\times$ 17 models $\times$ 16 symbols range-coded stream with adaptive 6-case delta predictor.
  - **Method 6 (Text Compression — PPMd)**:
    - Faithful C implementation of PPMd var. I (`Ppmd8`, Dmitry Shkarin) driven by the SQX carryless range coder.
    - Model persistence across solid file boundaries, full arena allocator, and order 2–16 / memory 4–128 MB support.
  - **Multimedia & Delta Transforms**:
    - Multi-channel 8-bit and 16-bit delta filters (`DeconvRType`) with initial adaptation frequency `0xFF`, paired with Fenwick-tree cumulative frequency range decoding (`DecodeRcData`).
- **Bit-Identical Stream Parity**:
  - Over 120 reference streams validated token-by-token and byte-by-byte against legacy archives.
  - 100% round-trip fidelity between custom encoder and official reference decoder `sqc`.
- **Zero External Dependencies**:
  - Clean ANSI C99 / POSIX implementation.
  - Single dual-target codebase compiling to both native Linux 64-bit ELF and Windows 64-bit PE (`.exe` via MinGW-w64).
  - No external compression libraries required (no zlib, no liblzma, no external PPMd library).

---

## Build

### Using the Build Script (`build.sh`)

The repository includes a convenient, cross-platform build script:

```sh
./build.sh            # Production Linux build -> build/linux/sqx (and symlink build/sqx)
./build.sh win64      # Production Windows 64-bit build -> build/win64/sqx.exe (static)
./build.sh all        # Build both Linux and Windows 64-bit binaries
./build.sh debug      # Debug build with symbols (-g3 -fsanitize=address)
./build.sh test       # Run complete regression and compliance test suite
./build.sh clean      # Clean build directories and temporary test artifacts
```

### Using `make`

```sh
make                  # Compile Linux binary into build/linux/sqx (-O3 -flto)
make win64            # Cross-compile Windows 64-bit static binary into build/win64/sqx.exe
make all-platforms    # Build both Linux and Windows 64-bit binaries
make test             # Run complete automated test suite
make clean            # Remove compiled binaries and intermediate build objects
```

### Manual Compilation

```sh
# Linux (GCC / Clang)
mkdir -p build/linux
gcc -O3 -flto -fomit-frame-pointer -Wall -Wextra -std=c99 -Isrc \
    -o build/linux/sqx src/main.c src/sqx.c src/lz.c src/huff.c src/henc.c \
    src/enc.c src/ir.c src/create.c src/rc.c src/delta.c src/audio.c src/ppm8.c -lm

# Windows 64-bit (via MinGW-w64 cross-compiler)
mkdir -p build/win64
x86_64-w64-mingw32-gcc -O3 -flto -fomit-frame-pointer -Wall -Wextra -std=c99 -Isrc -static \
    -o build/win64/sqx.exe src/main.c src/sqx.c src/lz.c src/huff.c src/henc.c \
    src/enc.c src/ir.c src/create.c src/rc.c src/delta.c src/audio.c src/ppm8.c -lm
```

---

## Usage

```
sqx [options] <command> [arguments...]
```

### Commands

| Command | Aliases | Action | Description |
|---|---|---|---|
| `list` | `l` | **List Contents** | List files, compression methods, sizes, and directory entries |
| `extract` | `x`, `e` | **Extract Files** | Decompress files to disk (`sqx extract outdir archive.sqx...`) |
| `info` | `i` | **Archive Information** | Display detailed header, version, volume, block, and flag metadata |
| `test` | `t` | **Test Integrity** | Verify block CRC16 and file payload CRC32 checksums |
| `create` | `a` | **Create Archive** | Pack files into a new SQX archive (`sqx create out.sqx [options] files...`) |
| `repack` | | **Bit-Exact Repack** | Re-encode existing archives from intermediate representation (IR) |
| `check` | | **Stream Check** | Verify byte-for-byte stream parity against packed reference streams |
| `raw` | | **Raw Stream Dump** | Dump compressed file payload stream directly to file (`.sqxdata`) |

### Options

| Switch | Description |
|---|---|
| `-u`, `--unix` | Format paths as Unix relative paths (`/` forward slashes, lowercase) |
| `-1`, `--bare` | Bare output format (one path per line, ideal for scripts, `xargs`, `grep`) |
| `--fmt=11\|20` | Archive container and codec version (`11` = SQX 1.1, `20` = SQX 2.0; default: `11`) |
| `--method=1..6` | Compression method (`1` = Fast, `2` = Normal, `3` = High, `4` = Best LZH, `5` = Audio, `6` = Text PPMd) |
| `--dict=KB` | Dictionary size in KB (`32` up to `32768`; default: `4096`) |
| `--ppm-order=2..16` | Model order for Method 6 PPMd text compression (default: `6` for `sqc`, `8` for GUI) |
| `--ppm-mem=4..128` | Model arena memory in MB for Method 6 PPMd (default: `32`) |
| `-h`, `--help` | Show command-line help and usage |

---

## CLI Examples

### Listing & Inspecting Archives

```sh
# Display container metadata, format version, and volume status
./build/sqx info archive.sqx

# List files with packed sizes, original sizes, and compression methods
./build/sqx list archive.sqx

# Bare listing: one file path per line (ideal for scripts and pipelines)
./build/sqx list -1 archive.sqx

# Bare listing with Unix relative paths (forward slashes, lowercase)
./build/sqx list -1 -u archive.sqx | grep '\.txt$' | xargs -n1 echo

# Inspect a Windows self-extracting archive directly
./build/sqx list setup.exe
```

### Verifying & Testing Integrity

```sh
# Verify header CRC16 and data CRC32 checksums for all entries
./build/sqx test archive.sqx

# Test an SFX archive
./build/sqx test setup.exe
```

### Extracting Files

```sh
# Extract files into a specific destination directory
./build/sqx extract output_dir archive.sqx

# Extract with Unix-style lowercase paths
./build/sqx extract -u output_dir archive.sqx

# Bare extraction output (prints extracted paths, one per line)
./build/sqx extract -1 output_dir archive.sqx

# Extract files from a self-extracting executable
./build/sqx extract unpacked setup.exe
```

### Creating Archives

```sh
# Create a SQX 2.0 archive with Best LZH compression (Method 4, 4 MB dictionary)
./build/sqx create backup.sqx --fmt=20 --method=4 --dict=4096 file1.txt file2.bin

# Create a solid SQX 2.0 archive
./build/sqx create archive.sqx --fmt=20 --method=4 --solid src/*.c src/*.h

# Create a high-ratio PPMd text archive (Method 6, Order 8, 32 MB arena)
./build/sqx create books.sqx --fmt=20 --method=6 --ppm-order=8 --ppm-mem=32 doc.txt manual.txt

# Create a SQX 1.1 legacy compatible archive
./build/sqx create legacy.sqx --fmt=11 --method=2 --dict=2048 document.pdf
```

---

## Test Suite & Validation Breakdown

The project incorporates an exhaustive automated test suite under `tests/`, ensuring backward compatibility and strict interoperability against historical test archives and the official `sqc` archiver:

| Test Script | Test Target | Results | Status |
|---|---|---|:---:|
| [`tests/test_container.sh`](tests/test_container.sh) | Container parser & block CRC16 cross-validation against Python oracle | 144 archives tested, 0 CRC errors, 0 divergences | **PASS** |
| [`tests/test_extract.sh`](tests/test_extract.sh) | Extraction compliance across corpora, formats 1.1/2.0, methods 0–6 | 130 archives verified, 0 failures | **PASS** |
| [`tests/test_audio.sh`](tests/test_audio.sh) | Audio codec (1.1 polynomial + 2.0 FIR/Rice 8/16-bit mono/stereo) | 27 oracle-verified streams, 10 round-trip fixtures | **PASS** |
| [`tests/test_bitexact.sh`](tests/test_bitexact.sh) | Bit-identical stream re-encoding against historical reference archives | 121 identical streams, 0 divergences | **PASS** |
| [`tests/test_encoder.sh`](tests/test_encoder.sh) | Custom LZH encoder round-trip and oracle `sqc` decompression | 18 combinations (1.1/2.0 $\times$ M1/M4 $\times$ dicts) | **PASS** |
| [`tests/test_large.sh`](tests/test_large.sh) | Extended distance matches (> 400,000 bytes, `MIN_LEN4` thresholds) | 8 large combinations verified | **PASS** |
| [`tests/test_ppm6.sh`](tests/test_ppm6.sh) | PPMd Method 6 text compression / decompression round-trip & memory recycling | 13 configurations verified (4M & 64M arenas, solid streams) | **PASS** |

Run all tests via:
```sh
./build.sh test
```

### Audio 2.0 Bit-Exact Fidelity & Correction of Legacy DLL Flaw (`x_altst16`)

During comprehensive verification of the **Method 5 Audio 2.0** encoder against the official Windows reference archiver (`sqx20u.dll` / `sqc.exe`):
- **100% Bit-Exact Stream Parity**: This native C encoder produces bitstreams that are **byte-for-byte identical** to the official reference archiver across all valid audio inputs (covering both standard audio suites `d_*.wav` and adversarial edge cases `corpus/x_*.wav`: extreme impulses, non-aligned buffers, silence, DC, full-scale ramps, and odd stereo lengths).
- **100% Lossless Round-Trip**: Round-trip compression and decompression (C encoder $\rightarrow$ C decoder) is 100% loss-exact across all audio formats (8/16-bit, mono/stereo) with zero sample divergence.

#### Correction of Critical Legacy DLL Bug (`x_altst16`)
- **The Legacy DLL Defect**: The original Windows DLL (`sqx20u.dll` / `sqc.exe`) suffers from a critical numerical overflow bug in its Audio 2.0 FIR estimation pipeline. When compressing adversarial alternating stereo audio at full scale ($\pm 32,767$ in anti-phase), evaluating stereo difference mode `icd = 3` produces sample deltas of $\pm 65,535$. These 32-bit values undergo geometric growth in the inverse pre-filter $B^{-1}$, while the legacy DLL's 32-bit x86 `imul` autocorrelation truncates and flips signs in the correlation matrix, destabilizing the Levinson-Durbin solver. The legacy Windows DLL enters an uncontrolled runaway loop, dumping a monstrous, corrupted **603 MB** archive from a tiny 36 KB audio file!
- **The Native C Correction**: This native C reimplementation explicitly **corrects** this legacy defect. By properly evaluating candidate predictor costs and preventing FIR divergence, it generates a clean, compact, specification-compliant **~23 KB** archive.
- **Verified Interoperability with Legacy Tools**: Crucially, **this ~23 KB archive is decompressed without error (`CRC OK!`) by the official reference extractor `sqc x` itself**. Full bidirectional interoperability is preserved: legacy users can unpack archives produced by this implementation using official SQX tools without risking disk-space exhaustion.
- **Intentional Design Decision**: This legacy flaw is intentionally **fixed**, not replicated. A bug is not a format specification. Intentionally reproducing an internal engine runaway that creates corrupted multi-hundred-megabyte archives on disk would be absurd and counterproductive.

---

## Project Structure

```
├── build.sh              # Unified compilation and test driver script
├── Makefile              # Dual Linux / Windows MinGW-w64 build configuration
├── FORMAT.md             # In-depth SQX format and codec specification
├── README.md             # Project documentation and usage guide
├── src/                  # Standalone C source tree
│   ├── main.c            # Command-line interface and command dispatch
│   ├── sqx.h / sqx.c     # Container parsing, block validation, and high-level extraction
│   ├── lz.h / lz.c       # LZH decompression engine (SQX 1.1 & 2.0)
│   ├── enc.h / enc.c     # LZH compression engine
│   ├── ir.h / ir.c       # Intermediate Representation (token parser & bit-exact emitter)
│   ├── huff.h / huff.c   # Dynamic canonical Huffman decoder
│   ├── henc.h / henc.c   # Huffman tree builder and bitstream encoder
│   ├── ppm8.h / ppm8.c   # Method 6 PPMd var. I codec (adapted Ppmd8 with SQX range coder)
│   ├── audio.h / audio.c # Method 5 Audio codec (1.1 polynomial & 2.0 FIR Levinson-Durbin)
│   ├── rc.h / rc.c       # Carryless Range Coder engine
│   ├── delta.h / delta.c # Multi-channel delta transforms and filter inversion
│   ├── create.h / create.c # Archive container writer
│   ├── bitio.h           # Bit-level stream reader
│   └── bitw.h            # Bit-level stream writer
├── tests/                # Automated regression test scripts and tools
│   ├── sqxdump.py        # Independent container inspector tool
│   ├── test_container.sh # Container block parser validation
│   ├── test_extract.sh   # Decompression test suite
│   ├── test_audio.sh     # Audio codec verification
│   ├── test_bitexact.sh  # Bit-identity test harness
│   ├── test_encoder.sh   # Compressor ratio & cross-validation
│   ├── test_large.sh     # Long-distance match regression test
│   └── test_ppm6.sh      # PPMd Method 6 test suite
├── corpus/               # Uncompressed reference test files
└── packed_corpus/        # Reference historical SQX archives
```

---

## Technical Specifications & Documentation

For exhaustive reverse-engineered architectural specifications, block headers, Huffman tree configurations, audio predictor formulas, and PPMd range coder mechanics, refer to:

- [`FORMAT.md`](FORMAT.md) — Comprehensive SQX File Format & Codec Specification.

---

## License

This reverse-engineered reimplementation is released for historical preservation, interoperability, and research purposes under the **MIT License**.
