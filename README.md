# OpenStone

OpenStone is an open-source visual programming environment for audio, MIDI, and interactive interfaces, inspired by FlowStone. Connect primitives, group them into reusable modules, and write DSP, Assembler, or Ruby code alongside the schematic.

**OpenStone is an experimental project.** The editor and runtimes are usable for testing, but compatibility is still in progress. FlowStone schematic import is partial; importing a file does not guarantee that it will render or behave identically. Development and validation currently center on macOS, with Windows and Linux validation still needed.

## What is here

- A Qt Quick schematic editor with nested modules, front panels, reusable library modules, search filters, favorites, and recent items.
- Saving and loading `.ostone` projects, undo/redo, group editing, clipboard operations, auto-linking, and straight or curved wires.
- Green values and triggers, Yellow drawing and interface values, and Mono, Mono4, double-precision, and Poly DSP streams.
- DSP Code and FlowStone-style Assembler frontends with an interpreter and an optional LLVM ORC backend.
- Ruby nodes with typed connectors, scheduled events, saved state, drawing, and mouse interaction.
- Live audio, MIDI, and serial device adapters kept separate from the core runtimes.
- An experimental `.fsm` importer with explicit diagnostics for unsupported records.

OpenStone is an independent project and is not affiliated with or endorsed by the makers of FlowStone.

## Build

### Requirements

The core libraries need CMake 3.24 or newer, C11 and C++23 compilers, and iconv. Ninja is recommended. The desktop application also needs:

- Qt 6.8 or newer: Quick, Quick Controls 2, Shader Tools, SVG, Serial Port, Network, Multimedia, and the Qt Gui private development headers.
- PortAudio, RtMidi 5 or newer, and zlib.
- A Ruby executable on `PATH` to run Ruby nodes. Override it with `OPENSTONE_RUBY_EXECUTABLE` if needed.

macOS and most Linux systems provide iconv. Windows builds need libiconv available to CMake. Add dependency prefixes to `CMAKE_PREFIX_PATH` when they are not discovered automatically; PortAudio and RtMidi can also be found through pkg-config.

```sh
git clone https://github.com/lumen-rsg/openstone.git
cd openstone
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/qt
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Omit `CMAKE_PREFIX_PATH` if your toolchain already finds Qt. On macOS, open `build/OpenStone.app`; on Linux, run `./build/OpenStone`; on Windows, run `build/OpenStone.exe` with the required dependency DLLs available. Multi-configuration generators may put the executable under `build/Release`; pass `--config Release` when building and `-C Release` when testing.

### Headless build

The core editing and DSP runtimes do not require Qt or device backends:

```sh
cmake -S . -B build/headless -G Ninja \
  -DOPENSTONE_BUILD_DESKTOP=OFF
cmake --build build/headless --parallel
ctest --test-dir build/headless --output-on-failure
```

### LLVM ORC

The interpreter is built by default. To enable the LLVM backend, install LLVM 21.1 and configure with:

```sh
cmake -S . -B build/orc -G Ninja \
  -DOPENSTONE_BUILD_DESKTOP=OFF \
  -DOPENSTONE_ENABLE_LLVM_ORC=ON \
  -DLLVM_DIR=/path/to/llvm/lib/cmake/llvm
cmake --build build/orc --parallel
ctest --test-dir build/orc --output-on-failure
```

You can enable the same option in the desktop build. Tests exercise both backends when ORC is enabled.

## Using OpenStone

Search or drag primitives from the library, then connect an output to an input. Double-click a module to enter it. Use **File → Save** to save a `.ostone` project. DSP Code and Ruby nodes contain editable source; the [manual](docs/manual.md) covers code examples, connector types, front panels, device setup, and editing shortcuts.

The `.fsm` importer reads the supported portion of a FlowStone schematic and reports unsupported records. See the [importer status](docs/compatibility/flowstone-fsm-import.md) for the current coverage and limitations. Always keep your original schematic.

## Development and compatibility

The C++ core is split between `src/editor`, `src/green`, `src/stream`, and `src/yellow`; `src/desktop` connects it to Qt and device backends. Tests include sample-by-sample DSP checks across buffer sizes, editor and persistence checks, and Ruby integration tests.

- [Manual and runtime notes](docs/manual.md)
- [Primitive inventory](docs/compatibility/flowstone-alpha-primitive-inventory.csv)
- [Bundled-module import report](docs/compatibility/flowstone-fsm-bundled-import.json)
- [Assembler audit and remaining limits](docs/compatibility/flowstone-assembler-audit-notes.md)
- [Reference fixtures and optional private-corpus tests](docs/testing.md)

Extracted FlowStone source and vendor binaries are **not distributed** in this repository. Tests that need that private reference corpus explicitly report skipped subchecks; the self-contained tests still run. Numeric reference traces and authored probes are included. A passing compile audit establishes compilation coverage, not complete numerical parity.

Please include the platform, build options, a minimal schematic or source snippet you have permission to share, and expected versus actual behavior when reporting a problem in [Issues](https://github.com/lumen-rsg/openstone/issues).

## License

Original OpenStone code is licensed under the **GNU General Public License, version 3 or later** (`GPL-3.0-or-later`). See [LICENSE](LICENSE).

Bundled Berkeley SoftFloat and Nunito retain their own licenses. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). The project license does not grant rights to FlowStone or to separately supplied reference sources.
