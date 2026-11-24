# Incoming XMM registers and Denormal Remover

The unchanged bundled **Denormal Remover (Bitmask)** block now compiles and loads
through the `.fsm` importer. Its `por xmm1,xmm1` remains a self-OR. OpenStone does
not rewrite it as `pxor`, edit the source, or execute its compiled FlowStone cache.

## Evidence and portable policy

The saved [FlowStone denormal experiments](../../tests/fixtures/flowstone/README.md#denormal-remover-bitmask)
show that incoming `xmm1` is not reliably zero. Reordering the producer while
preserving its signal samples changed `xmm1` and the remover's output. These are
scratch values left by generated upstream code, not a portable ABI constant.

OpenStone therefore supplies an explicit incoming XMM bank instead of reading
host CPU registers. **Unbound values default to zero.** This is a defined
OpenStone policy, not a claim that FlowStone initializes its registers to zero.
A host can supply the measured entry bits to reproduce either saved trace.
Native upstream register-allocation/clobber behavior is not reconstructed by this
change, and scratch-dependent programs may differ unless that context is supplied.

## Compiler and runtime

All sixteen XMM registers have an entry value. Registers remain canonical raw
128-bit packets, represented as four low-to-high DWORDs. The compiler records
only used register slots. The runtime refreshes them at each stage invocation,
before executing either interpreter or LLVM ORC code. Writes, partial writes,
branches, loops and local calls use those ordinary private register slots. Writes
do not contaminate another stage, block, sample or Poly packet's entry bank.
GPR pointer provenance, integer/flag availability and stack checks are unchanged.

`Runtime::setDspIncomingXmm(block, reg, frames)` copies caller-owned
`DspXmmRegister` packets between render calls:

- A single packet supplies a constant; an empty span restores the zero default.
- A multi-packet trace starts at the current render frame, then supplies zero
  after its last packet. Invalid block/register bindings fail without mutation.
- Reset retains the copied trace and rewinds it to frame zero. Rebinding between
  buffers starts a fresh trace at that point.
- Up to 1,048,576 packets can be supplied per register. Storage is allocated only
  when explicitly binding a bank, outside rendering. Default banks need no bank
  allocation; processing remains allocation-free.

Bindings describe physical SSE packets rather than numerical Float/Double
conversions. The direct scalar and packed Runtime APIs expose these bindings.
The Poly renderers use zero defaults in each physical packet; no new public Poly
voice-binding API is introduced. No register context is inferred from discarded
FlowStone cache bytes or persisted as a schematic's runtime state.

## Validation

`assembler_xmm_entry` checks all sixteen registers, raw signed zeros, positive
and negative subnormals, quiet/signaling NaN payloads and arbitrary words in both
backends. Tests cover owned input copies, mutation followed by a fresh entry,
Float and Double physical views, frame traces, trace exhaustion, clearing,
rebinding, reset, branch joins, local returns, stages 0–3 and five Poly voices
across two SSE packets. Relevant matrices run at buffer sizes 1, 3, 7, 31 and 64.
The suite also passes with LLVM disabled and portable SoftFloat enabled.

The existing schematic regression now compiles the exact original 160-byte
`denormal-mask.asm`, without a register-loading prologue or extra source ports.
It binds both previously captured `xmm1` traces and compares every output word
against the corresponding saved FlowStone trace at buffer sizes 1, 7 and 64 in
both backends. Unbound zero entry is checked against the saved explicitly-zeroed
trace. This preserves the original sign-inclusive mask's behavior, including its
handling of negative subnormals; this change does not redesign that filter.

The optional bundled import suite includes the hash-verified original
`Denormal Remover (Bitmask).hom` record (version 170). It skips the vendor cache,
checks unchanged source and source links, renders an impulse at buffer sizes
1, 7, 64 and 257, saves the conversion, reopens it and repeats the checks.

The compile audit increases from **58 to 59 prepared category/source entries**
(67 frontend passes, eight requiring explicit engine resources). All **11/11
bundled module sources** now compile and prepare in both backends. Every previous
pass is retained, and the unlisted mnemonic census remains empty. This is not an
exhaustive instruction/ABI parity claim or a new native Windows capture.
