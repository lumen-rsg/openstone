# Third-party notices

Original OpenStone code is Copyright (C) 2026 OpenStone contributors and is licensed under GPL-3.0-or-later. Third-party files retain their existing notices and licenses.

## Berkeley SoftFloat Release 3e

- Location: `third_party/softfloat/`
- Copyright: The Regents of the University of California (see individual files and the license for years).
- License: BSD 3-Clause.
- License text: [third_party/softfloat/LICENSE.txt](third_party/softfloat/LICENSE.txt).
- Source and local integration notes: [third_party/softfloat/README.md](third_party/softfloat/README.md).

## Nunito

- Font: `assets/fonts/Nunito.ttf`.
- Copyright: 2014 The Nunito Project Authors.
- License: SIL Open Font License 1.1.
- License text and complete copyright notice: [assets/fonts/OFL-Nunito.txt](assets/fonts/OFL-Nunito.txt).

## Build and runtime dependencies

Qt, LLVM, PortAudio, RtMidi, Ruby, zlib, and iconv are separately supplied dependencies and retain their respective licenses. They are not relicensed by OpenStone. The source checkout does not bundle their distributions. When distributing a built application, include the notices and source or other materials required by the versions and linking configurations you distribute.

## FlowStone reference material

FlowStone is referenced for interoperability research. Vendor binaries, bundled modules, and extracted or adapted vendor source are not included in the public repository and are not covered by OpenStone's GPL grant. `tests/fixtures/flowstone/external-sources.json` records the provenance of private test sources; `.gitignore` excludes those files. Numeric reference captures and OpenStone-authored probes are included separately.
