# Assembler signal-input working copies

Assembler may now store numeric values into a declared signal input during stages 1, 2 and 3. The destination is the block's private input slot, refreshed from its connected source before each execution stage. Stores never modify caller buffers, upstream outputs or another block's view of the same signal. DSP Code's input-assignment restriction is unchanged.

## Concrete audit gap and source evidence

The last two failing serialized Alpha 5483 kernels were complete envelope implementations. Both normalize their attack, decay and release inputs in place: they load a parameter, quantize it to a multiple of sixteen, and store the result back to the input operand. The legacy implementation uses x87 `fstp`; the current implementation uses packed conversions and `movaps`. Rejecting every input destination prevented these unchanged blocks from compiling.

Both fixtures are exact source substrings of the Alpha 5483 x64 executable, whose SHA-256 is `f4dbec8a34ba6fb7b8e91ed22038b69d081b94e275562c80d7ca9815a0b3832e`. The inspected x86 executable is `39c50f8b4973dca4202400a0951d7181ed0fe2a0ca21704e96ac895b0c8516e8`.

| Fixture | Bytes | x64 file offset | SHA-256 |
| --- | ---: | --- | --- |
| `assembler-envelope-legacy.asm` | 6099 | `0x791da4` | `a40f1ed2aafbc5aa4d1dcdd4aaf394a569fb457ce3b4f0fb3f2c39fb5ce3c6a1` |
| `assembler-envelope-current.asm` | 6624 | `0x7adce4`, `0x7b10b2` | `f51280946a7ac918b19c13fd93428d44cf8f6d57d47ad963a646608b40c89bf2` |

No declaration, instruction, register initialization or stage is added to these fixtures. Each has seven inputs; the legacy block has Done/value outputs, and the current block additionally reports its envelope stage.

## Runtime contract

The compiler records input destinations as stage dependencies, including a destination written without a preceding read. This supplies the current signal value for partial byte or lane stores and preserves existing graph scheduling. The private slot is cleared before loading a writable input, so an unconnected input and unused lanes of a scalar-width connection start at zero on every execution. Repeated writes cannot accumulate from a previous sample or a previous stage. Use a local variable for state that must persist between stages or samples.

The same existing slot operations serve packed/scalar Float, Double, Boolean and PolyInt operands in both backends. Poly preparation refreshes each voice's private copy; writes remain isolated across packets, peers and recycled voice slots. No new backend instruction or public serialization field is needed.

Stage 0 cannot read or write signal inputs. External FloatArray/Mem input arrays and MemArray descriptors remain read-only. Explicit bounded Mem writes retain their existing capability rules. Engine constants, bare engine pointers and opaque routine-target storage restrictions are unchanged.

This is OpenStone's explicit working-copy contract. The recovered source proves that these stores are legal in FlowStone; static evidence alone does not establish FlowStone's aliasing behavior for every possible graph or stage arrangement.

## Validation

`assembler_input_writes` runs both complete, unchanged envelope sources through the interpreter and LLVM ORC. An independent envelope state machine checks every output sample and raw Float value over four staggered lanes, 1537 samples and buffer sizes 1, 3, 7, 31, 64 and 257. Cases include attack/decay/sustain/release, parameter changes, fast release, smooth and hard retriggers, inactive periods, second notes, Done events, reported stages, reset, unchanged caller inputs and preserved caller floating-point state.

Additional tests check raw byte/arithmetic/x87 stores, untouched neighboring bits, Double-only precision (`1 + 2^-40`), Boolean masks, PolyInt words, fan-out isolation, scalar-width and unconnected inputs, stage refresh and stage-3 latency. A Poly test spans two voice packets, note release, voice ending and slot reuse across buffers. Negative cases retain the initialization, external-array, descriptor, routine-target and DSP Code restrictions. Existing byte arithmetic/logical/register negative tests now distinguish legal signal working copies from read-only external arrays.

The audit integration test prepares both original envelope fixtures without rewriting them. The strict corpus advances from **56 to 58 prepared entries** and **64 to 66 frontend-accepted entries**, retaining every earlier pass. All **19 serialized sources** now prepare in both backends; eight other entries still require explicit engine bindings. The corpus retains 236 category/source entries, 231 source hashes, zero missing mnemonic names and no backend-only failures.

These are exact recovered sources plus independent expected results, not new native FlowStone Windows numerical captures. Earlier fixtures and numerical captures remain unchanged.
