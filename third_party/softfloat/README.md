# Berkeley SoftFloat 3e

This is the subset of John R. Hauser's Berkeley SoftFloat needed for OpenStone's software x87 stack. The arithmetic, specialization and header sources are unchanged from the official release except for line-ending and trailing-whitespace normalization. `platform.h` and `CMakeLists.txt` are OpenStone integration files.

- Upstream: https://www.jhauser.us/arithmetic/SoftFloat.html
- Archive: https://www.jhauser.us/arithmetic/SoftFloat-3e.zip
- Archive SHA-256: `21130ce885d35c1fe73fc1e1bf2244178167e05c6747cad5f450cc991714c746`
- License: BSD 3-Clause; see [LICENSE.txt](LICENSE.txt).

The 8086 NaN specialization is used for x87, separately from the existing SSE policy. The internal software-FPU control/exception variables are thread-local; OpenStone's helpers save and restore them. Full extended precision and nearest-even rounding are selected for arithmetic. No host `long double`, LLVM dependency, heap allocation or native x86 instruction execution is required.

The platform configuration targets the project's little-endian ARM/x64 platforms. GCC/Clang integer intrinsics are optional. `OPENSTONE_SOFTFLOAT_PORTABLE=ON` selects the plain C path, which is tested with LLVM disabled and provides the fallback required by compilers without those intrinsics. The desktop build copies the complete license notice into the app's licenses directory.
