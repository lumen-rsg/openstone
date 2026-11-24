#!/usr/bin/env python3
"""Compile recovered FlowStone sources without changing or executing their bodies.

No FlowStone package is redistributed. Supply an unpacked build and a probe built
from the OpenStone tree being audited. Only Python's standard library is needed.
"""
import argparse
import collections
import csv
import hashlib
import json
import platform
import re
import subprocess
import tempfile
from pathlib import Path

PRINTABLE = re.compile(rb"[\t\r\n\x20-\x7e]{24,}")
COMMENTS = re.compile(rb"/\*.*?\*/|//[^\r\n]*", re.S)
PORT = re.compile(rb"\b(?:stream(?:double|bool)?|mono(?:double|bool)?|poly(?:double|bool|int)?|mem(?:array)?ref|mem|floatarray|intarray)(in|out)\s+([^;]+);")
ASSEMBLY = re.compile(rb"(?im)(?:^|[;\r\n{}])\s*(?:[a-z][a-z0-9]*\s+[^;\r\n]*\b(?:xmm\d+|[er](?:ax|bx|cx|dx|si|di|bp|sp)|st\([0-7]\))|(?:fld|dfld|fst|fstp|dfst|dfstp|cmp|jmp|jnz|jz|ret)\b[^;\r\n]*)\s*;")
DECLARATION = re.compile(r"^(?:float|double|int|bool|__const|(?:stream(?:double|bool)?|mono(?:double|bool)?|poly(?:double|bool|int)?|mem(?:array)?ref|mem|floatarray|intarray)(?:in|out))\b")


def strip_comments(source):
    # Keep line numbers intact for lexical inventory and source excerpts.
    return COMMENTS.sub(lambda m: b"\n" * m.group().count(b"\n"), source)


def has_assembly(source):
    return bool(ASSEMBLY.search(strip_comments(source)))


def port_counts(source):
    counts = collections.Counter()
    for match in PORT.finditer(strip_comments(source)):
        names = match[2].split(b",")
        if all(re.fullmatch(rb"\s*[A-Za-z_]\w*\s*(?:\[\s*\d+\s*\])?\s*", name) for name in names):
            counts[match[1]] += len(names)
    return counts[b"in"], counts[b"out"]


def source_records(data, require_counts=True):
    """Recognize plaintext serialized source fields, not the whole .hom format.

    The two bytes before the string prefix store inlet/outlet counts. Strings
    have a one-byte length or FF + little-endian uint16 length. Length bytes
    can themselves be printable, so inspect the first eight possible starts.
    """
    for run in PRINTABLE.finditer(data):
        for start in range(run.start(), min(run.end(), run.start() + 8)):
            prefixes = []
            if start >= 3 and data[start - 3] == 255:
                prefixes.append((start - 3, int.from_bytes(data[start - 2:start], "little")))
            if start >= 1 and data[start - 1] != 255:
                prefixes.append((start - 1, data[start - 1]))
            for prefix, length in prefixes:
                end = start + length
                if prefix < 2 or length < 24 or end > run.end():
                    continue
                source = data[start:end]
                counts = port_counts(source)
                stored_counts = tuple(data[prefix - 2:prefix])
                if not sum(counts) or (counts != stored_counts and (require_counts or stored_counts != (0, 0))):
                    continue
                # An unused embedded component can store zero connector counts.
                # Still require a real declaration start, not a printable length
                # byte accidentally glued to the first keyword.
                if counts != stored_counts and not re.match(rb"(?:stream|mono|poly|mem|floatarray|intarray)[a-z]*(?:in|out)\b|#\s*COMPATIBILITY_3_0_8\b|(?:float|double|int|bool)\b", strip_comments(source).lstrip()):
                    continue
                if has_assembly(source):
                    yield start, source
                    break
            else:
                continue
            break


def instruction_statements(source):
    """Lexical census independent of the compiler's first-error stop.

    It is a census, not a second Assembler parser. Declarations, labels, stage
    directives and comments are excluded. Format fragments remain fragments.
    """
    text = strip_comments(source).decode("utf-8", errors="replace")
    for part in re.split(r"[;{}]", text):
        part = re.sub(r"(?m)^\s*#.*$", "", part).strip()
        while re.match(r"^\w+\s*:", part):
            part = re.sub(r"^\w+\s*:\s*", "", part, count=1)
        part = re.sub(r"^stage\s*(?:\(\s*\d+\s*\)|\d+)\s*", "", part).strip()
        if not part or DECLARATION.match(part):
            continue
        match = re.match(r"([a-zA-Z][a-zA-Z0-9]*)\b(.*)", part, re.S)
        if match:
            yield match[1].lower(), " ".join(match[2].split())


def context_flags(source):
    text = strip_comments(source).decode("utf-8", errors="replace")
    flags = []
    if "__const" in text:
        flags.append("engine-pointer-bindings")
    declared = set(re.findall(r"\b(?:float|double|int|bool)\s+([A-Za-z_]\w*)", text))
    if set(re.findall(r"\b_[A-Z][A-Z0-9_]*\b", text)) - declared:
        flags.append("engine-symbols")
    if re.search(r"%(?:\d+\$)?[0-9.*+-]*[diuxXs]", text):
        flags.append("format-template")
    if re.search(r"\b(?:rbp|ebp|rsi|esi|rdi|edi|rsp|esp)\b", text):
        flags.append("native-frame-or-register-context")
    if not any(direction == b"out" for direction, _ in PORT.findall(strip_comments(source))):
        flags.append("no-output-declarations")
    return flags


def recover(root):
    records = {}
    scanned = []

    def add(kind, path, data, offset, source):
        digest = hashlib.sha256(source).hexdigest()
        record = records.setdefault((kind, digest), {"kind": kind, "sha256": digest,
            "length": len(source), "source": source, "occurrences": []})
        occurrence = {"file": path.relative_to(root).as_posix(),
            "file_sha256": hashlib.sha256(data).hexdigest(), "offset": offset, "length": len(source)}
        if kind in ("module-record", "embedded-record"):
            prefix = offset - (3 if offset >= 3 and data[offset - 3] == 255 else 1)
            occurrence["stored_port_counts"] = list(data[prefix - 2:prefix])
            occurrence["declared_port_counts"] = list(port_counts(source))
        record["occurrences"].append(occurrence)

    modules = sorted((root / "Modules").rglob("*.hom"))
    if not modules:
        raise ValueError("No Modules/*.hom files found in the FlowStone build")
    for path in modules:
        data = path.read_bytes()
        scanned.append({"file": path.relative_to(root).as_posix(), "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data)})
        for offset, source in source_records(data):
            add("module-record", path, data, offset, source)
    for path in sorted((root / "Snippets" / "Asm").rglob("*.txt")):
        data = path.read_bytes()
        scanned.append({"file": path.relative_to(root).as_posix(), "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data)})
        if has_assembly(data):
            add("asm-snippet", path, data, 0, data)
    for arch in ("x64", "x86"):
        path = root / arch / "FlowStone.exe"
        if not path.exists():
            continue
        data = path.read_bytes()
        scanned.append({"file": path.relative_to(root).as_posix(), "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data)})
        embedded = list(source_records(data, require_counts=False))
        for offset, source in embedded:
            add("embedded-record", path, data, offset, source)
        for run in PRINTABLE.finditer(data):
            if any(run.start() <= offset < run.end() for offset, _ in embedded):
                continue
            # Null-terminated source strings include complete kernels and code
            # fragments assembled by FlowStone. Do not pretend they stand alone.
            if data[run.end():run.end() + 1] == b"\0" and has_assembly(run.group()):
                add("kernel-string", path, data, run.start(), run.group())
    return sorted(records.values(), key=lambda item: (item["kind"], item["sha256"])), scanned


def classify(result, source):
    front = result["frontend"]
    if front["status"] != "pass":
        message = front.get("message", "").lower()
        if front["status"] in ("timeout", "probe-error", "error"):
            return "probe-" + front["status"]
        if "__const" in message or "unknown dsp name" in message:
            return "unbound-name-or-engine-context"
        if "before reading" in message:
            return "incoming-register-state"
        if "label" in message or "branch" in message or "subroutine" in message:
            return "control-flow"
        if "instruction" in message or "assembler mnemonic" in message:
            return "instruction-or-syntax"
        if "packed width" in message or "lane" in message or "operand" in message:
            return "operand-or-width"
        return "frontend-other"
    if any(result[name]["status"] == "fail" for name in ("interpreter", "llvm_orc")):
        return "backend-failure"
    if any(result[name]["status"] == "requires-bindings" for name in ("interpreter", "llvm_orc")):
        return "engine-bindings-required"
    if result["llvm_orc"]["status"] == "unavailable":
        return "llvm-unavailable"
    if result["llvm_orc"]["status"] == "no-output":
        return "frontend-only-no-output"
    return "pass"


def probe_source(probe, path, timeout):
    try:
        run = subprocess.run([str(probe), str(path)], capture_output=True, text=True, timeout=timeout)
        if run.returncode:
            return {"frontend": {"status": "probe-error", "message": f"Exit {run.returncode}: {run.stderr.strip()[:600]}"}}
        try:
            return json.loads(run.stdout)
        except (json.JSONDecodeError, UnicodeError):
            return {"frontend": {"status": "probe-error", "message": "Probe did not return valid JSON"}}
    except subprocess.TimeoutExpired:
        return {"frontend": {"status": "timeout", "message": f"Exceeded {timeout:g} seconds; no samples rendered"}}


def location(item):
    first = item["occurrences"][0]
    return f"{first['file']} @ 0x{first['offset']:x}"


def write_reports(base, report):
    base.parent.mkdir(parents=True, exist_ok=True)
    base.with_suffix(".json").write_text(json.dumps(report, indent=2) + "\n")
    with base.with_suffix(".csv").open("w", newline="") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(["Kind", "Source SHA256", "Occurrences", "Frontend", "Interpreter", "LLVM ORC", "Category", "Line", "Column", "Diagnostic", "First location", "Context flags", "Unlisted mnemonics"])
        for item in report["blocks"]:
            result = item["compile"]
            front = result["frontend"]
            writer.writerow([item["kind"], item["sha256"], len(item["occurrences"]), front["status"],
                result.get("interpreter", {}).get("status", "not-reached"), result.get("llvm_orc", {}).get("status", "not-reached"),
                item["category"], front.get("line", ""), front.get("column", ""), front.get("message", ""), location(item),
                "; ".join(item["context_flags"]), "; ".join(item["unlisted_mnemonics"])])
    lines = ["# FlowStone Assembler compile audit", "", "Compile-only audit of unchanged recovered source. No samples were rendered; passing compilation does not establish numerical parity.", "",
        "## Coverage", "", "| Source category | Distinct sources | Instances | Frontend pass | Interpreter prepared | LLVM ORC compiled |", "|---|---:|---:|---:|---:|---:|"]
    for kind, counts in report["summary"].items():
        lines.append(f"| {kind} | {counts['sources']} | {counts['occurrences']} | {counts['frontend_pass']} | {counts['interpreter_pass']} | {counts['llvm_orc_pass']} |")
    lines.extend(["", f"Scanned {sum(f['file'].endswith('.hom') for f in report['input_files'])} module files, the Assembler snippets, and the executables listed in the JSON manifest. Exact source bytes are deduplicated within each category. Identical x86/x64 strings share one entry. No implicit ports, constants, caller-register prologues, or instruction rewrites were injected into the recovered sources. Compiler-owned entry bindings apply, including OpenStone's zero fallback for unbound incoming XMM registers. This fallback is a portable policy, not a verified native scratch-register value.", "",
        "Module records are recognized by their stored string length and inlet/outlet counts. Embedded executable records also accept a zero/zero connector header when the exact length and declaration boundaries match; unused component copies have these headers. Kernel strings are null-terminated executable strings that look like assembly; some are fragments, format templates or require FlowStone engine bindings. This is a recovered plaintext corpus, not a full schematic/resource decoder or an exhaustive instruction/operand matrix.", "",
        "## First diagnostic groups", "", "The compiler stops at the first error. The independent mnemonic census scans the whole recovered source; unlisted names inside engine fragments are leads, not proof that a standalone Assembler block requires them.", "", "| Category | Sources |", "|---|---:|"])
    counts = collections.Counter(item["category"] for item in report["blocks"])
    lines.extend(f"| {name} | {count} |" for name, count in sorted(counts.items()))
    lines.extend(["", "## Unlisted mnemonic census", "", "| Name | Distinct source hashes | First location |", "|---|---:|---|"])
    for name, entry in report["unlisted_mnemonics"].items():
        lines.append(f"| `{name}` | {len(entry['source_sha256'])} | {entry['first_location']} |")
    if not report["unlisted_mnemonics"]:
        lines.append("| None | 0 | — |")
    lines.extend(["", "## Failed self-contained records", "", "Kernel strings are excluded from this table; all diagnostics and locations remain in CSV/JSON.", "", "| Category | SHA-256 prefix | First location | First diagnostic |", "|---|---|---|---|"])
    for item in report["blocks"]:
        if item["kind"] == "kernel-string" or item["category"] == "pass":
            continue
        diagnostic = item["compile"]["frontend"].get("message", item["category"])
        lines.append(f"| {item['kind']} | `{item['sha256'][:12]}` | {location(item)} | {diagnostic.replace('|', '/').replace(chr(10), ' ')} |")
    lines.extend(["", "## Reproduce", "", "Build `openstone_assembler_probe` with `OPENSTONE_BUILD_ASSEMBLER_AUDIT=ON` and `OPENSTONE_ENABLE_LLVM_ORC=ON`, then:", "", "```sh", "python3 tools/audit_flowstone_assembler.py --flowstone-root /path/to/unpacked/FlowStone \\", "  --probe /path/to/build/openstone_assembler_probe \\", "  --output /path/to/report/flowstone-assembler-audit", "```", "", "Use `--extract-dir /path/to/local/corpus` to retain the unchanged extracted sources. Every source and containing file has a SHA-256 and every occurrence has a byte offset. The JSON also hashes the probe and compiler sources to identify the exact audited implementation. Backend statuses `unavailable` and `requires-bindings` are never counted as passes. The latter means the source compiled but requires explicit caller-owned `__const` resources; the probe does not invent null/default bindings. Sources are compiled in separate processes with a timeout; stage 0 and rendering are never executed.", ""])
    base.with_suffix(".md").write_text("\n".join(lines))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--flowstone-root", type=Path, required=True)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="Report path without extension")
    parser.add_argument("--extract-dir", type=Path)
    parser.add_argument("--timeout", type=float, default=30)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    probe = args.probe.resolve()
    words = json.loads(subprocess.check_output([str(probe), "--words"], text=True, timeout=args.timeout))
    known = set(words["mnemonics"])
    blocks, files = recover(args.flowstone_root.resolve())
    cache, census = {}, {}
    with tempfile.TemporaryDirectory(prefix="openstone-asm-audit-") as temporary:
        extract = args.extract_dir or Path(temporary)
        extract.mkdir(parents=True, exist_ok=True)
        for item in blocks:
            source = item.pop("source")
            path = extract / (item["sha256"] + ".asm")
            path.write_bytes(source)
            if item["sha256"] not in cache:
                cache[item["sha256"]] = probe_source(probe, path, args.timeout)
            result = cache[item["sha256"]]
            item["compile"] = result
            item["category"] = classify(result, source)
            item["context_flags"] = context_flags(source)
            instructions = list(instruction_statements(source))
            item["mnemonics"] = sorted(set(name for name, _ in instructions))
            item["unlisted_mnemonics"] = sorted(set(item["mnemonics"]) - known)
            for name in item["unlisted_mnemonics"]:
                entry = census.setdefault(name, {"source_sha256": set(), "first_location": location(item)})
                entry["source_sha256"].add(item["sha256"])
            front = result["frontend"]
            if front.get("line"):
                source_lines = source.decode("utf-8", errors="replace").splitlines()
                item["diagnostic_excerpt"] = source_lines[front["line"] - 1][:240] if front["line"] <= len(source_lines) else ""
    summary = {}
    for item in blocks:
        counts = summary.setdefault(item["kind"], {"sources": 0, "occurrences": 0, "frontend_pass": 0, "interpreter_pass": 0, "llvm_orc_pass": 0})
        counts["sources"] += 1
        counts["occurrences"] += len(item["occurrences"])
        for label, name in (("frontend_pass", "frontend"), ("interpreter_pass", "interpreter"), ("llvm_orc_pass", "llvm_orc")):
            counts[label] += item["compile"].get(name, {}).get("status") == "pass"
    source_root = Path(__file__).resolve().parent.parent
    compiler_files = ["tools/AssemblerCompileProbe.cpp", "tools/audit_flowstone_assembler.py", "src/stream/DspCode.cpp", "src/stream/DspCode.hpp", "src/stream/DspInterpreter.cpp", "src/stream/DspOrc.cpp", "src/stream/DspX87.cpp", "src/stream/DspProgramData.hpp", "src/stream/Runtime.cpp", "src/stream/Runtime.hpp", "src/stream/AssemblerSampleClock.hpp", "src/stream/PolyRuntime.cpp", "src/stream/PolyRuntime.hpp", "src/stream/PolyRenderer.cpp", "src/stream/PolyRenderer.hpp"]
    report = {"schema_version": 1, "scope": "compile-only, unchanged recovered plaintext sources; no rendering or numerical parity claim",
        "host": {"system": platform.system(), "machine": platform.machine()},
        "probe_sha256": hashlib.sha256(probe.read_bytes()).hexdigest(), "llvm_orc_available": words["llvm_orc_available"],
        "implementation_sha256": {name: hashlib.sha256((source_root / name).read_bytes()).hexdigest() for name in compiler_files},
        "input_files": files, "summary": summary, "compiler_mnemonics": sorted(known),
        "unlisted_mnemonics": {name: {**entry, "source_sha256": sorted(entry["source_sha256"])} for name, entry in sorted(census.items())}, "blocks": blocks}
    write_reports(args.output, report)
    print(json.dumps({"summary": summary, "unlisted_mnemonics": sorted(census), "report": str(args.output.with_suffix('.md'))}, indent=2))
    if any(item["category"].startswith("probe-") for item in blocks):
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
