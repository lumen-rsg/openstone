import argparse
import importlib.util
import json
import subprocess
import sys
import tempfile
import os
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("audit", ROOT / "tools/audit_flowstone_assembler.py")
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)
PROBE = None


REFERENCE_DIR = ROOT / "tests/fixtures/flowstone"
REFERENCE_NAMES = [line for line in (REFERENCE_DIR / "external-sources.txt").read_text().splitlines()
                   if line and not line.startswith("#")]
MISSING_REFERENCES = [name for name in REFERENCE_NAMES if not (REFERENCE_DIR / name).is_file()]


def private_references(method):
    def run(self):
        if MISSING_REFERENCES:
            message = "private FlowStone reference corpus omitted; see docs/testing.md"
            if os.environ.get("OPENSTONE_REQUIRE_FLOWSTONE_FIXTURES") == "1":
                self.fail(message)
            self.skipTest(message)
        return method(self)
    return run


def record(source, counts=(1, 1)):
    length = bytes([len(source)]) if len(source) < 255 else b"\xff" + len(source).to_bytes(2, "little")
    return bytes(counts) + length + source


class ExtractionTest(unittest.TestCase):
    def test_length_boundaries_and_exact_bytes(self):
        source = b"streamin in;\r\nstreamout out;\r\nmovaps xmm0,in;movaps out,xmm0;"
        for padding in (b"", b"//" + b"x" * 280 + b"\n", b"# COMPATIBILITY_3_0_8 // legacy header\r\n"):
            text = padding + source
            data = b"\x00\x80" + record(text) + b"\x00trailing text;"
            offset, extracted = next(audit.source_records(data))
            self.assertEqual(extracted, text)
            self.assertEqual(data[offset:offset + len(text)], text)
            self.assertEqual(len(list(audit.source_records(data))), 1)

    def test_malformed_records_and_dsp_are_excluded(self):
        source = b"streamin in;streamout out;movaps xmm0,in;movaps out,xmm0;"
        for data in (record(source, (2, 1)), record(source)[:-6], record(source.replace(b"movaps xmm0,in;movaps out,xmm0;", b"out = in * 0.5;"))):
            self.assertEqual(list(audit.source_records(data)), [])

    def test_commas_and_zero_header_copy(self):
        source = b"streamin a,b;streamout c,d;movaps xmm0,a;movaps c,xmm0;movaps d,xmm0;"
        self.assertEqual(audit.port_counts(source), (2, 2))
        self.assertEqual(next(audit.source_records(record(source, (2, 2))))[1], source)
        self.assertEqual(list(audit.source_records(record(source, (0, 0)))), [])
        self.assertEqual(next(audit.source_records(record(source, (0, 0)), require_counts=False))[1], source)

    def test_instruction_census_ignores_comments_and_labels(self):
        source = b"// missing fake,xmm0;\nstreamin in;streamout out;float tmp;stage(2){again: movaps xmm0,in; /* bad xmm0; */ test eax,7;jnz again;movaps out,xmm0;}"
        self.assertEqual([name for name, _ in audit.instruction_statements(source)], ["movaps", "test", "jnz", "movaps"])

    def test_unavailable_is_not_a_pass(self):
        result = {"frontend": {"status": "pass"}, "interpreter": {"status": "pass"}, "llvm_orc": {"status": "unavailable"}}
        self.assertEqual(audit.classify(result, b""), "llvm-unavailable")

    def test_recovery_deduplicates_and_keeps_locations(self):
        source = b"streamin in;streamout out;movaps xmm0,in;movaps out,xmm0;"
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "Modules").mkdir()
            (root / "Modules/test.hom").write_bytes(b"\0" + record(source) + b"\0" + record(source))
            for arch in ("x86", "x64"):
                (root / arch).mkdir()
                (root / arch / "FlowStone.exe").write_bytes(source + b"\0")
            blocks, files = audit.recover(root)
            self.assertEqual(len(files), 3)
            self.assertEqual(len(blocks), 2)
            self.assertEqual({b["kind"] for b in blocks}, {"module-record", "kernel-string"})
            self.assertTrue(all(len(b["occurrences"]) == 2 for b in blocks))
            self.assertEqual(blocks[0]["sha256"], blocks[1]["sha256"])


@unittest.skipUnless(PROBE, "Pass --probe for compiler integration checks")
class ProbeTest(unittest.TestCase):
    def test_scalar_memory_destination_forms(self):
        words = json.loads(subprocess.check_output([str(PROBE), "--words"], text=True))
        for op in ("add", "sub", "and", "or", "xor", "shl", "shr"):
            with tempfile.TemporaryDirectory() as temporary:
                path = Path(temporary) / "memory.asm"
                path.write_text("memrefin data;streamout out;mov ecx,data[0];" + op + " [ecx],7;movaps xmm0,[ecx];movaps out,xmm0;")
                result = audit.probe_source(PROBE, path, 20)
                self.assertEqual(result["frontend"]["status"], "pass")
                self.assertEqual((result["inputs"], result["outputs"]), (1, 1))
                self.assertEqual(result["interpreter"]["status"], "pass")
                self.assertEqual(result["llvm_orc"]["status"], "pass" if words["llvm_orc_available"] else "unavailable")
                if op in ("add", "sub", "and", "or"):
                    path.write_text("streamout out;int first,second;" + op + " first,second;")
                    result = audit.probe_source(PROBE, path, 20)
                    self.assertEqual(result["frontend"]["status"], "fail")
                    self.assertIn("cannot have two memory operands", result["frontend"]["message"])

    @private_references
    def test_x87_conversion_fragments_keep_their_caller_memory_context(self):
        words = json.loads(subprocess.check_output([str(PROBE), "--words"], text=True))
        for narrow in (False, True):
            for add in (False, True):
                name = "engine-" + ("double-to-float-" if narrow else "float-to-double-") + ("add.asm" if add else "move.asm")
                original = ROOT / "tests/fixtures/flowstone" / name
                result = audit.probe_source(PROBE, original, 20)
                self.assertEqual(result["frontend"]["status"], "fail")
                self.assertIn("unknown DSP name 'io'", result["frontend"]["message"])
                if narrow:
                    prefix = b"streamin first,second;streamout out;float io[8],tmp;movaps xmm0,first;movaps io[0],xmm0;movaps xmm0,second;movaps io[4],xmm0;"
                    if add:
                        prefix += b"xorps xmm0,xmm0;"
                    suffix = b"movaps out,xmm0;"
                else:
                    prefix = b"streamin io;streamout first,second;float tmp[8];"
                    if add:
                        prefix += b"xorpd xmm0,xmm0;xorpd xmm1,xmm1;"
                    suffix = b"movaps first,xmm0;movaps second,xmm1;"
                with tempfile.TemporaryDirectory() as temporary:
                    wrapped = Path(temporary) / "caller.asm"
                    wrapped.write_bytes(prefix + original.read_bytes() + suffix)
                    result = audit.probe_source(PROBE, wrapped, 20)
                    self.assertEqual(result["frontend"]["status"], "pass")
                    self.assertEqual((result["inputs"], result["outputs"]), (2, 1) if narrow else (1, 2))
                    self.assertEqual(result["engine_pointers"], [])
                    self.assertEqual(result["interpreter"]["status"], "pass")
                    self.assertEqual(result["llvm_orc"]["status"], "pass" if words["llvm_orc_available"] else "unavailable")

    @private_references
    def test_mem_writers_retain_their_original_caller_contract(self):
        words = json.loads(subprocess.check_output([str(PROBE), "--words"], text=True))
        for stereo in (False, True):
            original = ROOT / "tests/fixtures/flowstone" / ("engine-mem-writer-stereo.asm" if stereo else "engine-mem-writer-mono.asm")
            result = audit.probe_source(PROBE, original, 20)
            self.assertEqual(result["frontend"]["status"], "fail")
            self.assertIn("unknown DSP name 'in2'", result["frontend"]["message"])
            with tempfile.TemporaryDirectory() as temporary:
                wrapped = Path(temporary) / "caller.asm"
                ports = b"streamin in1,in2,in3;streamout out0,out1;" if stereo else b"streamin in1,in2;streamout out0;"
                wrapped.write_bytes(ports + original.read_bytes())
                result = audit.probe_source(PROBE, wrapped, 20)
                self.assertEqual(result["frontend"]["status"], "pass")
                self.assertEqual((result["inputs"], result["outputs"]), (4, 2) if stereo else (3, 1))
                self.assertEqual(result["engine_pointers"], [])
                self.assertEqual(result["interpreter"]["status"], "pass")
                self.assertEqual(result["llvm_orc"]["status"], "pass" if words["llvm_orc_available"] else "unavailable")

    @private_references
    def test_mem_readers_retain_their_original_caller_contract(self):
        words = json.loads(subprocess.check_output([str(PROBE), "--words"], text=True))
        for name in ("engine-mem-reader-channels.asm", "engine-mem-reader-hop-x86.asm", "engine-mem-reader-hop-x64.asm"):
            original = ROOT / "tests/fixtures/flowstone" / name
            result = audit.probe_source(PROBE, original, 20)
            self.assertEqual(result["frontend"]["status"], "fail")
            self.assertIn("unknown DSP name 'in1'", result["frontend"]["message"])
            with tempfile.TemporaryDirectory() as temporary:
                wrapped = Path(temporary) / "caller.asm"
                wrapped.write_bytes(b"streamin in1;streamout out0,out1;" + original.read_bytes())
                result = audit.probe_source(PROBE, wrapped, 20)
                self.assertEqual(result["frontend"]["status"], "pass")
                self.assertEqual((result["inputs"], result["outputs"]), (2, 2))
                status = "requires-bindings" if "hop" in name else "pass"
                self.assertEqual(result["interpreter"]["status"], status)
                self.assertEqual(result["llvm_orc"]["status"], status if words["llvm_orc_available"] else "unavailable")

    @private_references
    def test_compact_stage_kernels_advance_to_their_caller_context(self):
        for arch in ("x86", "x64"):
            original = ROOT / "tests/fixtures/flowstone" / f"engine-dezipper-compact-{arch}.asm"
            result = audit.probe_source(PROBE, original, 20)
            self.assertEqual(result["frontend"]["status"], "fail")
            self.assertIn("unknown DSP name 'out0'", result["frontend"]["message"])
            with tempfile.TemporaryDirectory() as temporary:
                wrapped = Path(temporary) / "caller.asm"
                wrapped.write_bytes(b"streamout out0;" + original.read_bytes())
                result = audit.probe_source(PROBE, wrapped, 20)
                self.assertEqual(result["frontend"]["status"], "pass")
                self.assertEqual((result["inputs"], result["outputs"]), (0, 1))
                self.assertEqual(result["engine_pointers"], ["p_target", "p_posstep", "p_negstep", "p_reset"])
                self.assertEqual(result["interpreter"]["status"], "requires-bindings")
                self.assertEqual(audit.classify(result, wrapped.read_bytes()), "engine-bindings-required")

    @private_references
    def test_unchanged_envelopes_with_signal_input_stores(self):
        words = json.loads(subprocess.check_output([str(PROBE), "--words"], text=True))
        for variant, outputs in (("legacy", 2), ("current", 3)):
            path = ROOT / "tests/fixtures/flowstone" / f"assembler-envelope-{variant}.asm"
            result = audit.probe_source(PROBE, path, 20)
            self.assertEqual(result["frontend"]["status"], "pass")
            self.assertEqual((result["inputs"], result["outputs"]), (7, outputs))
            self.assertEqual(result["interpreter"]["status"], "pass")
            self.assertEqual(result["llvm_orc"]["status"], "pass" if words["llvm_orc_available"] else "unavailable")

    @private_references
    def test_unchanged_stage_directive_kernel(self):
        path = ROOT / "tests/fixtures/flowstone/assembler-stage-delay1-double.asm"
        result = audit.probe_source(PROBE, path, 20)
        self.assertEqual(result["frontend"]["status"], "pass")
        self.assertEqual((result["inputs"], result["outputs"]), (1, 1))
        self.assertEqual(result["interpreter"]["status"], "pass")
        words = json.loads(subprocess.check_output([str(PROBE), "--words"], text=True))
        self.assertEqual(result["llvm_orc"]["status"], "pass" if words["llvm_orc_available"] else "unavailable")

    def test_engine_pointer_contract_is_not_a_prepared_pass(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "engine.asm"
            path.write_text("__const first,second;streamout out;mov eax,[first];mov ebx,[second];movd xmm0,eax;movaps out,xmm0;")
            result = audit.probe_source(PROBE, path, 20)
            self.assertEqual(result["frontend"]["status"], "pass")
            self.assertEqual(result["engine_pointers"], ["first", "second"])
            self.assertEqual(result["interpreter"]["status"], "requires-bindings")
            self.assertEqual(audit.classify(result, path.read_bytes()), "engine-bindings-required")

    @private_references
    def test_unchanged_pointer_cell_kernels_require_typed_context(self):
        for arch in ("x86", "x64"):
            path = ROOT / "tests/fixtures/flowstone" / f"engine-sample-index-double-{arch}.asm"
            result = audit.probe_source(PROBE, path, 20)
            self.assertEqual(result["frontend"]["status"], "pass")
            self.assertEqual(result["engine_pointers"], ["p_sampleIndexAddress"])
            self.assertEqual(result["interpreter"]["status"], "requires-bindings")
            self.assertEqual(audit.classify(result, path.read_bytes()), "engine-bindings-required")

    @private_references
    def test_native_transfer_loops_advance_to_their_caller_contract(self):
        for arch in ("x86", "x64"):
            original = ROOT / "tests/fixtures/flowstone" / f"engine-buffer-transfer-{arch}.asm"
            result = audit.probe_source(PROBE, original, 20)
            self.assertEqual(result["frontend"]["status"], "fail")
            self.assertIn("balance push and pop", result["frontend"]["message"])
            with tempfile.TemporaryDirectory() as temporary:
                wrapped = Path(temporary) / "caller.asm"
                wrapped.write_bytes(b"streamout out;" + original.read_bytes() +
                    b"pop rdi;pop rdx;pop rcx;pop rbx;movaps xmm0,_ZERO;movaps out,xmm0;")
                result = audit.probe_source(PROBE, wrapped, 20)
                self.assertEqual(result["frontend"]["status"], "pass")
                self.assertEqual(result["engine_pointers"], ["p_counter", "p_inOffsets", "p_inBuffers", "p_outOffsets", "p_outBuffers"])
                self.assertEqual(result["interpreter"]["status"], "requires-bindings")
                self.assertEqual(audit.classify(result, wrapped.read_bytes()), "engine-bindings-required")

    def test_unused_engine_declaration_does_not_need_binding(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "unused.asm"
            path.write_text("__const unused;streamout out;movaps xmm0,_ONE;movaps out,xmm0;")
            result = audit.probe_source(PROBE, path, 20)
            self.assertEqual(result["engine_pointers"], ["unused"])
            self.assertEqual(result["interpreter"]["status"], "pass")
            self.assertEqual(audit.classify(result, path.read_bytes()), "llvm-unavailable" if result["llvm_orc"]["status"] == "unavailable" else "pass")

    def test_compile_and_diagnostics(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "test.asm"
            path.write_text("streamin in;streamout out;movaps xmm0,in;movaps out,xmm0;")
            result = audit.probe_source(PROBE, path, 20)
            self.assertEqual(result["frontend"]["status"], "pass")
            self.assertEqual(result["interpreter"]["status"], "pass")
            words = json.loads(subprocess.check_output([str(PROBE), "--words"], text=True))
            self.assertEqual(result["llvm_orc"]["status"], "pass" if words["llvm_orc_available"] else "unavailable")
            path.write_text("streamout out;\nvbadps xmm0,xmm0;\n")
            failure = audit.probe_source(PROBE, path, 20)
            self.assertEqual(failure["frontend"]["status"], "fail")
            self.assertEqual(failure["frontend"]["line"], 2)
            self.assertIn("vbadps", failure["frontend"]["message"])
            self.assertNotIn("llvm_orc", failure)

    def test_report_end_to_end(self):
        source = b"streamin in;streamout out;movaps xmm0,in;movaps out,xmm0;"
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "Modules").mkdir()
            (root / "Modules/test.hom").write_bytes(record(source))
            (root / "x64").mkdir()
            (root / "x64/FlowStone.exe").write_bytes(b"streamout out;mov eax,1;test eax,7;xor eax,eax;vbadps xmm0,xmm0;\0")
            base = root / "reports/audit"
            subprocess.run([sys.executable, str(ROOT / "tools/audit_flowstone_assembler.py"), "--flowstone-root", str(root), "--probe", str(PROBE), "--output", str(base)], check=True, capture_output=True)
            report = json.loads(base.with_suffix(".json").read_text())
            self.assertEqual(report["summary"]["module-record"]["frontend_pass"], 1)
            self.assertEqual(report["summary"]["kernel-string"]["frontend_pass"], 0)
            self.assertIn("vbadps", report["unlisted_mnemonics"])
            self.assertNotIn("test", report["unlisted_mnemonics"])
            self.assertNotIn("xor", report["unlisted_mnemonics"])
            kernel = next(block for block in report["blocks"] if block["kind"] == "kernel-string")
            self.assertIn("vbadps", kernel["compile"]["frontend"]["message"])
            self.assertEqual(len(report["probe_sha256"]), 64)
            self.assertIn("src/stream/DspCode.hpp", report["implementation_sha256"])
            self.assertIn("src/stream/Runtime.hpp", report["implementation_sha256"])
            self.assertIn("does not establish numerical parity", base.with_suffix(".md").read_text())
            self.assertTrue(base.with_suffix(".csv").exists())


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", type=Path)
    args = parser.parse_args()
    PROBE = args.probe.resolve() if args.probe else None
    # skipUnless is evaluated at import time; enable integration tests here.
    ProbeTest.__unittest_skip__ = PROBE is None
    unittest.main(argv=[sys.argv[0]])
