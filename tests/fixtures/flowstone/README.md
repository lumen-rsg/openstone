# Compatibility fixtures

Extracted or adapted FlowStone source is kept out of the public repository. Some `.asm` filenames documented below are private reference files and will not be present in a public checkout. See [the omitted-source manifest](external-sources.txt), [provenance metadata](external-sources.json), and [test setup](../../../docs/testing.md). Authored probes and numeric captures remain available. Private-source subchecks explicitly skip without the corpus; they can be required with `OPENSTONE_REQUIRE_FLOWSTONE_FIXTURES=1`.

## FlowStone Alpha 5483

`multi-sample-bounds.asm` is the unchanged 648-byte wave-index validity block extracted from `Multi Sample.hom` at byte offset 55158.

The input to FlowStone's Signal Analyser capture was `waveNr[n] = n * 0.25 - 8` from sample 0 to 63. The two captures contain little-endian binary32 words recorded with the MemArray inlet unwired and with one wave loaded from `test-wave.wav`. A true mask is stored as `0xffffffff`. The reference has no true samples with the inlet unwired and one true sample at index 32 with the wave loaded.

A separate FlowStone metadata probe measured the wave count and last index as `(0, 0)` when unwired, `(1, 0)` with one wave, and `(2, 1)` with two slots.

`stream-power.asm` is the unchanged 1379-byte Assembler block from `Stream Power.hom` at byte offset 191. `compressor-power.asm` is the unchanged 2274-byte power block from `Compressor.hom` at byte offset 18145.

The two `.f32` traces were captured separately in FlowStone using `exp[n] = 2 - n / 128` and `bas[n] = 0.5 + n / 8`, for samples 0 through 63. Each trace contains 64 little-endian binary32 samples. The OpenStone parity test feeds each packed lane a rotated copy of the same input pair and matches every output word against the corresponding reference sample.

## Packed Poly voices

`poly-voice-reverse.asm` is the same Assembler source run in FlowStone Alpha 5483 x64 and on both OpenStone backends. A MIDI to Poly Pitch output feeds its generic Stream input. The output goes through Poly to Mono into Mono to Graph (64 samples), then a Ruby node saves `values.pack("e*")`. An enabled, unconnected Direct Sound Out supplies the audio clock without sending the test signal to speakers. Signal Analyser cannot capture this live Mono output.

The first capture holds four channel-1 notes, pitches 60–63, at velocity 127. The second adds pitches 64–67 while the first four remain held. Each packet uses weights `w = pitch - 59`. Its periodic source is `w * (1 + (n % 64) / 64)`. `shufps xmm0,xmm0,27` reverses the four neighboring voices before multiplying by each destination voice's weight. The extra weighting makes the shuffle observable after summing: four voices produce `20 * (1 + n/64)`, while eight produce `184 * (1 + n/64)`, for the captured period. Without the shuffle, the initial sums would be 30 and 204. The additional notes start on a 64-sample audio boundary. Both captures begin at phase zero; the raw files are saved without rotation or numerical transformation.

`poly-voice-four.f32` and `poly-voice-eight.f32` each contain 64 little-endian binary32 samples captured from FlowStone. The regression compares every raw output word in the interpreter and LLVM ORC at buffer sizes 1, 7, and 64, for three periods to cover wraparound. It also runs the real editor MIDI/voice-bank/Assembler/Poly-to-Mono/Audio Out path against both traces. Separate headless checks inspect each individual output lane, a partially populated final packet, inactive slots, staggered starts, stage-zero initialization, early/late stage ordering across DSP blocks, note-off, lane reuse, reset, and allocation-free rendering. The native traces validate the summed outputs for fully occupied packets; they do not establish FlowStone's inactive-lane or voice-allocation policy.

## MemArray wave data

`memarray-wave-array-kernel.asm` is the exact 982-byte Assembler kernel embedded in the bundled Wave Array Read primitive in FlowStone Alpha 5483's `x64/FlowStone.exe`, at file offset `0x596000`. Its SHA-256 is `438ef530181be04f9f7729ef95619cb438287ba1c7090de03a485288a25f757e`. The current Multi Sample modules use this primitive for playback; their separate Assembler validity mask does not fetch samples.

The kernel reads the channel table through descriptor field `[4]`, loads wave pointers from `[5]` with an eight-byte stride, and obtains frame limits through `[7]`. It clamps and rounds each lane's wave and sample indices, then reads either mono floats or interleaved stereo frames. A separate native probe confirmed that `[6]` stores byte sizes: a loaded 16-frame, two-channel float buffer reports 128 bytes and a last-frame limit of 15.

The `memarray-wave-left.asm` and `memarray-wave-right.asm` fixtures expose the kernel's original `in1`, `in2`, `out0`, and `out1` ports and initialize the three caller registers preserved by its push/pop sequence. The right-channel fixture copies `out1` to the first output for capture. Their instruction body is unchanged apart from whitespace and the surrounding port/constant context. FlowStone's editor capture fixtures, `memarray-wave-*-flowstone.asm`, use the equivalent `jz` alias for the embedded `je` and rename its supplied `_ZERO` constant to `fZERO`.

The independently captured `memarray-input.f32` contains `position[n] = 0.5*n - 8` for 64 samples, covering negative positions, rounding ties, valid frames, and positions beyond the end. The generated 16-bit PCM files `memarray-mono.wav` and `memarray-stereo.wav` contain exactly representable power-of-two sample values. The mono-file bench used FlowStone's default stereo expansion, so its two output captures agree; the stereo file has distinct left and right values. OpenStone also exercises the true one-channel layout. All four captures, `memarray-mono-left.f32`, `memarray-mono-right.f32`, `memarray-stereo-left.f32`, and `memarray-stereo-right.f32`, matched independently calculated sample words.

The regression compares every captured word on the interpreter and LLVM ORC at buffer sizes 1, 7, and 64, using four independently rotated packed lanes. It also checks mixed mono/stereo arrays with different wave choices in each lane. Additional headless tests cover all four descriptor tables, empty entries, clearing/replacing the array, retained wave lifetimes, bounds, shared sample writes, and read-only tables. Compiling this real kernel also required the `je` alias, address-register push/pop preservation, and backward-branch checks that allow a temporary register's reference type to change when its incoming value is overwritten before any read.

## Denormal Remover (Bitmask)

`denormal-mask.asm` is the unchanged 160-byte block from `Denormal Remover (Bitmask).hom` at byte offset 553. The input contains 16 copies each of the smallest positive subnormal, the smallest negative subnormal, the smallest positive normal, and 1.0. A `movaps` pass-through into Signal Analyser reproduced all four raw input words.

The original producer combines the subnormal terms on the left of a bitwise OR and the normal terms on the right. The second producer swaps the two sides. The signal samples stayed identical. The `por xmm1,xmm1` in the unchanged block retained different register contents on the two runs.

| Captured words | Positive subnormal group | Negative subnormal group | Minimum normal group | 1.0 group |
|---|---|---|---|---|
| Stream input | `00000001` | `80000001` | `00800000` | `3f800000` |
| Original producer, entry `xmm1` | `00000000` | `00000000` | `00800000` | `3f800000` |
| Original producer, unchanged output | `00000000` | `80000001` | `00000000` | `00000000` |
| Reordered producer, entry `xmm1` | `00000001` | `80000001` | `00000000` | `00000000` |
| Reordered producer, unchanged output | `00000001` | `80000001` | `00800000` | `3f800000` |
| Explicit `pxor` initialization | `00000000` | `80000001` | `00800000` | `3f800000` |
| `mulps` by 1 | `00000000` | `80000000` | `00800000` | `3f800000` |
| `mulps` by 0.5 | `00000000` | `80000000` | `00000000` | `3f000000` |

The register and multiply traces were measured by encoding each 16-bit half of the raw result in the mantissa of a normal float before capture and recombining the halves offline. The saved output traces are direct captures. The OpenStone test compiles the exact unchanged block and supplies each observed `xmm1` value through `Runtime::setDspIncomingXmm`, without a source prologue or extra connector. The unbound bank uses OpenStone's documented zero default; it is not assumed to reproduce FlowStone's upstream scratch state. [Incoming-register policy and tests](../../../docs/compatibility/flowstone-assembler-xmm-entry.md). A separate case tests the explicitly initialized variant. Its sign-inclusive mask passes negative subnormals.

## x87 extended precision and community probe

OpenStone now stores x87 values as a 64-bit explicit significand and a 16-bit sign/exponent record. Portable Berkeley SoftFloat 3e arithmetic keeps the same representation on ARM and x64, in the interpreter and LLVM ORC. Arithmetic uses the architecture's full-precision default: a 64-bit significand with round-to-nearest, ties-to-even. Actual FlowStone precision-control settings on physical Windows x64 are not yet measured. This is an architectural implementation with headless verification, not a new Windows x64 parity claim.

Headless tests exercise cancellation around 2^53 and 2^63, the rounding boundary at 2^64, multiplication and division, temporary overflow/underflow outside the binary64 exponent range, non-popping Float/Double stores, explicit memory rounding, stack copies/exchanges, a local call carrying an extended value, integer conversion/rounding, scaling including extreme exponents and invalid infinite scaling, and signed remainder. They run in scalar and four-lane graphs across buffer sizes 1, 7 and 64 on both backends. Additional checks cover eight-byte stores through Mem references without overwriting neighboring cells, and allocation-free packed Poly processing. A build with LLVM disabled and SoftFloat's plain C integer path enabled runs the same interpreter tests. Existing saved FlowStone x87/Stream Power/Compressor traces still pass unchanged.

`dfst`, `dfstp`, `fdivp`, and `dfdivp` are supported as well. These spellings appear in the bundled Alpha executable's own double oscillator/conversion kernels, including the sequences at file offsets `0x5903d0`, `0x591e90`, `0x593870`, and `0x593f30` in the extracted string catalog. Float and Double memory stores round a copy; the stack record retains its extra bits until popped.

For community testing, paste **`x87-extended-probe.asm`** into a FlowStone Alpha Assembler node. Enable an unconnected Direct Sound Out to provide the audio clock, then connect each of its eight outputs to a Mono Readout. In declaration order, OpenStone's readouts are:

| keep24 | keep53 | keep63 | limit64 | recoverHigh | recoverLow | keepAfterStore | storedDouble |
|---|---|---|---|---|---|---|---|
| 1 | 1 | 1 | 0 | 1 | 4 | 1 | 0 |

A 53-bit significand setting would normally give `1, 0, 0, 0, 1, 4, 0, 0`; a 24-bit setting would give `0, 0, 0, 0, 1, 4, 0, 0`. The range-recovery outputs also distinguish an extended exponent range from binary64 intermediates. Record all eight values, the FlowStone build, CPU model, and audio buffer size. The included probe is itself compiled and checked on both OpenStone backends across buffer sizes.

Exact transcendental microcode, selectable x87 precision/rounding control words, exception/status instructions, and partial `fprem` reduction remain outside this pass. Trigonometry, `f2xm1`, and `fyl2x` retain their existing binary64 libm behavior. [Intel x87 precision-control reference](https://cdrdv2-public.intel.com/843827/253665-sdm-vol-1-dec-24.pdf), [SoftFloat format and arithmetic documentation](https://www.jhauser.us/arithmetic/SoftFloat-3/doc/SoftFloat.html).

## SSE floating-point edge cases

The `assembler-fp32-*` and `assembler-fp64-*` fixtures are independently captured raw result words from FlowStone Alpha 5483 x64, running in Windows 11 ARM through its x64 execution environment. Each contains 64 samples. Identity probes `lhs` and `rhs` reproduced every intended input word before the arithmetic results were accepted. The input DSP sources are included; their comparison counters must match their sample precision so the masks preserve the full data width. The single-precision headless source seeds lanes 1–3 at offsets 17, 34 and 51, and wraps its counter at 64. Lane 0 reproduces the captured period unchanged.

For each precision the cases cover all signed-zero pairs, both infinities, invalid arithmetic, positive and negative quiet and signaling NaNs with distinct payloads, every two-NaN pairing and operand order, minimum and maximum normal values, positive and negative subnormals, overflow/underflow, halfway addition, non-exact division, negative square-root arguments, and integer conversion boundaries. The binary32 cases additionally cover `cvtps2dq` and `cvttps2dq`, including positive and negative ties. Each instruction block is unchanged between the FlowStone bench and OpenStone.

A following Assembler block bitwise masks each 16-bit part of the result and uses `cvtdq2ps` to send exact, ordinary numeric floats to Signal Analyser. Two captures reconstruct each binary32 word, and four reconstruct each binary64 word. This preserves signaling-NaN payloads and signs without passing them through a numeric display. The decoder checks that every captured part is an integer from 0 to 65535 and joins the parts with integer shifts. The `.f32` and `.f64` files contain those little-endian reconstructed words, with no numerical conversion.

Measured behavior: SSE arithmetic flushes subnormal operands/results with sign preserved; moves and min/max retain raw words as appropriate. Add/subtract/multiply/divide prioritize a signaling NaN, then the left NaN, and quiet the selected payload. Invalid arithmetic produces a positive canonical NaN in this reference environment. Square root preserves the selected NaN payload and produces a negative canonical NaN for negative numeric inputs. Min/max choose the second operand for ties or unordered comparisons, including an unquieted signaling NaN. These details are pinned to the recorded environment. They are not a claim that invalid-result signs or NaN selection agree with every physical x86 CPU. [Intel floating-point architecture reference](https://cdrdv2-public.intel.com/843827/253665-sdm-vol-1-dec-24.pdf)

The regression compares every word for 16 instructions, plus both identity probes at each precision, on the interpreter and LLVM ORC at buffer sizes 1, 7 and 64. It exercises scalar and four-lane graphs with independently rotated inputs. The single-precision test also reads results through an Assembler bit encoder so unquieted NaNs can be verified before any display conversion. Separate headless tests enable each host rounding direction and ARM flush/default-NaN or SSE FTZ/DAZ controls, raise a pre-existing exception flag, then check round-to-nearest arithmetic/conversion, raw subnormal/signaling-NaN moves, and complete restoration of the caller's environment. Existing allocation checks cover rendering with the environment scope enabled.

Hardware-specific `rcpps` estimates, exact x87 transcendental algorithms, and architectural exception-status instructions remain open. The later x87 pass below adds extended-precision intermediates.

## Double Assembler

The `assembler-addpd.asm`, `assembler-subpd.asm`, and `assembler-mulpd.asm` blocks were run in FlowStone Alpha 5483 with `streamdoublein` and `streamdoubleout`. Each uses the coefficient `0.5 + 2^-40`. The stimulus is `(n - 32) / 8 + (n + 1) * 2^-40` for samples 0 through 59, followed by the positive and negative minimum subnormals and the positive and negative minimum normal numbers.

A separate Assembler reads the double output port. It uses `movapd` to load, `shufps xmm0,xmm0,85` for the high 32 bits, `psrld xmm0,16` for the upper half of either word, and `pand` to mask the selected 16 bits. `cvtdq2ps` then converts the selected bits to an exact integer-valued float for the Signal Analyser preview. Four captures of 64 samples yield the four 16-bit parts of each little-endian binary64 word. The identity block reproduced the input bits, including signed subnormals. `mulpd` returned signed zeros on the two subnormal inputs and on underflow from both minimum normal inputs.

The regression runs each unchanged arithmetic block on the interpreter and LLVM ORC, with buffer sizes 1, 7, and 64. The four-lane check uses different offsets into the single-voice FlowStone capture on each lane.

`assembler-divpd.asm`, `assembler-minpd.asm`, `assembler-maxpd.asm`, and `assembler-sqrtpd.asm` use the same capture method. The two inputs were captured independently as `assembler-double-math-lhs.f64` and `assembler-double-math-rhs.f64`. The first 44 samples use positive ramps with increments of `2^-40` added to their low bits. The last 20 cover equal values, both signs of zero, infinities, subnormals, the minimum normal values, and quiet and signaling NaNs with distinct payloads.

The FlowStone x64 build ran on Windows on Arm. For ties and unordered comparisons, `minpd` and `maxpd` selected the second operand, including a signaling NaN with its bits intact. Invalid `divpd` produced `0x7ff8000000000000`. Division selected a signaling NaN over a quiet NaN and otherwise selected the first NaN, preserving the selected payload as a quiet NaN. A negative `sqrtpd` operand produced `0xfff8000000000000`, and an input NaN produced its quiet form.

## Double Assembler comparison masks

`assembler-cmppd-lhs.f64` and `assembler-cmppd-rhs.f64` contain the 64 input pairs captured independently in FlowStone Alpha 5483. Samples 40 through 43 compare close doubles such as `1.0` and `1.0 + 2^-40`. Later samples include equal values, both orders of signed zero, signed subnormals, infinities, and quiet and signaling NaNs.

The bench ran `cmppd xmm0,rhs,predicate` with a named right operand for each predicate from 0 to 7. It captured all eight aliases independently using two XMM registers. A separate Assembler compared both 32-bit halves of the double output to zero and to all ones. It sent the analyser 0 for an all-zero word, 1 for an all-ones word, or 2 for any other word. Every capture contained only 0 and 1, and both spellings matched for each predicate.

`assembler-cmppd-masks.f64` stores the reconstructed little-endian 64-bit masks in predicate order, 64 samples per predicate. The interpreter and LLVM ORC tests compare both instruction spellings against these raw words in single-lane and four-lane renders at buffer sizes 1, 7, and 64.

## Double Assembler bitwise operations

The `assembler-andpd.asm`, `assembler-andnpd.asm`, `assembler-orpd.asm`, and `assembler-xorpd.asm` blocks use the same 64 input pairs as the comparison tests. Each computes a `cmppd` mask in `xmm0` and combines it with an original input. The four cases use predicates 5, 1, 3, and 5 respectively, with both named memory and XMM operands. `andnpd` inverts the mask before applying AND.

Each `.f64` output was reconstructed from four captures of 16-bit chunks. The reconstructed words matched independent bitwise operations on the previously captured inputs and masks, including signed zeros, subnormals, and signaling NaN payloads. The regression runs the same four blocks in the interpreter and LLVM ORC, with single-lane and offset four-lane replay at buffer sizes 1, 7, and 64.
## Register-indirect Assembler operands

`assembler-indirect-input.f32`, `assembler-indirect-packed.f32`, and `assembler-indirect-scalar.f32` are 64-sample binary32 captures from FlowStone Alpha build 5483 (64-bit). Signal Analyser captured a ramp starting at −8 with a 0.25 step and the outputs of the two same-named Assembler sources. Both sources use `mov rax,buffer[0]` with a 64-byte mono Mem Create buffer. The packed probe stores four distinct lanes with `movaps` and sums them after loading them back. The scalar probe stores an unaligned word, reads it via x87 `fld`, and adds a value from an indirect integer store, `inc`, and load. OpenStone compares the raw captured words at buffer sizes 1, 7, and 64 in both backends.

## Register-indirect calls

`assembler-call-input.f32` and `assembler-register-call.f32` contain 64 little-endian binary32 samples captured in FlowStone Alpha build 5483 (64-bit). The input ramps from −8 by 0.25 per sample. The probe calls the same local routine twice per sample, applies arithmetic between returns, and adds a persistent counter so a missed call or reset changes the trace.

The captured FlowStone source is `assembler-register-call-flowstone.asm`. FlowStone’s `mov rax,adjust` probe yielded zero on this build. The captured version obtains the local routine address from a direct call’s return address and invokes it with `call rax`. A direct-call control produced the same 64 samples. OpenStone’s `assembler-register-call.asm` binds the same routine by name. The tests compare `call eax` and `call rax` bit for bit with the FlowStone trace at buffer sizes 1, 7, and 64 in both backends.

## Branches inside local subroutines

`assembler-subroutine-loop-flowstone.asm` ran in FlowStone Alpha build 5483 (64-bit). Its 64-sample capture, `assembler-subroutine-loop.f32`, uses the same input ramp as the register-indirect call fixture. The routine runs two iterations on the first call and three on the second. A forward `jz` skips the first loop at zero input, and a backward `jnz` repeats the loop otherwise. The caller adds 1 between the two calls. FlowStone uses `call rax` with the same return-address setup as the register-indirect fixture. `assembler-subroutine-loop.asm` binds the routine by label in OpenStone, which checks both `call eax` and `call rax` bit for bit at buffer sizes 1, 7, and 64 in the interpreter and LLVM ORC.

## Nested local subroutines

`assembler-nested-calls.asm` ran unchanged in FlowStone Alpha build 5483 (64-bit), using the 64-sample input ramp from the call fixtures. The stage calls `outer`, which calls `middle`, which calls `leaf`. Each return resumes arithmetic at a different site. The stage calls the same `leaf` again after `outer` returns. At zero input, a branch skips the two nested calls. The little-endian binary32 trace `assembler-nested-calls.f32` gives `x + 2.75` for nonzero input and `3.5` for zero. OpenStone compares the unchanged source as well as `assembler-nested-register-calls.asm`, which uses `call eax` or `call rax` and saves and restores the target register at each nested call. Both backends compare all output words at buffer sizes 1, 7, and 64.

## Calls to earlier local labels

FlowStone Alpha 5483 (64-bit) produced identical 64-sample traces for `assembler-earlier-direct.asm` and `assembler-earlier-register-flowstone.asm`, using the input ramp from the other call fixtures. The direct probe calls an earlier routine that itself calls an earlier helper twice. The register probe obtains the earlier helper's address from a direct call's return address and invokes it twice with `call rax`. Both render `0.25 * x + 0.875`. OpenStone runs the direct source unchanged and uses `assembler-earlier-register.asm` to bind the earlier helper symbolically for both `call eax` and `call rax`. The interpreter and LLVM ORC compare every sample with `assembler-earlier-calls.f32` at buffer sizes 1, 7, and 64.

## Recursive local subroutines

`assembler-recursive-direct.asm` ran unchanged in FlowStone Alpha 5483 (64-bit). Each input sample supplies a signed counter by truncation. The base case returns for counters at or below zero. Each recursive frame saves its own counter, then halves the running value and adds the saved counter after its child returns. The stage adds 1 after the outermost return. `assembler-recursive-register-flowstone.asm` obtains the local routine's address from a direct call and uses `call rax` to recurse. Both FlowStone probes produced the same 64-sample binary32 trace. OpenStone compares `assembler-recursive-calls.f32` sample for sample for the direct source and `assembler-recursive-register.asm`, with both register spellings, at buffer sizes 1, 7, and 64 in the interpreter and LLVM ORC.

## Branching recursion

`assembler-fibonacci-direct.asm` ran unchanged in FlowStone Alpha 5483 (64-bit). It adds 8 to the input ramp, then truncates to cover integer inputs 0 through 15 with four samples per value. Each non-base frame saves its input and the first result while it makes the two calls needed to compute `F(n-1) + F(n-2)`. `assembler-fibonacci-register-flowstone.asm` uses native `call rax` and produced the same 64 output words. OpenStone compares `assembler-fibonacci.f32` bit for bit with the unchanged direct source and both register spellings in `assembler-fibonacci-register.asm`. Both backends run at buffer sizes 1, 7, and 64.

## Early returns in local subroutines

`assembler-fibonacci-early-return-direct.asm` uses a separate `ret` in the Fibonacci base case. A forward `jg` jumps past it to the two recursive calls, which end at another `ret`. FlowStone Alpha 5483 (64-bit) ran the direct source unchanged and the native `call rax` source in `assembler-fibonacci-early-return-register-flowstone.asm`. Both produced the same 64 binary32 samples as the original Fibonacci probe, covering inputs 0 through 15. OpenStone compares every word of `assembler-fibonacci-early-return.f32` from the direct source and both register spellings in `assembler-fibonacci-early-return-register.asm`, using buffer sizes 1, 7, and 64 in the interpreter and LLVM ORC.

## Tail jumps between local subroutines

`assembler-tail-calls.asm` ran unchanged in FlowStone Alpha 5483 (64-bit), using the 64-sample input ramp from the call fixtures. One routine adds 1 before a backward tail jump. Another adds 0.25 before a forward tail jump. The target routines also run as ordinary calls, and the zero-input sample takes a direct return instead of the backward jump. The caller's arithmetic between and after calls makes each continuation observable. OpenStone compares every output word to `assembler-tail-calls.f32` in the interpreter and LLVM ORC at buffer sizes 1, 7, and 64.

## Register tail jumps

FlowStone Alpha 5483 (64-bit) ran `assembler-register-tail-calls-flowstone.asm` with native `jmp eax`. The target address comes from a direct call's return address, as in the register-call fixtures. FlowStone rejects the `jmp rax` spelling in this probe. The labeled-jump control, `assembler-register-tail-control.asm`, produced identical output words. The 64 binary32 samples in `assembler-register-tail-calls.f32` use the input ramp from the other call fixtures. The stage calls `outer` twice with arithmetic between and after calls. Each target `ret` resumes the original stage caller. OpenStone's `assembler-register-tail-calls.asm` binds the target symbolically and uses `jmp eax`. Tests also use the `rax` spelling. Both backends compare every sample to the native FlowStone capture at buffer sizes 1, 7, and 64.

## Runtime selection of local targets

FlowStone Alpha 5483 (64-bit) ran `assembler-runtime-call-flowstone.asm` and `assembler-runtime-tail-flowstone.asm` using the 64-sample input ramp from the other call fixtures. Branches select one routine for negative inputs and another for zero and positive inputs. The native probes obtain both target addresses from local return addresses and copy the selected address into `rax`. The call probe uses `call rax`. The tail probe uses `jmp eax` inside a routine whose target `ret` resumes the original stage caller. Both traces give `0.25 * x + 0.625` for negative inputs and `x` otherwise, including arithmetic after the return.

OpenStone binds the same targets by label in `assembler-runtime-call.asm` and `assembler-runtime-tail.asm`. Tests compare every word of the separate `assembler-runtime-call.f32` and `assembler-runtime-tail.f32` captures in the interpreter and LLVM ORC, at buffer sizes 1, 7, and 64, using both `eax` and `rax` spellings. Headless regressions also cover three-way selection, saved stack targets, labels returned from different routines, and invalid incoming paths.

## Local targets saved in memory

FlowStone Alpha 5483 (64-bit) ran `assembler-memory-call-flowstone.asm` and `assembler-memory-tail-flowstone.asm`. Branches choose one of two native routine addresses, store it in `saved`, and clear `eax`. The probes reload `rax` from `saved` before `call rax` or `jmp eax`. The tail target's `ret` resumes the original caller, which performs further arithmetic. Both captures use the 64-sample ramp from the other call fixtures.

OpenStone binds the local labels symbolically in `assembler-memory-call.asm` and `assembler-memory-tail.asm`, storing portable target identifiers in named local memory. Both `eax` and `rax` variants compare every word of `assembler-memory-call.f32` and `assembler-memory-tail.f32` in the interpreter and LLVM ORC at buffer sizes 1, 7, and 64. Headless regressions cover packet lanes, local arrays, computed table indices, numeric and SIMD overwrites, x87 stores, and paths that leave a target cell unset.

## Calls and tail jumps through memory operands

FlowStone Alpha 5483 (64-bit) ran `assembler-indirect-call-flowstone.asm` and `assembler-indirect-tail-flowstone.asm`. Each probe receives a 64-byte Mem Create buffer. Branches choose a native local routine address and save it in the first cell. The address register then points to that cell. The probes use `call [eax]` and `jmp [eax]` directly. The target's `ret` resumes the caller, which adds 1 and halves the result. Both captures contain 64 binary32 samples from the same input ramp as the other call fixtures.

OpenStone's `assembler-indirect-call.asm` and `assembler-indirect-tail.asm` save portable local identifiers in the connected Mem. Both `eax` and `rax` variants compare every output word with `assembler-indirect-call.f32` and `assembler-indirect-tail.f32` in the interpreter and LLVM ORC at buffer sizes 1, 7, and 64. Headless checks also cover address-register preservation, copied pointers, offset cells, numeric branch spans, overwritten or unset targets, and recovery after empty or short Mem inputs.

## Indexed memory targets

FlowStone Alpha 5483 (64-bit) ran `assembler-indexed-call-flowstone.asm` and `assembler-indexed-tail-flowstone.asm` with `call [eax+ebx*4]` and `jmp [eax+ebx*4]`. The three native routine addresses occupy bytes 0, 8, and 16 of a Mem Create buffer, with padding for 64-bit pointers. Runtime indices 0, 2, and 4 select those entries using the requested scale of 4. Negative, zero, and positive samples choose different routines. The caller adds 1 and halves the result after returning.

OpenStone's `assembler-indexed-call.asm` and `assembler-indexed-tail.asm` use the same cell offsets and runtime indices with portable local identifiers. Every word of the two 64-sample captures, `assembler-indexed-call.f32` and `assembler-indexed-tail.f32`, is compared at buffer sizes 1, 7, and 64 in the interpreter and LLVM ORC, using both `eax` and `rax` base spellings. Headless checks cover invalid offsets, holes containing numeric copies of valid identifiers, short Mem inputs, recovery, explicit register reloads, preserved base/index registers, and relative branches over indexed transfers.

## Indexed writes to jump tables

FlowStone Alpha 5483 (64-bit) ran `assembler-indexed-write-call-flowstone.asm` and `assembler-indexed-write-tail-flowstone.asm`. Each sample selects a local routine from the sign of the input and stores its native address through `mov [eax+ebx*4],rcx;`. Indices 0, 2, and 4 write entries at bytes 0, 8, and 16, leaving padding for FlowStone's 64-bit pointers. A second indexed write replaces entry zero with the selected target. The call probe invokes that entry directly; the tail probe calls a wrapper that jumps through it, whose `ret` must resume the original caller. The caller then adds 1 and halves the result.

As in the preceding native address fixtures, FlowStone binds local addresses by popping the return addresses of setup calls. OpenStone's corresponding `assembler-indexed-write-call.asm` and `assembler-indexed-write-tail.asm` bind labels directly and store portable 32-bit target identifiers in the same byte positions. These are equivalent target-table probes, not identical source or pointer layouts. The input was recaptured and matched `assembler-call-input.f32` byte for byte. Both 64-sample output captures match the independently calculated binary32 results.

The schematic test compares every output word with `assembler-indexed-write-call.f32` and `assembler-indexed-write-tail.f32` in the interpreter and LLVM ORC at buffer sizes 1, 7, and 64, using both `eax` and `rax` base spellings. Additional headless tests cover conditional writes, persistence across buffers, target replacement, uninitialized cells, numeric and aliased overwrites, out-of-bounds writes and transfers, new Mem bindings, explicit register reloads, and loop initialization of unaligned target cells.

## Copying targets between indexed cells

FlowStone Alpha 5483 (64-bit) ran `assembler-indexed-copy-call-flowstone.asm` and `assembler-indexed-copy-tail-flowstone.asm`. The source table holds three local routine addresses at bytes 0, 8, and 16. Each sample selects source index 0, 2, or 4 and a distinct destination index 10, 6, or 8, respectively. `mov rcx,[eax+ebx*4];` loads the selected target, and `mov [eax+edx*4],rcx;` copies it to bytes 40, 24, or 32. The probe then clears the temporary register and the original source cell with numeric zero before calling the copied entry. The tail variant calls a wrapper whose indexed `jmp` must return to the original caller. The caller adds 1 and halves the result.

The native source uses return-address setup calls to bind 64-bit local addresses, as in the preceding fixtures. OpenStone's `assembler-indexed-copy-call.asm` and `assembler-indexed-copy-tail.asm` bind labels directly and copy portable 32-bit target identifiers through `ecx`. The byte positions retain padding for native pointer widths. The sources therefore implement the same probe with different address binding and pointer widths. A fresh input capture matched `assembler-call-input.f32` byte for byte, and both native output captures matched independently calculated binary32 results.

`assembler-indexed-copy-call.f32` and `assembler-indexed-copy-tail.f32` each contain 64 samples. The schematic test compares every output word in the interpreter and LLVM ORC at buffer sizes 1, 7, and 64, using both `eax` and `rax` base spellings. Additional headless checks cover all three targets, copied-entry replacement, self-copies, clearing the source after copying, persistence across buffers, invalid source cells, out-of-bounds source and destination indices, numeric destination overwrites, recovery, and calls through an explicitly reloaded target register.

## Chained copies between indexed cells

FlowStone Alpha 5483 (64-bit) ran `assembler-chained-copy-call-flowstone.asm` and `assembler-chained-copy-tail-flowstone.asm`. Each sample selects one of the three source targets at bytes 0, 8, and 16 and copies it through three indexed destinations. The first destination is selected from bytes 40, 24, and 32; the next two are bytes 48 and 56. Every hop reloads the preceding cell with `mov rcx,[eax+ebx*4];` and stores to `mov [eax+edx*4],rcx;`. It then zeroes the temporary register and the preceding cell before the next hop. The final entry is invoked by indexed CALL or by a wrapper's indexed tail JMP, after which the original caller adds 1 and halves the result.

The native sources bind 64-bit local addresses with return-address setup calls. OpenStone's `assembler-chained-copy-call.asm` and `assembler-chained-copy-tail.asm` bind portable local identifiers directly and use 32-bit `ecx` loads and stores at the same byte positions, with padding for the native pointers. These sources implement equivalent probes with different target binding and pointer widths. A fresh input capture matched `assembler-call-input.f32` byte for byte; both native captures also matched independently calculated binary32 results.

The schematic test compares all 64 output words in `assembler-chained-copy-call.f32` and `assembler-chained-copy-tail.f32` in the interpreter and LLVM ORC at buffer sizes 1, 7, and 64, using both `eax` and `rax` base spellings. The headless indexed-copy sequence also carries a target from cell 6 through cells 8, 10, 12, and 14 on successive samples, clearing each preceding entry, then verifies that only the final entry remains callable. This runs at buffer sizes 1, 3, 7, and 27, testing persistence across render calls with direct calls, tail jumps, and explicit target-register reloads.

## Swapping indexed targets

FlowStone Alpha 5483 (64-bit) ran `assembler-indexed-swap-call-flowstone.asm` and `assembler-indexed-swap-tail-flowstone.asm`. The table holds routine A at byte 0, B at byte 8, and C at byte 16. Negative, zero, and positive samples select the pairs A/B, B/C, and C/A, respectively. The probe saves both indices in integer locals, loads the two original entries into `rcx` and `rdx`, then stores them in opposite indexed cells. Both temporary registers are cleared before either entry is invoked.

Each sample invokes both swapped entries, first the left entry and then the right. Routine A computes `0.5*x + 0.25`, B computes `2*x - 1`, and C computes `x + 4`; the caller then adds 1 and halves the result. This order-sensitive composition distinguishes a successful swap from an unchanged table at every one of the 64 input samples. The tail variant calls a wrapper twice whose indexed JMP must return to the original caller each time.

The native sources use return-address setup calls to bind 64-bit local addresses. OpenStone's `assembler-indexed-swap-call.asm` and `assembler-indexed-swap-tail.asm` bind portable local identifiers directly and use 32-bit temporary registers at the same byte positions, retaining padding for native pointer widths. A fresh input capture matched `assembler-call-input.f32` byte for byte. Both native captures matched the independently calculated binary32 results for the swapped order.

The schematic test compares every word in the two 64-sample captures, `assembler-indexed-swap-call.f32` and `assembler-indexed-swap-tail.f32`, in the interpreter and LLVM ORC at buffer sizes 1, 7, and 64, using both `eax` and `rax` base spellings. Additional headless tests initialize the table once and cover all three pairs, swap-back, self-swaps, persistence across buffers, invalid first and second indices, unchanged entries after rejected swaps, and recovery. These run at buffer sizes 1, 3, 7, and 21 with direct calls, tail jumps, and explicitly reloaded target registers.

## Numeric jump displacements

The four `assembler-jmp-*.asm` and `assembler-jnz-*.asm` blocks ran in FlowStone Alpha build 5483 (64-bit) against the 64-sample ramp in `assembler-jump-input.f32`. FlowStone's listing showed five-byte JMP and six-byte JNZ encodings with signed relative displacements. Its corresponding little-endian binary32 captures cover a seven-byte SSE skip, a two-byte conditional skip with both outcomes, and counted loops with `jmp -23` and `jnz -18`. OpenStone runs the same source and checks each output word at buffer sizes 1, 7, and 64 in the interpreter and LLVM ORC.

## Backward label branches

`assembler-label-jmp.asm` and `assembler-label-jnz.asm` ran unchanged in FlowStone Alpha build 5483 (64-bit), with the same input confirmed by a fresh capture. The JMP loop adds 0.5 per iteration and exits on a forward JZ. It runs twice on negative samples and four times otherwise. The JNZ loop applies `x = 0.5 * x + 0.5` each pass and runs once at zero and twice for every other sample. The two little-endian binary32 output captures contain 64 samples each. OpenStone compares every output word in both backends at buffer sizes 1, 7, and 64.

## SSE2 double conversions and shuffle

These four sources are unchanged null-terminated kernel bodies recovered from the Alpha 5483 x64 executable. The containing executable SHA-256 is `f4dbec8a34ba6fb7b8e91ed22038b69d081b94e275562c80d7ca9815a0b3832e`. Each fixture preserves the original source bytes, including the absence of a trailing newline.

| Source fixture | Executable byte offset | Source SHA-256 |
|---|---|---|
| [assembler-double-round.asm](assembler-double-round.asm) | `0x58d4e0` | `785141e572046e6d7ac7867bb470eb399463c1f11dd237dc031f10f5c24ad4e9` |
| [assembler-double-truncate.asm](assembler-double-truncate.asm) | `0x58d1c0` | `0f2c1d9e23f70282f5f0f244552055029cc25f7ef41ad77420cb4267a3e3b3f6` |
| [assembler-double-pack.asm](assembler-double-pack.asm) | `0x586d90` | `84b772250b94f2920b93cc565c5cf8f91877a6b5d7793a1c39103b11ea3a1da4` |
| [assembler-double-unpack.asm](assembler-double-unpack.asm) | `0x595950` | `f79d7258b05f71f53cc77bb4fa2e36a3be5ece38ff153f03733cf1eb7cf86bc8` |

`assembler_sse2` renders the unchanged sources through the interpreter and LLVM ORC at buffer sizes 1, 7, 31 and 128. Round and Truncate check all four double lanes against explicit integer results. Pack checks four input streams in lane order; Unpack checks the first scalar lane of each of its four output streams. Independent instruction tests exercise nearest-even and toward-zero rounding, values distinguishable only in double precision, signed integer boundaries, NaNs/infinities, register aliases, named and indexed memory, Mem reference reads, all low-bit shuffle choices, ignored high immediate bits and numeric jump widths. Raw shuffle checks include signed zeros, signaling/quiet NaNs and subnormals. A build with LLVM disabled runs the same tests through the interpreter.

The expected results follow the [Intel SSE2 conversion specification](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-2a-manual.pdf#page=337) and [shuffle specification](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-2b-manual.pdf#page=619). New FlowStone numerical captures for these four kernels have not been collected. The fixtures are ready for community testing on Windows x64.

## Raw XMM views and unchanged comparison/switch kernels

These six unchanged sources come from the same Alpha 5483 x64 executable identified above. Each fixture retains its original bytes.

| Source fixture | Executable byte offset | Source SHA-256 |
|---|---|---|
| [assembler-double-equal.asm](assembler-double-equal.asm) | `0x578d20` | `02e6b26e479d69f496a51cc111dbb63d46d75250b4727035b843a92c1ee81666` |
| [assembler-double-less.asm](assembler-double-less.asm) | `0x578a20` | `0b879cd07047e4cb7bec40b83a90909aab84cc37763ba3c0a68ba9034d369b59` |
| [assembler-double-less-equal.asm](assembler-double-less-equal.asm) | `0x578ba0` | `64bf85ef4724b9a8c7dfff3756f302b4975737e451b161e798b67883a5595561` |
| [assembler-double-greater-equal.asm](assembler-double-greater-equal.asm) | `0x578c60` | `fb2ac91f4e56a53ad7af400b7af9bad23899087cbf576f9d441a6147b849c83c` |
| [assembler-double-greater.asm](assembler-double-greater.asm) | `0x578ae0` | `1fe49f44d9438095d52c753f253b6510bc38a030e20f5633678ff904210e9a75` |
| [assembler-double-switch.asm](assembler-double-switch.asm) | `0x58f560` | `59c03fa9caef117ea21454f7afc604dbb9c9cd2ac5d771a5081b21a1d9050af4` |

`assembler_raw_xmm` compares every output bit with an independent scalar/raw-word oracle in the interpreter and LLVM ORC across buffer sizes 1, 3, 7, 31 and 64. The cases include double-precision differences of `2^-40`, infinities, payload-preserving signaling/quiet NaNs, signed zeros, subnormals, every comparison predicate used by these kernels, and per-lane switch selection. The comparison sources declare a Double outlet and issue a 128-bit `movaps` store: the oracle checks four DWORD masks packed into the first two Double words; the other two Double words retain their initialized zeros. The switch oracle checks all four selected Double words directly.

The suite also checks register and named-memory PS/PD round trips, `addps` on PD-loaded words and `addpd` on PS-loaded words, partial `movss`/`movd` writes, Double array byte offsets and guards, scalar descriptor reads, Boolean true/false transitions, bounded short-array accesses, mixed-view branch joins/backward loops/local calls, and overwritten saved-target invalidation. Four-lane Double packets use two physical XMM windows. The previous numeric-branch test now checks the two Double elements written by its single 128-bit store; former width-mismatch rejection cases are covered by positive raw-view tests.

XMM view changes follow the [Intel 128-bit move contract](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-2b-manual.pdf#page=47). The kernel files are original recovered FlowStone sources; their expected outputs are specification/reference-model results, with new FlowStone Windows x64 captures pending community testing.

The older scalar FlowStone `.f64` captures remain byte-for-byte unchanged. Their single-window instruction bodies now run as two pairwise replays in the packed regression: input rotations 0/17 and 34/51. Each replay checks both active Double lanes against the saved words and checks that the untouched upper output window stays zero. This retains all four independent reference rotations while enforcing the physical two-Double XMM width. The scalar editor captures still run the original source directly.

## Connector declaration grammar

These two serialized Assembler sources retain the exact bytes recovered from the same Alpha 5483 x64 executable identified above, including their comma-separated connectors.

| Source fixture | Executable byte offset | Source SHA-256 |
|---|---|---|
| [assembler-interpolation-weights.asm](assembler-interpolation-weights.asm) | `0x7b9027` | `9bfd818d18729997c488a6c513fe0f93392478809399d8cab55144a284550455` |
| [assembler-double-accumulator.asm](assembler-double-accumulator.asm) | `0x7b9639` | `1f58b42f4a9d970d19837717fa5ef426fbc61b36bcf1c3202add1a6317189d22` |

`dsp_declarations` runs both unchanged sources in the interpreter and LLVM ORC at buffer sizes 1, 3, 7, 31 and 64. Interpolation tests include positive and negative fractional positions and check all four outputs in declaration order. The accumulator test uses four independent step/length pairs, double-only `2^-40` increments, wraparound and the source’s final conversion to Float. Expected samples come from independent scalar models; these are recovered sources with reference-model tests, not new FlowStone output captures.

The suite also checks comma-separated DSP Code/Assembler inputs and outputs, independent array capacities and Mem/MemArray bindings, a variable or connector named `stage`, real stage blocks alongside parenthesized lane assignments, Assembler local-call and numeric-jump scanning, persistent state across buffers, and source locations for malformed or duplicate declarations. It also runs in interpreter-only builds.

## Greater-than comparison aliases

`cmpgtps` and `cmpgtpd` deliberately use SSE predicate 6 (not-less-than-or-equal / greater-than-or-unordered), matching FlowStone Alpha 5483's alias registration. They produce a full all-ones DWORD/QWORD when either operand is NaN. Equal operands, including positive and negative zero in either order, produce zero. They retain the existing SSE comparison rule that flushes subnormal operands before comparing. This differs from an ordered greater-than operation.

Static evidence comes from the same x64 `FlowStone.exe`, SHA-256 `f4dbec8a34ba6fb7b8e91ed22038b69d081b94e275562c80d7ca9815a0b3832e`, used by the compile audit. The following are virtual addresses at image base `0x140000000`; they can be inspected with `llvm-objdump -d --x86-asm-syntax=intel`.

| Name | Name registration reference | Registered emitter |
|---|---|---|
| `cmpgtps` | `0x14025c775` | `0x14028a2c0` |
| `cmpnleps` | `0x14025c7cd` | `0x14028a2c0` |
| `cmpgtpd` | `0x14025cb2a` | `0x14028a2b0` |
| `cmpnlepd` | `0x14025cb7f` | `0x14028a2b0` |

Both emitter paths set the immediate-control cell at `0x140706a70` to 6. The PS handler emits `0F C2 /r 06`; the PD handler selects 6 and jumps into the common PD encoder at `0x14028a400`. This is static evidence for the alias mapping, rather than a newly captured FlowStone output trace.

`assembler_raw_xmm` has 29 explicit raw-input/expected-mask cases per precision, rotated independently through all four lanes. The suite checks register, named-memory, byte-indexed and Mem-indirect sources, uppercase mnemonics, self-comparisons, unchanged source-register payloads, both 128-bit Double windows and numeric jumps over register/memory forms using high XMM registers. Each result must be exactly `0xFFFFFFFF` / `0xFFFFFFFFFFFFFFFF` or zero, and must also match explicit predicate 6 and the existing NLE alias. Cases include positive/negative quiet and signaling NaNs on either side, all signed-zero pairings, equal and unequal finite values, infinities, subnormals and differences of `2^-40` in Double inputs. A memory destination and a third operand remain errors; initially unwritten XMM destinations now use the documented incoming bank. Tests run through the interpreter and LLVM ORC at buffer sizes 1, 3, 7, 31 and 64, and also run in interpreter-only builds. Native FlowStone Windows x64 mask captures remain pending community testing.

## Scalar COMISS flags and branches

`assembler_comiss` uses the [Intel COMISS flag table, section 5-84](https://www.intel.com/content/dam/develop/external/us/en/documents/319433-024-697869.pdf) as its reference. It compares raw DWORD zero of each operand as Float. The expected ZF/PF/CF triples are 000 (greater), 001 (less), 100 (equal), and 111 (unordered); positive and negative zeros are equal. The signed-less state clears with SF/OF. Subnormals use OpenStone's existing SSE flush policy. These are specification/reference-model results, not new FlowStone numerical captures.

The suite supplies 24 literal raw input/expected-flag cases, including NaNs of both signs and both quiet/signaling forms, signed zeros in all pairings, infinities, adjacent finite values and subnormals. Contradictory upper lanes, including signaling NaNs, verify that only the low Float word controls the whole packed branch. Each case runs through six operand forms (register, named memory, byte-indexed memory, a nonzero constant lane offset, Mem-indirect memory, and self-comparison) in the interpreter and LLVM ORC across buffer sizes 1, 3, 7, 31 and 64. Both operands are checked bit for bit afterward. The same suite runs without LLVM.

Sixteen branch spellings are checked: `je`, `jz`, `jne`, `jnz`, `ja`, `jb`, `jbe`, `jae`, `jp`, `jnp`, `jg`, `jl`, `jng`, `jnl`, `jge`, `jle`. Signed branches retain EFLAGS semantics: after COMISS, `jg` requires ZF=0, so it also takes the ordered less-than case. `ja` is the floating greater-than branch. Additional cases cover raw Float words in Double-loaded XMM registers, local returns, branch joins, loops that redefine flags, numeric skips over high-register and named-memory COMISS encodings, zero-count shifts and INC's preserved carry.

Carry and parity must have producers on every incoming path. Moves, packed comparisons and push/pop preserve them; INC preserves carry and zero-count shifts preserve all modeled flags. Integer producers are covered below. The COMISS suite retains specific diagnostics for unwritten/invalid operands, wrong arity, missing producers, mixed valid/invalid incoming paths, opaque address arithmetic and cross-stage flag leakage. Its former rejection cases after numeric CMP/ADD/INC are replaced by positive integer-flag tests. x86 SIMD exception-status/trap emulation remains a separate gap. New native FlowStone Windows x64 traces remain pending community testing.

## Integer carry and parity flags

`assembler_integer_flags` uses the [Intel status-flag definitions](https://cdrdv2-public.intel.com/843827/253665-sdm-vol-1-dec-24.pdf) and [INC flag-preservation contract](https://cdrdv2-public.intel.com/868140/253666-089-sdm-vol-2a.pdf). For 32-bit numeric operands, ADD carries out of bit 31; SUB/CMP borrow when the unsigned left operand is smaller. Parity is even popcount of only the wrapped result's low byte. CMP changes flags without storing its result. INC changes PF/ZF and signed-branch state but preserves carry. These are independent reference-model tests, not new native FlowStone output captures.

The oracle uses a widened unsigned sum, unsigned borrow, wrapped arithmetic, sign-bit overflow formulas and byte popcount. It checks 744 pairs per operand form: 20 boundary pairs, every possible result byte with contrasting high bits, and 96 deterministic mixed-word pairs, each repeated with incoming carry both clear and set. Boundaries distinguish unsigned carry from signed overflow (`0xFFFFFFFF+1` versus `0x7FFFFFFF+1`), signed underflow, zero wraparound and borrow. Sixteen branch spellings expose zero, unsigned, parity and signed relations; raw output words check wrapping, unchanged CMP operands and preserved sources. Contradictory packed upper lanes check scalar addressing.

CMP/ADD/SUB run with register, named-memory, immediate and aliased-register operands. INC runs in a register, a named local and a Mem-indirect cell. Each program renders in both the interpreter and LLVM ORC at buffer sizes 1, 3, 7, 31 and 64. The suite also checks flags across calls, mixed-producer joins, counted loops, COMISS overwrites and INC with no incoming carry producer. Additional cases verify AND/OR's cleared carry and SHL/SHR's last-shifted-bit carry and byte parity, including shift count 31. It also passes in an interpreter-only build.

The interpreter computes carry with widened integer arithmetic and parity with byte popcount. LLVM uses wrapping i32 addition plus unsigned comparison for ADD, unsigned comparison for borrow, and masked `ctpop` for parity. Arithmetic aliasing cannot change operands before carry is captured. INC's preserved carry retains its availability state; a carry branch without a producer is rejected. Opaque Mem-reference null checks/address arithmetic do not expose numeric address flags. Undefined multi-bit shift overflow, raw auxiliary/overflow flag access and SIMD exception/trap emulation are outside this batch. Native FlowStone Windows x64 traces remain pending community testing.

## DEC flags and branches

The integer flag suite now also covers `dec`, following the [Intel DEC contract (Volume 2A, section 3-262)](https://cdrdv2-public.intel.com/868140/253666-089-sdm-vol-2a.pdf). It subtracts one and updates zero, even low-byte parity and signed-branch state, while preserving carry. The key literal cases are `0 -> 0xFFFFFFFF` (wraparound, negative, even parity), `1 -> 0` (zero, even parity), and `0x80000000 -> 0x7FFFFFFF` (signed overflow, so SF xor OF is true). Each keeps incoming CF unchanged.

`assembler_integer_flags` applies the same 744-case boundary/byte/random corpus to DEC in a register, named local and Mem-indirect cell, with incoming carry both clear and set. All 16 branch spellings and raw result words are checked in the interpreter and LLVM ORC across buffer sizes 1, 3, 7, 31 and 64. Every low-byte parity appears, and packed upper lanes remain irrelevant to scalar arithmetic. DEC without a carry producer can branch on parity, but a carry branch remains an error.

Additional checks exercise a local subroutine, mixed register/memory branch joins, a three-iteration DEC/JNZ loop followed by a final decrement, COMISS-provided carry in both states, uppercase spelling, numeric jumps over the two-byte register and six-byte named-memory encodings, and invalid or unwritten operands. The suite also runs without LLVM. Expected results come from independent wrapped-word and overflow formulas, not new FlowStone captures. The recovered DEC-containing sources retain their original earlier engine-context failures in the compile audit; native FlowStone Windows x64 traces remain pending community testing.

## Signed JGE and JLE branches

`jge` aliases `jnl` (SF=OF), and `jle` aliases `jng` (ZF=1 or SF differs from OF), following the [Intel Jcc conditions](https://cdrdv2-public.intel.com/868140/253666-089-sdm-vol-2a.pdf). They read signed flags without modifying them. Their conditions therefore include overflow; comparing the wrapped register value with zero would be incorrect.

`assembler_integer_flags` now checks both names after CMP, ADD, SUB, INC and DEC throughout its 744-case operand matrix at buffer sizes 1, 3, 7, 31 and 64 in the interpreter and LLVM ORC. The matrix has 16 branch outputs plus raw result/source checks. Expected conditions use independently calculated sign, zero and overflow bits. Both incoming carry states verify that the signed branches do not use or change CF.

Sixteen additional literal cases independently check equality, signed-limit comparisons and positive/negative arithmetic overflow. These include `0x7FFFFFFF+1`, `0x80000000-1`, INC/DEC at the signed limits, and `0x80000000+0x80000000`: its wrapped zero result has OF set, so JGE is false and JLE true. Uppercase spellings run in the same checks. Additional programs use each alias in a backward loop, preserve overflow flags across a local return, and skip their six-byte native branch encodings with numeric JMP spans. Missing flag producers remain errors.

The COMISS suite also checks the two aliases: SF/OF clear makes JGE true, and JLE follows ZF, including unordered inputs. Both suites pass without LLVM. These are specification/reference-model results, not new native FlowStone captures. The compile audit preserves recovered source unchanged and still reports earlier engine bindings/incoming-register dependencies where present; native FlowStone Windows x64 traces remain pending community testing.

## Scalar CVTSS2SI conversion

The audit identifies CVTSS2SI in the x86/x64 Poly-selection engine strings (`5b3dd49653d6` and `b9f32bc74809`). These strings retain their earlier `__const` engine-binding failures. The implementation adds the standalone 32-bit form, following the [Intel SSE conversion reference and Appendix D invalid-result table](https://cdrdv2-public.intel.com/874240/325462-090-sdm-vol-1-2abcd-3abcd-4.pdf) and OpenStone's existing nearest-even/denormal-flush policy. An invalid conversion returns `0x80000000`. The instruction reads a low Float DWORD, preserves the source and modeled EFLAGS, and replaces any pointer/target tag in its integer destination.

`assembler_scalar_convert` checks 42 literal raw Float/result pairs: signed zeros, positive/negative half ties and adjacent Float values, integers past binary32's consecutive-integer range, both signed-32 limits and their neighboring Floats, infinities, maximum Floats, signed payload quiet/signaling NaNs, subnormals and minimum normal values. Each input appears under three earlier CMP flag patterns in a 131-sample sweep. Contradictory upper lanes prove scalar behavior. Raw result words preserve the exact integer result without a Float display conversion.

Six forms cover low/high XMM sources, named memory, byte-indexed memory with the index register also the destination, Mem-indirect memory with the base register also the destination, and a local array cell. Together they write EAX, EBX, ECX and EDX. Every source packet is checked bit for bit afterward, and carry/parity/zero/signed-branch outputs must retain the earlier CMP state. Both backends run buffer sizes 1, 3, 7, 31 and 64 under an intentionally different caller rounding mode; processing must restore that mode and the caller's exception flags.

Further cases read a Float DWORD from a Double-loaded XMM register whose numerical Double value would round differently, return a converted GPR through a local subroutine, and skip known four-/five-byte register and eight-byte named-memory encodings with numeric JMP. Invalid destinations, 64-bit integer destinations, unwritten XMM sources, wrong arity, using conversion as an EFLAGS producer, and stale pointer/local-target provenance are rejected. The suite also passes without LLVM. These are specification/reference-model tests, not new native FlowStone captures. Mutable MXCSR rounding and SIMD exception-status/trap emulation remain outside this pass; native FlowStone Windows x64 traces remain pending community testing.

## Raw XMM half moves

MOVHLPS copies source bits 127:64 to destination bits 63:0 while keeping the destination high half. MOVLHPS copies source bits 63:0 to destination bits 127:64 while keeping the destination low half. Both legacy forms require two XMM registers and perform no floating-point arithmetic. The reference is the [Intel instruction manual, MOVHLPS and MOVLHPS entries](https://cdrdv2-public.intel.com/782156/325383-sdm-vol-2abcd.pdf). Self-aliasing duplicates the original high or low half respectively, and EFLAGS stay unchanged.

`assembler_raw_xmm` checks 131 input samples at buffer sizes 1, 3, 7, 31 and 64 in both the interpreter and LLVM ORC. Each instruction uses six register pairings: low/low, low/high, high/low, high/high, self/low and self/high. The exact raw-word oracle checks the moved half, preserved half and distinct source. Inputs rotate signed zeros, subnormals, infinities, quiet/signaling NaN payloads and arbitrary Float words. Double-loaded registers include precise finite values, NaNs and subnormals; both 128-bit windows of the four-lane Double signal are checked, without changing the instruction's raw DWORD interpretation. Carry, parity, zero and signed branches must retain their earlier CMP flags.

Additional cases check local calls/returns, numeric jumps over three-byte low-register and four-byte REX encodings, uppercase spelling, invalid memory/GPR/immediate operands, wrong arity, unwritten register paths and missing EFLAGS producers. The suite also passes in an interpreter-only portable build. The three recovered reduction engine strings (`141bdaedf239`, `4c7771f92c74`, `6c01a855e0c4`) remain unchanged and retain their earlier incoming-`xmm0` audit failures. These are specification/reference-model results, not new native FlowStone numerical captures; Windows x64 community verification remains pending.

## Scalar TEST and XOR flags

TEST computes a logical AND for its flags without storing the result. XOR writes the exclusive OR to its first operand. Both clear CF and OF, set ZF from the zero result, SF from its sign bit and PF from even parity of its low byte; AF is undefined. The references are Intel's [TEST operand and operation table](https://cdrdv2-public.intel.com/825743/325462-sdm-vol-1-2abcd-3abcd-4.pdf) and [XOR flags entry](https://cdrdv2-public.intel.com/929356/334569-093-sdm-vol-2d.pdf). These tests target the existing 32-bit numeric register/memory model.

`assembler_integer_flags` applies eight operand forms per instruction to its 744-case corpus: boundary pairs, all 256 low bytes with contrasting upper bits and 96 deterministic mixed-word pairs, each with both incoming CF states. OF is deliberately set before the logical instruction in both paths. Every one of the 16 conditional branch spellings is checked, together with exact raw result words and unchanged sources. Register, immediate, named memory, self-register and indexed Mem forms run in the interpreter and LLVM ORC at buffer sizes 1, 3, 7, 31 and 64. Self-TEST checks every low-byte parity without changing its operand; self-XOR must produce zero, ZF/PF set and cleared signed/carry state.

Further checks cover local calls/returns, mixed TEST/XOR joins, a TEST-controlled counted loop, uppercase spelling, known two-/six-/ten-byte numeric spans, zeroing all four unwritten GPRs, partly initialized incoming paths and clearing old pointer/target tags. TEST leaves a saved local target cell callable; even identity XOR writes invalidate saved target tags. Invalid or unwritten operands, two-memory forms, 64-bit numeric registers, stale pointer/target use and cross-stage flags remain errors. TEST EAX with an immediate has both accumulator and ModRM encodings; numeric spans over that form are left unknown until FlowStone's choice is verified. The suite passes in an interpreter-only portable build as well. The audit's synthetic unknown-instruction fixture now uses `vbadps`, verifying that TEST/XOR are recognized and compile before the intended failure.

The recovered TEST/XOR sources remain unchanged and retain their earlier engine-context failures. These are specification/reference-model checks, not new native FlowStone numerical captures; Windows x64 community verification remains pending.

## Scalar XCHG swaps

XCHG exchanges a register with another register or a memory operand and leaves EFLAGS unchanged, following the [Intel XCHG reference](https://cdrdv2-public.intel.com/929356/334569-093-sdm-vol-2d.pdf). Both original values must be captured before either write. OpenStone writes the memory operand before replacing its possible index register, preserving the original effective address. Numeric swaps use the existing 32-bit model; opaque memory references and local routine identifiers retain their metadata during register exchanges.

`assembler_exchange` checks all 16 pairings of EAX/EBX/ECX/EDX, including each self-swap, and six memory forms in both operand orders. The 131-sample corpus includes zero, signed limits, arbitrary integer words, signed zeros, subnormals and quiet/signaling NaN bit patterns with contradictory upper input lanes. Named scalar, fixed/indexed local arrays, indirect Mem and scaled Mem indices are checked as raw DWORDs, with surrounding guard cells. Cases where the swapped register is also a memory index replace it with a very different value, detecting an incorrectly recomputed store address. Both backends run buffer sizes 1, 3, 7, 31 and 64.

Six literal incoming flag states (CMP less/equal/greater, two ADD overflows and COMISS unordered) check every one of the 16 conditional branch spellings. Additional checks exercise reference offsets, RAX/RBX opaque reference swaps, self-swapping a callable target, exchanging local targets with each other or with numeric data, and preserving target tags through named and runtime-indexed Mem cells. Loaded targets remain callable after the old cell receives ordinary data; replacement data loses old callable tags. Calls/returns preserve flags; branch/loop value tests deliberately reseed flags after their own flag-writing control instructions. Known two-byte non-accumulator register and six-byte named-memory numeric spans are checked. The same suite passes in an interpreter-only portable build.

Invalid/unwritten or mixed-width operands, XMM/immediate/two-memory forms, input/descriptor writes, raw numeric 64-bit forms, storing opaque Mem references into bytes, stale pointer/target use and missing flag producers remain errors. Unlike self-XOR, self-XCHG requires an initialized register. Accumulator exchanges have both short and ModRM encodings, so numeric spans over those forms remain unknown until FlowStone's encoding is verified. Native bus-lock semantics and arbitrary native pointer serialization are not modeled by this single-renderer portable exchange.

The recovered XCHG sources remain byte-for-byte unchanged. Their EBP/RBP engine-frame context remains a separate gap and their earlier audit diagnostics are retained. These are specification/reference-model results, not new native FlowStone numerical captures; Windows x64 community verification remains pending.

## Signed IDIV and divide faults

The reference is [Intel IDIV, Volume 2A sections 3-441 through 3-443](https://cdrdv2-public.intel.com/868140/253666-089-sdm-vol-2a.pdf). A signed 64-bit EDX:EAX dividend and signed 32-bit divisor produce a signed 32-bit quotient in EAX and remainder in EDX. Rounding is toward zero; the remainder has the dividend's sign. A zero divisor or quotient outside signed-32 range faults, and arithmetic flags are undefined after a completed divide. OpenStone rejects later flag branches until another instruction defines the required flags.

The interpreter uses unsigned dividend/divisor magnitudes and explicit quotient limits, including INT64_MIN without signed overflow. LLVM independently constructs the i64 dividend and emits signed division/remainder, sanitizing zero and INT64_MIN/-1 operands before evaluation so speculative or optimized evaluation cannot create poison. A guarded success path writes both result registers; a fault path records the reason and returns from the stage without committing either register. `Runtime::dspFault(block)` returns the first reason and absolute frame since reset. This diagnostic does not change buffer-validation success, roll back earlier instructions or prevent later valid samples from running. It models a divide fault without delivering a native hardware exception to the host.

`assembler_divide` supplies 34 literal boundary/sign cases, 128 deterministic full-width mixed cases and seven trailing valid samples. Literal checks pin all four operand-sign combinations. Expected results use independently guarded C++ signed arithmetic plus dividend = quotient*divisor + remainder and sign/magnitude checks. Cases include positive/negative fractional results, both signed-32 limits and neighboring invalid quotients, non-sign-extended high words, dividends beyond binary64's consecutive-integer range with exact boundary quotients and remainders, zero divisors and INT64_MIN/-1.

Fifteen program variants cover EBX/ECX, named and fixed/indexed local memory, indirect/scaled Mem, EAX/EDX divisor aliases, both implicit registers as memory indices, descriptor channel counts, a local subroutine and two-/six-byte numeric skips. Every sample is checked as raw result words at buffer sizes 1, 3, 7, 31 and 64 in both backends, with contradictory upper lanes and caller rounding/exception-state preservation. Faulting samples retain prewritten output sentinels and skip following instructions, even from a nested caller; valid samples afterward recover. A separate direct-stage test confirms that faulting divisions leave all register/divisor/marker slots untouched and report the precise reason/frame. A counted-loop test and invalid/unwritten operand and undefined-flag tests cover control flow. The suite also passes in an interpreter-only portable build.

The original Mem-reader source `17a241c5e13c` is unchanged and still requires its incoming register context. Its byte MOV forms are covered by the following batch. No new native FlowStone captures were created. These are specification/reference-model tests; native Windows x64 behavior and trap delivery remain for community verification. The audit's missing-mnemonic census is now empty, while operand forms, engine context and floating-point details remain separate compatibility work.

## Low-byte MOV registers

AL/BL/CL/DL address bits 7:0 of EAX/EBX/ECX/EDX. MOV replaces just that byte and preserves the other parent bits and EFLAGS. The native width rule is described in [AMD's general-register diagram and write-width rules](https://www.scs.stanford.edu/~zyedidia/docs/x86/amd-manual-v3.pdf). This pass applies the rule inside OpenStone's existing 32-bit numeric register model. Numeric parents must already be initialized; opaque pointer/target parents cannot be partially accessed. Byte stores and high-byte aliases were added in the following batches; further byte instructions are tracked in the batches below.

`assembler_byte_register` checks byte loads from Mem, packed inputs, local 32-/64-bit arrays and FloatArray ports, plus every low-byte register copy pairing and self-copy. Its 259-sample sweep includes every byte value, the final byte of a 257-byte allocation, empty memory, negative offsets, one-past-end offsets, unaligned bytes and raw NaN/subnormal payload bytes. The oracle masks only the old low byte and supplies a separately selected expected byte; upper 24 bits and unchanged source words are checked exactly. All 16 conditional branches retain six earlier CMP/overflow/COMISS flag states. Both backends run buffer sizes 1, 3, 7, 31 and 64, with caller rounding-mode/exception-state checks.

Separate cases observe signed/unsigned immediates (-128, -1, 128, 255 and zero), an ECX address index changed by its own CL load, and known two-/six-byte numeric skips. Invalid/unwritten and mixed-width operands, opaque parents, missing flags and unsupported byte instructions fail explicitly. The same suite passes in an interpreter-only portable build.

[mem-reader-byte.asm](mem-reader-byte.asm) is an exact CRLF-preserved copy of recovered source SHA-256 `17a241c5e13c219bafc062119cb32775a88f88beaa1d363133fff81376789a64`. The harness prepends only `mov edx,0;mov ecx,0;` as an explicit caller context; the recovered body itself is unchanged. It is checked over 131 staggered four-lane index samples with no Mem and mono/stereo Mem. Expectations follow its original byte-count/channel limit, clamp and nearest-even conversion, including final-byte reads. The strict compile audit injects no prologue and retains the original incoming-register diagnostic. These are specification/reference-model results, not new native FlowStone numerical captures; Windows x64 community verification remains pending.

## Low-byte MOV stores

`mov memory,al/bl/cl/dl` follows the byte-sized `88 /r` form of [Intel MOV (Volume 2B, section 4-28)](https://cdrdv2-public.intel.com/858447/253667-088-sdm-vol-2b.pdf). It replaces one addressed byte without changing the source parent or EFLAGS. Indexed operands use the existing 32-bit wrapped byte-address rules, including scales 1, 2, 4 and 8 and signed displacements. Named storage retains its existing element shorthand for constant indices; register-indexed offsets are bytes. Memory outside the binding is ignored, matching OpenStone's existing bounded-access policy, rather than emulating native page faults.

`assembler_byte_register` now sweeps AL/BL/CL/DL stores over 263 samples per form, all byte values, four indexed address forms and eight storage cases: ordinary Mem, an unaligned Mem view, empty Mem, a single-byte allocation, 32-bit and 64-bit local arrays, a local packet and an output packet. Independent byte-vector oracles check every raw result word per sample and every Mem byte after each buffer, including guards outside the view, the final valid byte, negative/out-of-range addresses and wrapped scaled offsets. Each store is immediately read back. Source parents, all 16 branch spellings after six incoming flag states, and caller rounding/exception state remain unchanged. Both backends run buffer sizes 1, 3, 7, 31 and 64; the interpreter-only portable build runs the same suite.

Additional tests alias the source parent with the address index, write bytes into loaded MemArray wave data, verify that all four MemArray tables remain read-only, and retain adjacent static callable targets. A byte write invalidates any overlapping routine-target provenance even when the stored bits are unchanged, including writes through another Mem view. Static local/constant Mem targets fail compilation after clobbering; runtime-indexed targets stop that sample's stage through the existing provenance guard. Known six-byte named BL/CL/DL store encodings work in numeric jump spans; indirect and ambiguous AL widths are not guessed. Input/descriptor destinations, unwritten or opaque byte sources, mixed register widths and unsupported byte instructions fail explicitly.

The recovered Mem reader and all earlier capture/source fixtures remain byte-for-byte unchanged. The strict audit still accepts 44 category/source entries with no regression and no unlisted mnemonic names; its original incoming-register and engine-context diagnostics remain visible. These tests use specification/reference-model oracles; no new native FlowStone trace is claimed.

## High-byte MOV registers

AH/BH/CH/DH select bits 15:8 of EAX/EBX/ECX/EDX. MOV loads and copies replace only that byte, retaining bits 31:16 and 7:0. Stores extract that byte without changing the source parent. [Intel MOV (Volume 2B, section 4-28)](https://cdrdv2-public.intel.com/858447/253667-088-sdm-vol-2b.pdf) defines same-size byte moves and their unchanged flags. Its high-byte restriction on REX-prefixed instructions does not require remapping these aliases to SPL/BPL/SIL/DIL: OpenStone's numeric register model supports the four legacy 32-bit parents, with no explicit native prefixes or extended GPRs. The existing RAX/RBX/RCX/RDX address-reference spellings can still address byte memory.

The `assembler_byte_register` suite now covers all eight byte aliases. Its 259-sample load/copy matrix checks all 64 register pairs, high/high and high/low transfers, self-copies and both byte aliases of one parent. Source bytes are captured before merging with the destination, so `mov ah,al` and `mov al,ah` use the original parent. Mem, packed inputs, 32-/64-bit local arrays and FloatArray reads include all byte values, final-byte/empty/out-of-range cases and raw payload bytes. Literal signed/unsigned immediate expectations check untouched high words and low bytes.

The 263-sample store matrix covers all eight sources over four indexed address forms and eight storage cases at buffer sizes 1, 3, 7, 31 and 64. Each selected source byte visits all 256 values, with a different value in the other byte. Independent byte-vector oracles check raw results per sample and every Mem byte after each buffer, including external guards of unaligned views. Readback observes the destination byte in its correct parent position. Exact source parents, six incoming flag states, all 16 branch spellings and caller floating-point state remain unchanged in the interpreter and LLVM ORC.

Additional tests check a high-byte load whose parent is also its original address index, scaled stores sharing source/index parents, uppercase AH with an RBX memory reference, two-byte register/immediate and six-byte named-memory numeric skips, and highlighting/completion entries for all aliases. Uninitialized/opaque parents, mixed widths, byte address indices, byte instructions outside MOV/ADD/SUB/CMP/INC/DEC/AND/OR/XOR/TEST/SHL/SHR/XCHG/IDIV and missing flag producers remain errors. The interpreter-only portable build runs the same expanded suite. The strict unchanged-source audit retains 44 accepted entries with no missing mnemonic names; all original numerical/source fixtures remain unchanged. These are specification/reference-model results, not new native FlowStone captures.

## Byte ADD, SUB and CMP

The byte forms follow [Intel ADD and CMP (Volume 2A, sections 3-32 and 3-179)](https://cdrdv2-public.intel.com/843847/253666-sdm-vol-2a-dec-24.pdf) and [Intel SUB (Volume 2B, section 4-675)](https://cdrdv2-public.intel.com/858447/253667-088-sdm-vol-2b.pdf). Operands are captured once before writing. Arithmetic wraps modulo 256, stores merge only the selected register/memory byte, and CMP performs no write. An eight-bit register on either side supplies the operand width for memory; byte immediates use -128..255 encodings. Mixed register widths and implicit byte-sized memory immediates are not introduced.

The compiler emits shared integer operations consumed by both backends. It computes byte carry/borrow, even parity, zero, and the SF-xor-OF state used by the supported signed branches. Signed byte operands are extended before testing the exact unwrapped sum/difference; this distinguishes signed overflow from the wrapped sign bit. AF and separate OF/SF branch instructions are not newly exposed by this batch.

The new `assembler_byte_arithmetic` suite checks all 65,536 input byte pairs for each of ADD, SUB and CMP in the interpreter and LLVM ORC, at buffer sizes 1, 3, 7, 31 and 64. An independent oracle uses signed range checks for OF, the wrapped sign bit for SF and an unsigned range/borrow check for CF. It observes all 16 supported branch spellings and exact parent/source words. Literal 127+1, 128+128, 128-1 and 127-255 checks pin signed overflow, unsigned carry/borrow, zero and parity behavior. Prior 32-bit producers seed contrasting incoming flag states, including overflow.

A second matrix covers all 64 low/high register pairings (including self-aliases and two bytes of the same parent), all eight destinations with seven signed/unsigned immediates, signed-limit/equality/wrap boundaries, and every byte value with contrasting untouched bits and packed upper lanes. Calls, matching and mixed ADD/SUB branch joins, byte-counted loops, subsequent 32-bit INC carry preservation and caller floating-point state are checked. Known two-byte register, three-byte non-AL immediate, and six-byte named-memory spans work in numeric jumps; ambiguous AL immediate widths remain an explicit diagnostic.

Memory tests exercise both operand directions in an unaligned Mem view and local 32-/64-bit arrays, including the final valid byte, negative/out-of-range offsets and exact neighboring raw words. Each rendered buffer checks all Mem bytes, including guards outside the view. Separate cases share a source/destination parent with its address index, compare read-only input bytes, retain saved targets through CMP, and invalidate overlapping callable provenance on ADD/SUB stores even when zero arithmetic leaves the bits unchanged. Writes through overlapping Mem views and static local target facts are included. Diagnostics cover uninitialized/opaque parents, mismatched widths, read-only stores, descriptor bytes and unverified numeric widths.

The expanded MOV byte suite remains a regression check for the shared byte access helpers. Both suites also run in the interpreter-only portable build. The strict compile audit retains all 44 accepted category/source entries and zero unlisted mnemonic names. Earlier capture/source fixtures, including the recovered byte Mem reader, remain byte-for-byte unchanged; no new native FlowStone numerical capture is claimed.

## Byte INC and DEC

The register forms follow [Intel INC and DEC (Volume 2A, sections 3-517 and 3-329)](https://cdrdv2-public.intel.com/843847/253666-sdm-vol-2a-dec-24.pdf). They add/subtract one modulo 256 in AL/BL/CL/DL or AH/BH/CH/DH, preserve every other parent bit, and retain the prior CF value. Zero, parity and SF-xor-OF signed branch state are updated for the byte result. The compiler reuses the byte arithmetic path while omitting the carry calculation/store entirely, retaining its prior validity state as well. No incoming CF is invented when a byte step is the first flag producer.

`assembler_byte_step` checks all 256 byte values with both incoming carry states for both instructions on all eight aliases, across buffer sizes 1, 3, 7, 31 and 64 in the interpreter and LLVM ORC. Four seed modes exercise CF from overflowed 32-bit ADD, byte ADD, byte SUB and ordered/unordered COMISS. The independent oracle checks signed overflow with a range test, combines it with the wrapped sign bit for signed branches, and uses the wrapped byte for zero and even parity. All 16 supported branch spellings, exact parent words, unchanged companion registers, contrasting packed upper lanes and caller floating-point state are observed.

Literal 127-to-128 and 128-to-127 cases pin INC/DEC overflow; 255-to-0 and 0-to-255 cases distinguish carry preservation from ADD/SUB. Calls, same-operation and mixed INC/DEC joins, opposite-step pairs and JNZ loops test state propagation and byte-local wraparound. Numeric jumps skip the known two-byte register encoding. A step without incoming carry can still supply JZ/JP/JL; carry branches must fail both with no prior slot and after an older slot was made invalid by opaque pointer arithmetic. Unwritten/opaque parents and extra operands remain diagnostics. The same suite passes in the interpreter-only portable build.

The strict compile audit retains all 44 accepted category/source entries with no missing mnemonic names. All original source/capture fixtures remain unchanged. These are reference-model results; no new native FlowStone numerical capture is claimed. Byte-memory size qualifiers, explicit AF/OF/SF exposure and other byte operations remain separate work; unqualified memory INC/DEC retain the existing 32-bit form.

## Byte AND, OR, XOR and TEST

The byte logical forms follow Intel's [AND (Volume 2A)](https://cdrdv2-public.intel.com/843847/253666-sdm-vol-2a-dec-24.pdf), [OR and TEST (Volume 2B)](https://cdrdv2-public.intel.com/858447/253667-088-sdm-vol-2b.pdf), and [XOR (Volume 2D, section 6-40)](https://cdrdv2-public.intel.com/843855/334569-sdm-vol-2d-dec-24.pdf). They clear CF and OF and derive zero, even parity and sign from the eight-bit result. With OF clear, the modeled SF-xor-OF state is exactly result bit 7. TEST computes AND for flags without storing. Its second operand follows the existing register/immediate rule. AF is undefined; separate AF/OF/SF exposure remains outside this batch.

`assembler_byte_logical` checks all 65,536 input pairs for each of the four operations in both backends at buffer size 64. A second matrix checks all 64 low/high register pairings, including self-aliases and the two bytes of one parent, plus seven signed/unsigned immediates for each alias across buffer sizes 1, 3, 7, 31 and 64. It observes all 16 supported branch spellings, exact parent/source words and caller floating-point state. Prior 32-bit arithmetic seeds both CF values with OF set; explicit zero/sign/parity literals, zero results and self-XOR distinguish logical flag clearing from preserved carry/overflow. Inputs have contrasting untouched parent bits and packed upper lanes.

Memory tests exercise AND/OR/XOR in both operand directions and TEST with a memory first operand, using an unaligned Mem view and local 32-/64-bit arrays. Negative/out-of-range offsets, final valid bytes, all neighboring raw words and guards outside the view are checked per rendered buffer. TEST leaves input bytes and saved routine-target provenance intact. AND/OR/XOR stores invalidate overlapping callable provenance even for identity masks, including writes through an aliased Mem view. Static local target facts are checked too.

Additional cases cover source/index aliases, calls, joins with identical and mixed logical operations, a backward XOR/TEST loop, subsequent INC and known two-/three-/six-byte numeric spans. CF becomes valid and clear with no prior producer, after an older flag slot was invalidated by opaque pointer arithmetic, and after unordered COMISS. Byte self-XOR requires an initialized numeric parent; it cannot fabricate the other 24 bits or partially overwrite an opaque reference. Diagnostics reject mixed widths, descriptor/read-only stores, invalid immediates, reversed TEST memory operands and ambiguous AL-immediate numeric widths. These tests also pass in the interpreter-only portable build.

The strict unchanged-source audit retains all 44 accepted category/source entries with zero unlisted mnemonic names. All earlier numerical/source fixtures remain byte-for-byte unchanged. These are specification/reference-model checks; no new native FlowStone capture is claimed.

## Byte SHL and SHR

The register shifts follow [Intel SAL/SAR/SHL/SHR (Volume 2B, sections 4-594–4-597)](https://cdrdv2-public.intel.com/858447/253667-088-sdm-vol-2b.pdf). All eight low/high aliases accept unsigned byte immediates or CL. The count is masked to five bits, and a masked zero preserves the entire parent and every flag. Nonzero shifts wrap/zero-fill in eight bits and preserve the other 24 parent bits. Masked counts from 8 through 31 produce zero. ZF/PF and one-bit SF-xor-OF follow the byte result and defined overflow; defined carry contains the last bit shifted out.

Intel leaves OF undefined for counts greater than one, CF undefined for counts at least eight, and AF undefined for nonzero shifts. OpenStone keeps the existing scalar-shift OF=0 convention for multi-bit counts. Its deterministic CF extension follows iterative eight-bit shifting: count eight contains the final old bit; greater counts clear CF. Tests pin those choices for backend consistency, without claiming native FlowStone parity for undefined flags. Separate AF/OF/SF exposure remains outside this batch.

`assembler_byte_shift` checks every 256-byte-value/256-raw-count pair for each direction in both backends at buffer size 64. All eight destinations run a byte-value matrix with 19 count boundaries (CL self-cases necessarily use one byte for both operands), and 19 immediate counts run on every alias, across buffer sizes 1, 3, 7, 31 and 64. Six incoming flag patterns include both carry states, signed overflow, nontrivial parity, zero, COMISS and unordered NaNs. All 16 supported branch spellings, exact parent/count words, contradictory packed upper lanes and caller rounding/exception state are observed. CL and CH alias cases capture the old CL before modifying ECX; CL itself may change only its selected byte.

Local calls, identical/mixed joins, backward SHL/SHR loops, carry propagation into 32-bit INC, fresh and stale flag slots, and known two-/three-byte numeric spans are covered. A positive immediate defines flags without an earlier producer. Zero immediates preserve incoming availability; variable CL cannot define flags that were previously unavailable, since its runtime count may be zero. Unwritten/opaque parents, non-CL register counts, invalid immediates and unsized memory destinations remain diagnostics. Count-one numeric spans remain unknown because both D0 and C0 encodings are possible. Full 32-bit shift forms retain their existing behavior.

The same suite passes in the interpreter-only portable build. The strict unchanged-source audit retains all 44 accepted category/source entries with zero missing mnemonic names. All earlier source/capture fixtures remain unchanged; these are specification/reference-model tests, not new native FlowStone captures.

## Byte XCHG swaps

The byte forms follow [Intel XCHG (Volume 2D, sections 6-32–6-33)](https://cdrdv2-public.intel.com/843855/334569-sdm-vol-2d-dec-24.pdf): two byte registers, or one byte register and one memory operand, exchange their original contents without changing flags. All eight low/high aliases retain every parent bit outside the selected byte. AL/AH-style shared-parent swaps capture both old bytes before merging either result. Memory is written before the register changes, preserving the old effective address when its index shares the byte register's parent.

`assembler_byte_exchange` checks all 64 alias pairs over a 263-sample byte corpus, self-swaps, same-parent swaps, local calls, joins and double swaps across buffer sizes 1, 3, 7, 31 and 64 in both backends. Separate exhaustive sweeps check all 65,536 byte-value pairs for AH/BL and AH/AL at buffer size 64. Six literal incoming flag patterns include CMP less/equal/greater, both ADD overflow cases and unordered COMISS; all 16 supported conditional branch spellings retain their previous result. Raw parents, contradictory packed upper lanes and caller rounding/exception state are observed.

Memory swaps cover all eight aliases in both operand orders, four scaled/displaced address forms, and eight storage cases: ordinary Mem, an unaligned view, empty Mem, a single-byte allocation, 32-/64-bit local arrays, a local packet and an output packet. Independent byte-vector oracles compare old register/memory values, every resulting raw word, and Mem bytes/outer guards after each buffer. Negative/final/out-of-range/wrapped offsets are covered. Additional literals alias each exchanged parent with the address index at scales 1/2/4/8, ensuring the old address is used. Loaded MemArray wave bytes can be exchanged; all four descriptor tables stay protected.

Any partial write clears overlapping callable-target provenance even for an identity exchange, including aliased Mem views; neighboring untouched targets remain callable. Two-byte register and six-byte named-memory numeric spans are tested. Unknown indirect/indexed spans remain diagnostics. Missing/opaque parents, mixed widths, immediates, two-memory exchanges, read-only inputs/descriptors and malformed operands are rejected. XCHG leaves flag availability unchanged: it needs no prior flags for data-only swaps and cannot supply missing flags to a later branch. Existing byte-register negative tests now reject mixed-width XCHG rather than the newly valid byte forms.

The portable memory policy remains bounded: invalid reads return zero, invalid writes are ignored, and register merging still occurs. Like the existing 32-bit XCHG lowering, this is a sequential DSP swap, not an emulation of native cross-thread bus locking or page faults. The same byte-exchange, byte-register and 32-bit-exchange suites pass without LLVM. The strict unchanged-source audit retains all 44 accepted category/source entries and no missing mnemonic names. Earlier source/capture fixtures remain byte-for-byte unchanged; these are specification/reference-model checks, not new native FlowStone traces.

## Byte IDIV division

The byte form follows [Intel IDIV (Volume 2A, sections 3-441–3-443)](https://cdrdv2-public.intel.com/868140/253666-089-sdm-vol-2a.pdf). Signed AX is divided by a signed eight-bit divisor. The quotient truncates toward zero and must fit -128..127; AL receives its raw byte and AH receives the dividend-signed remainder. EAX bits 31:16 and EDX are preserved. The entire numeric EAX parent and a register divisor's parent must be initialized, including AL/AH aliases. Byte division does not require EDX initialization. All arithmetic flags are undefined and require a new producer before branching.

`idiv al/bl/cl/dl/ah/bh/ch/dh` selects the byte form. Memory uses `idiv byte cell`, `idiv byte cells[edx]`, or `idiv byte ptr [ecx+edx*4]`; BYTE/PTR are case insensitive. These read exactly one byte, including from local Double storage, read-only DSP/FloatArray inputs, and the final byte of a bound Mem. Unsized `idiv memory` retains its 32-bit form. A declared symbol named `byte` remains an ordinary symbol, preserving earlier grammar. Byte qualifiers on registers and direct Mem/MemArray descriptor fields are rejected. Register and named-memory byte forms have known two-/six-byte numeric spans; indirect/indexed widths remain conservative diagnostics.

The existing three-input/two-result divide operation uses `bits=8` internally. The interpreter independently masks/signs AX and its divisor, divides unsigned magnitudes, and checks the signed byte limit. LLVM sign-extends i16/i8 into its guarded signed-division path and checks -128..127 before committing. Optimization may safely narrow the internal division. Both paths return before any result store on zero/overflow; only success reaches the two partial EAX merges. Fault kind and first sample frame remain sticky until reset, and later samples can recover. Native traps/page faults are not delivered, matching the existing 32-bit divide policy.

`assembler_byte_divide` runs 18 register/memory/call/skip forms over signed boundaries, all divisor bytes, near-limit quotients and nonzero remainders, at buffer sizes 1, 3, 7, 31 and 64 in both backends. AL/AH divisors use their original values; an EAX-indexed Mem operand uses its original address. Raw EAX/AL/AH, complete divisor parents and EDX, success markers, exact fault kind/frame, reset, caller rounding/exception state and final-byte memory neighbors are checked. The FloatArray variant uses a fixed signed-byte source, reflecting its buffer-scoped binding.

Direct backend tests independently exercise all 65,536 AX bit patterns with seven signed divisor values (-128, -127, -1, 0, 1, 2 and 127), with contradictory upper EAX, high-input and divisor bits. They check exact signed outputs on success and unchanged slots plus skipped following instructions on failure. Literal result words pin negative quotients and remainders. Additional tests cover single-byte/empty/out-of-range bindings, earlier local subroutines, loops, joins, no implicit EDX read, numeric spans, malformed/opaque operands and invalidation of all 16 supported conditional branch spellings. Existing high-byte negative tests now use malformed IDIV operands rather than the newly valid forms.

The byte-divide, 32-bit-divide and byte-register suites also pass in the portable interpreter-only build. The strict unchanged-source audit retains all 44 accepted category/source entries and zero missing mnemonic names. Earlier source/capture fixtures remain unchanged; these are specification/reference-model tests, not new native FlowStone numerical captures. Word/64-bit IDIV forms remain separate work.

## Verified engine constants

Five new fixtures are exact kernel slices from the Alpha 5483 x64 executable: `engine-double-floor.asm`, `engine-double-ceiling.asm`, `engine-double-reciprocal.asm`, `engine-double-sign.asm`, and `engine-double-absolute.asm`. Their lengths, SHA-256 hashes, binary locations, and the x86/x64 constructor evidence for `_DONE`, `_DMONE`, `_DABS` and `_DSIGN` are recorded in [the engine-constant evidence](../../../docs/compatibility/flowstone-assembler-engine-constants.md). No declarations or caller wrappers were added to these bodies. All earlier source/capture fixtures remain unchanged.

`assembler_engine_constants` runs the original bodies in both backends over four distinct Double lanes and buffer sizes 1, 3, 7, 31 and 64. Independent oracles check exact masks/signs, reciprocal values, and the original Floor/Ceiling kernels' DWORD-conversion behavior, including their limited numeric range. Raw packet/int/byte/x87 reads, lazy constant slots, explicit variable shadowing, reset/stage timing, calls, numeric branch spans, unchanged caller floating-point state, and read-only diagnostics are also covered. Stage-3 outputs retain the established one-sample latency. The suite passes in the portable interpreter-only build.

These are static executable bindings plus specification/reference-model results; no new native FlowStone execution trace is claimed. The strict audit gains exactly five entries (44 → 49), preserving every earlier accepted entry and the empty missing-mnemonic list. Missing register/frame context and `__const` bindings remain explicit failures.


### Float engine constants

Four added fixtures (`engine-float-floor.asm`, `engine-float-ceiling.asm`, `engine-float-sign.asm`, `engine-float-absolute.asm`) are byte-for-byte engine fragments present in both Alpha 5483 executables. [The engine-constant evidence](../../../docs/compatibility/flowstone-assembler-engine-constants.md#verified-float-bindings) records their native offsets, lengths, hashes and the exact x86/x64 constructor words for all nine Float constants: `_PI`, `_E`, `_SQRT2`, `_ABS`, `_SIGN`, `_DENORM`, `_ZERO`, `_ONE`, `_MONE`.

Unlike the earlier Double kernels, these internal Float fragments omit their caller's ports. Tests prepend only `streamin in0;streamout out0;` to establish their typed I/O context, retaining every fixture byte and instruction. No constant declarations or register initialization are injected. The strict audit continues to report the missing caller `in0` and retains all 49 accepted entries with zero missing mnemonic names.

The extended `assembler_engine_constants` suite compares all four kernels against independent bit/math oracles in the interpreter and LLVM ORC over four distinct lanes and buffers 1, 3, 7, 31, 64. It checks NaN payloads, infinities, signed zeros, subnormals, fractions and DWORD conversion boundaries; raw PS/PD/DWORD/byte reads of every constant; mixed Float/Double storage; calls, stages, reset and numeric spans; unchanged caller rounding/exception state; explicit shadowing, read-only diagnostics and completion. The portable interpreter-only suite passes too. Existing fixtures remain unchanged, and no new native FlowStone execution trace is claimed.


## Assembler stage directives

`assembler-stage-delay1-float.asm` and `assembler-stage-delay1-double.asm` are exact engine strings found in both Alpha 5483 executables. [Stage-directive evidence](../../../docs/compatibility/flowstone-assembler-stage-directives.md) records their hashes, stored lengths and native file offsets. They save input in `stage 3;` and publish the previous value in `stage 1;`. The Float fragment needs explicit caller ports supplied separately in the rendering test; the Double kernel is entirely self-contained. Fixture bodies, earlier sources and numerical captures are unchanged.

`assembler_stage_directives` compares exact words for scalar and four-lane Float/Double delays against an independent one-sample oracle at buffers 1, 3, 7, 31, 64 and 257, plus varying/zero-frame calls. Signed zeros, subnormals, infinities, quiet/signaling NaNs, Double-only precision, reset and caller floating-point state are covered. Staggered four-lane feedback tests compare both taps and tap orders against an independent recurrence. Further tests exercise stage initialization/order, held late outputs, mixed block syntax, local names, direct/indirect calls, loops, numeric spans and boundary diagnostics. Both the LLVM/interpreter suite and portable interpreter-only suite pass.

The strict unchanged-source audit gains exactly the self-contained Double kernel (49 → 50 accepted entries), keeping every prior pass and zero missing mnemonic names. The Float fragment advances to its real missing caller-port diagnostic. The audit integration test also prepares the original Double body without source rewriting. No new native FlowStone numerical capture is claimed.


## Additional GPR families

`assembler-mem-byte-writer.asm`, `engine-frame-load-float.asm` and `engine-frame-load-double.asm` are exact recovered bodies found in both Alpha 5483 executables. [Register evidence and validation](../../../docs/compatibility/flowstone-assembler-extra-gprs.md) records binary hashes, original offsets and fixture SHA-256 values. Rendering tests provide explicit caller save-register values or a bounded Mem-backed frame around the original bodies; no fixture instruction is rewritten. The strict audit retains their genuine incoming-state diagnostics and the exact earlier set of 50 accepted entries, with no missing mnemonic names.

The `assembler_extra_gprs` suite checks ESI/EDI/EBP and their pointer/target aliases RSI/RDI/RBP in both backends across buffers 1, 3, 7, 31 and 64. It covers arithmetic, logic, all 16 conditional branch spellings, raw results, independent register storage, original byte-writer memory effects and save/restore, Float/Double frame loads, bounded tails, pointer/target tags, local calls/tail transfers, stack operations, loops, numeric skips, XCHG, stage and width diagnostics, and completion. Legacy AL/BL/CL/DL/AH/BH/CH/DH must retain their original four parent registers. The new and existing byte-register suites also pass without LLVM using portable SoftFloat. Full 64-bit integers, native frame entry, ESP/RSP and additional byte/word aliases remain separate work. No new native FlowStone numerical capture is claimed.

## Native incoming-register bindings

The four `assembler-incoming-clock-*`, `assembler-mem-nibble-reader.asm` and `assembler-memarray-flat-reader.asm` fixtures are exact, unchanged Alpha 5483 executable source slices. The existing `mem-reader-byte.asm` and `assembler-mem-byte-writer.asm` also run standalone without synthetic register prologues. Their new headless tests cover the native base ECX clock and preservation-only saves of unknown incoming GPRs in both backends. [Exact offsets, hashes, entry disassembly, test provenance and remaining frame/wrapper limits](../../../docs/compatibility/flowstone-assembler-entry-bindings.md).

## Native frame memory

`engine-frame-mask-read-x86.asm` and `engine-frame-mask-read-x64.asm` are exact 77-byte reducer-source substrings. Both run unchanged with caller memory and input/output context. Existing Float/Double frame-load fixtures now also run with automatically bound RBP/RDX. [Offsets, hashes, memory-alias evidence, tests and remaining native context](../../../docs/compatibility/flowstone-assembler-frame-bindings.md). No previous source or numerical capture was changed.

## Named engine pointers

The seven `engine-bound-*` fixtures are exact Alpha 5483 executable substrings, including original x86 absolute and x64 register-based forms. They run with explicit named caller data in both backends; the Float fragment receives only its missing outlet declaration. [Native binder evidence, exact offsets/hashes, resource contract, tests and remaining pointer-table gaps](../../../docs/compatibility/flowstone-assembler-engine-pointers.md). Existing sources and numerical captures were not changed.

## Engine pointer cells and tables

The six `engine-sample-index-*` and `engine-buffer-table-*` fixtures are unchanged Alpha 5483 executable slices. Headless tests bind the original four/eight-byte pointer layouts and add only missing caller declarations/readout taps. [Native provider evidence, exact offsets/hashes, typed table API and remaining dispatch limits](../../../docs/compatibility/flowstone-assembler-engine-tables.md). Prior code and numerical fixtures are unchanged.

## Buffer-transfer loop flags

The two `engine-buffer-transfer-*` fixtures are exact 574-byte Alpha 5483 source slices. Tests add only their caller's frame/input/readout context, counter writeback and four saved-register restores. [Exact offsets/hashes, backward flag-liveness fix, test oracles and strict audit limits](../../../docs/compatibility/flowstone-assembler-loop-flags.md). Existing fixtures and numerical captures are unchanged.


## Assembler signal-input stores

`assembler-envelope-legacy.asm` and `assembler-envelope-current.asm` are exact, complete serialized Alpha 5483 x64 source substrings. They quantize their signal parameters by storing back into the input operands. Headless tests run both unchanged bodies with four independent lanes and compare every sample against an independent envelope state machine in the interpreter and LLVM ORC across six buffer sizes. Additional tests cover private input copies, partial writes, Double precision, stage refresh and Poly voice reuse. [Exact lengths, hashes, offsets, runtime boundaries and test provenance](../../../docs/compatibility/flowstone-assembler-input-writes.md). Earlier sources and numerical captures remain unchanged; no new native Windows capture is claimed.


## Compact Assembler stages and output seeds

`engine-dezipper-compact-x86.asm` and `engine-dezipper-compact-x64.asm` are exact 337-/391-byte Alpha 5483 executable substrings. Tests prepend only the omitted caller outlet, bind the original controls explicitly and compare every output word against independent recurrences in the interpreter and LLVM ORC across six buffer sizes. The x86 body tests/clears `p_target`; x64 tests/clears `p_reset`. Both remain unchanged. Additional tests cover stage-zero output publication, Double precision, seeded feedback, staggered Poly initialization and reused voice slots, contextual names and stage boundaries. [Offsets, hashes, caller contract, architecture differences and validation](../../../docs/compatibility/flowstone-assembler-compact-stages.md). Earlier fixtures and captures are unchanged; no new native Windows execution trace is claimed.


## Numeric Mem descriptor register aliases

`engine-mem-reader-channels.asm`, `engine-mem-reader-hop-x86.asm` and `engine-mem-reader-hop-x64.asm` preserve complete, exact Alpha 5483 executable source strings. All start channel dispatch with `mov rax,mem[3]` followed by DWORD comparisons. Tests run their exact dispatch prefixes with explicit classification bodies in both backends across six buffer sizes; the complete sample-loading bodies retain their separate missing caller ports and `mem[4]` Float-limit requirement. Further tests cover integer fields through all seven wide aliases, exact words and flags, live metadata rebinding, views, calls/stacks/loops, stage timing and pointer/target invalidation. [Native offsets/hashes, precise test scope, alias contract and audit results](../../../docs/compatibility/flowstone-assembler-descriptor-aliases.md). Prior fixtures and captures remain unchanged; no new Windows execution capture is claimed.


## Packed Mem limit and complete Wave Read kernels

The unchanged `engine-mem-reader-channels.asm`, `engine-mem-reader-hop-x86.asm` and `engine-mem-reader-hop-x64.asm` fixtures now run as complete bodies with only omitted caller ports declared and original hop controls explicitly bound. Every sample word matches independent mono/stereo wave-lookup, hop and hold oracles in the interpreter and LLVM ORC across six buffer sizes. Field 4 is a read-only Float packet verified in both native `CVarMemRef` binders; tests pin its two rounding steps near 2^24 using real Mem views and literal expected bits. Further coverage includes empty/short buffers, live wave changes, tails, raw views, scalar/x87 reads, stage timing, input/owner preservation, adjacent descriptor isolation and portable interpreter execution. [Binder addresses and hashes, precise formula, complete-body test scope and strict audit results](../../../docs/compatibility/flowstone-assembler-mem-limit.md). No fixture byte or earlier numerical capture changed; no new native Windows trace is claimed.

## Packed numeric Mem descriptors and complete Wave Write kernels

`engine-mem-writer-mono.asm` and `engine-mem-writer-stereo.asm` are exact, complete 668-/928-byte Alpha 5483 source strings from both executable builds. Tests prepend only the missing caller ports. Both unchanged bodies match independent passthrough and byte-write oracles in the interpreter and LLVM ORC across six buffer sizes, including live Mem changes, bounds, root lane-zero branch behavior, five Poly voices, inactive masks, release and reused slots. Raw PS/PD tests pin the sixteen-byte windows at numeric fields 1..3 and their overlap with the Float limit. [Source offsets/hashes, native layout evidence, implementation bounds and audit scope](../../../docs/compatibility/flowstone-assembler-descriptor-packets.md). Earlier fixture and numerical capture bytes remain unchanged; no new native Windows execution capture is claimed.


## x87 mixed-width conversion memory

The four `engine-float-to-double-*` / `engine-double-to-float-*` fixtures preserve complete, exact Alpha 5483 source strings from both executable builds. Headless tests supply caller buffers, optional accumulators and raw readouts around each unchanged body, then compare every sample word in the interpreter and LLVM ORC across six buffer sizes. Independent IEEE expansion/literal rounding oracles include NaN payloads, signed zeros, subnormals and Double-only precision. Additional tests cover cross-width loads/stores in packets and arrays, partial Double preservation, bounds, caller FP state, private input copies, target provenance and five Poly voices with slot reuse. [Source hashes/offsets, caller context, memory-width contract and audit scope](../../../docs/compatibility/flowstone-assembler-x87-memory-views.md). Earlier source and numerical fixtures are unchanged; no new native Windows numerical capture is claimed.
