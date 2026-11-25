"""Import unchanged bundled module records without redistributing vendor files.

The .hom library header is not a document. Wrap its verified module body in an
independently authored .fsm document; never alter the module/code/cache bytes.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import zipfile

MODULES = {
    'Delay': (159, 0x12a, '12d551c5d830a33727384a5ada4ead380af37fa1ac53ec1fa76cda826d07f022'),
    'Denormal Remover (Bitmask)': (170, 0xfe, 'da3074ce6708ea4e8793e826f35188e7c3028b7ed53baadb9876c06a9de34303'),
    'Clip': (159, 0x6f, '276f1988525bfa41c9c7cae5812544005966d22a454cfed30b771bda95b28a93'),
    'Stream Biquad (DF1)': (169, 0xaf, 'e4584d120ec16835e17366598da66f07beeaebaa16bd26b1b63e48a91b840f88'),
    'Double Stream Biquad (DF1)': (169, 0xbd, '5b20279d8e533f686352b77c29d15bebee1db6982a96c8caacd7f4f201030d83'),
    'Phase Invert': (159, 0x61, '66099ec5c4e52db53df201565eb8080da0a5f76710933e271c2a6ad9529af631'),
    '0-1 To Hz': (159, 0x8e, '59520cf060b666482005b3ea91a6083ea138ba2a387e6f4858c0e645572d9cda'),
    'Hz To 0-1': (159, 0x8e, 'f58b4c526f17f9c5eb6321efec904b01450c6db7dff637ec9edbfa159360f1af'),
    'DeZip': (169, 0x7a, '26f5164fdb228ac61e65ef159649b941ad1cc909f65d678b3a47627d40d4b931'),
    'Stream Biquad (DF2)': (169, 0xb0, 'f1974604694659b7e7fe81a4d934ef2dc78a4ce7a976f0c52241cec48a4e937d'),
    'Stream Biquad (TDF1)': (169, 0xbb, '6a24b74c285bcb9d89de6c08ca4e2493de16d007ff02d037b6d8c2049402dae5'),
    'Stream Biquad (TDF2)': (169, 0xbc, '028e0ba86b02fcf1aa4ea3bac64063cf938e4b353918be9438b50fc68c9ef27d'),
    'Double Stream Biquad (DF2)': (169, 0xbe, 'c39fb0bd185379ec4ed2defef766feaea1fd3aa45502f0d82f7951e1f4f9618f'),
    'Double Stream Biquad (TDF1)': (169, 0xc9, '501fb91bafdcf99ab6c7886dabe3264e9671dd1905931513151c0c2c90ac6f94'),
    'Double Stream Biquad (TDF2)': (169, 0xca, 'a6936a1b797ef54a693aa3785c77fe36e8ed5af764b88d9acf86e39a9bb01719'),
    'Denormal Remover (Noise)': (170, 0xd5, 'bc2626a1c1bc2ed1fad0d2d18841aab006e68e4974d8472842e2a937bbc3a332'),
    'Denormal Remover (Trunc)': (170, 0xe4, '7cc43c8ebda5761ae7460129d042c8bdc0dd21e8b211f8d4936b25996b025065'),
    'FM De-modulator': (159, 0x7a, 'd5d5dd5de4e6d655abb1afcaf8a1556f410908fefba110bb13e829e8e2dd6ad6'),
}

# Standalone component records copied unchanged from otherwise not-yet-importable GUI modules.
GUI_RECORDS = {
    'Amp GUI': ('Amp', 0x301, 26, 'b7ef92876488c00f1d16bee4d11cf2dbccaeb29904a6bb05ffd9ca2b5645dc28'),
    'Label GUI': ('Label', 0x8b, 42, '6049d5d91e8d84e6d678d5eee00a7d8e8646b96abd1f7f29cf7a1ca24c0c1e3d'),
    'LED GUI': ('LED-Indicator', 0xf7, 42, 'f08e9ab0041a87ebae2425cd87669c1591689ac5e80965071a9fbf4ff43f2165'),
}

DRAWING_RECORDS = {
    'Edit Box Pen': ('Edit Box', 0xb44, 25, '601e3b09ee5dda7e748e065fea539e9e5e3a83d015e44b8bcf38fa3c693ed085'),
    'LED Pen': ('LED-Indicator', 0x6d2, 21, 'f08e9ab0041a87ebae2425cd87669c1591689ac5e80965071a9fbf4ff43f2165'),
    'Label Pen': ('Label', 0x549, 21, '6049d5d91e8d84e6d678d5eee00a7d8e8646b96abd1f7f29cf7a1ca24c0c1e3d'),
    'Label Font': ('Label', 0x408, 218, '6049d5d91e8d84e6d678d5eee00a7d8e8646b96abd1f7f29cf7a1ca24c0c1e3d'),
    'Stream Power Font': ('Stream Power', 0x778, 23, '89d738929fa54fe0b69ec5f2baaee1c3a37ff1b5178b1a158f603413b521a561'),
    'Text View Font': ('Text View', 0x39c, 27, 'e31486c45f2660c049a6cf2ab34b77a7c5a9087833c10e6be5ea1b6aace0ff6f'),
}

FORMAT_RECORDS = {
    'Label String Format': ('Label', 0x6e2, 216, '6049d5d91e8d84e6d678d5eee00a7d8e8646b96abd1f7f29cf7a1ca24c0c1e3d'),
    'Text State String Format': ('Text State', 0x71b, 216, 'f9d5a332b8464be5f924473329936ebffee85b828be81ec7732d6d7389c4ea03'),
}

SHAPE_RECORDS = {
    'Group Adornment Round Rectangle Legacy': ('Group Adornment', 0xc2, 53, '48d0a41d8a503404e39f8035ac12e4c5f4c77c757b84a06f33bb5b8ce8417ce4'),
    'Group Adornment Round Rectangle': ('Group Adornment', 0xfb, 98, '48d0a41d8a503404e39f8035ac12e4c5f4c77c757b84a06f33bb5b8ce8417ce4'),
    'Graph Shaper Rect': ('Graph Shaper', 0xe7a0, 218, '0a22147909c80732898cb400ea0a31427113d77616066069693b7ba2b3492db0'),
    'Float Graph Recorder Rect': ('Float Graph Recorder', 0x174, 219, 'e224c0d0dca75019cb65cdbd1ba257dd8db988b0ee409ea2611df631445c496f'),
    'Group Adornment Ellipse': ('Group Adornment', 0x23a, 220, '48d0a41d8a503404e39f8035ac12e4c5f4c77c757b84a06f33bb5b8ce8417ce4'),
    'Parametric EQ Ellipse': ('Parametric EQ', 0x63f2, 221, 'd8147a654edf25def02e4008b9b34f7589347fb6a2af78b9b38db20f6832667b'),
    'Multi Sample Arc': ('Multi Sample', 0x10ef8, 109, '00556438065154ac7bfeb03251f9b0fd865d0f7d057856baab465b2f0b19e871'),
}

COMPARISON_RECORDS = {
    'Moog Filter Equal': ('Moog Filter',0x1c2,1048,'a81d93edb62babf2f90b2110bbfe3946a07cd4b925ab6179cdec3be1a8e04da8'),
    'Multi Sample Less Than': ('Multi Sample',0x127be,1049,'00556438065154ac7bfeb03251f9b0fd865d0f7d057856baab465b2f0b19e871'),
    'Multi Sample Less Than or Equal': ('Multi Sample',0x12ce5,1050,'00556438065154ac7bfeb03251f9b0fd865d0f7d057856baab465b2f0b19e871'),
    'Hysteresis Gate Greater Than': ('Hysteresis Gate',0x1bc,1051,'824b1ccae8579fec60088294753eba1c96696ab27e4e87fd2df68352c22aeb04'),
    'Hysteresis Gate Greater Than or Equal': ('Hysteresis Gate',0x1e7,1052,'824b1ccae8579fec60088294753eba1c96696ab27e4e87fd2df68352c22aeb04'),
}

ROUTING_RECORDS = {
    'Step LFO Trigger Blocker': ('Step LFO', 0x60c, 35, 1056, '47ee149d5474ca5f62d93a975fe398f88e1ce39d5859c810074ac1208dc146d4'),
    'Poly Stream Record Samples Trigger Switch': ('Poly Stream Record Samples', 0x77, 35, 1060, '265abde2ea2cae6239d762b716b967e2cc1867755b92b6d620a9f6f76890efeb'),
    'Reverb Float Select': ('Reverb', 0x41e1, 48, 1057, '290717424881e49e5027310f3cb240ff0f7b8019adf70f70da30dd7e98dbd841'),
    'Reverb String Select': ('Reverb', 0x15f11, 48, 1057, '290717424881e49e5027310f3cb240ff0f7b8019adf70f70da30dd7e98dbd841'),
}

STATE_RECORDS = {
    'Fast Power Index Selector': ('Fast Power', 0x6c2, 97, 408, '606bb50744debd6fc7f368c5ebe0290d5d2d9a5bab06ac4610a77ffd34a478af'),
    'Bitmap Index Selector': ('Bitmap', 0x1d9a, 121, 408, '989eebccab1c4af3abb4b23a8d6e8b2b6c2dba77b5a07bdaf860aacf7c3b3a37'),
    'Float Graph Recorder Index Selector': ('Float Graph Recorder', 0x4002, 131, 408, 'e224c0d0dca75019cb65cdbd1ba257dd8db988b0ee409ea2611df631445c496f'),
    'Hysteresis Gate Counter': ('Hysteresis Gate', 0x1a1, 11, 326, '824b1ccae8579fec60088294753eba1c96696ab27e4e87fd2df68352c22aeb04'),
    'Ticker Custom After Load': ('Ticker - Custom', 0x580, 7, 258, '37d23e44a15e8c1bf79c553cea9a963c9a4cf7f3cf5554bb75c1faac139f4a41'),
}

AUDIO_RECORDS = {
    'Echo Delay native Delay': ('Echo Delay', 0x4b4, 77, '1f4eb57375b5dfabe799c25596f111855a3048aec5cf0f0aea97e04ac96946d6'),
    'Multi Osc native Sine': ('Multi Osc', 0xd5, 33, '7ff84f38208052387da2ab9f4232307b6fe81d295a7263099ebb18428d1529b9'),
    'Multi LFO native Sine': ('Multi LFO', 0xb3, 33, '8009c16ef388eaf3b80323cb4ce050d0c20a67f43a18f7925dd5fc278f533f2d'),
    'State Variable Filter Pitch to Freq': ('State Variable Filter', 0x3c8, 35, 'd74ff76e4bef1eb365db122a691bd3c5742531780d93d5d2ed7277031c47d4f7'),
    'Moog Filter Pitch to Freq': ('Moog Filter', 0x43a, 35, 'a81d93edb62babf2f90b2110bbfe3946a07cd4b925ab6179cdec3be1a8e04da8'),
}

GENERATOR_RECORDS = {
    'Multi Osc Triangle': ('Multi Osc', 0xdc, 165, '7ff84f38208052387da2ab9f4232307b6fe81d295a7263099ebb18428d1529b9'),
    'Multi Osc Sawtooth': ('Multi Osc', 0x2f1, 27, '7ff84f38208052387da2ab9f4232307b6fe81d295a7263099ebb18428d1529b9'),
    'Multi LFO Triangle': ('Multi LFO', 0xba, 165, '8009c16ef388eaf3b80323cb4ce050d0c20a67f43a18f7925dd5fc278f533f2d'),
    'Multi LFO Sawtooth': ('Multi LFO', 0x13d, 27, '8009c16ef388eaf3b80323cb4ce050d0c20a67f43a18f7925dd5fc278f533f2d'),
    'PWM Triangle': ('PWM', 0xbd, 165, '9935be89d99054fc61e99d626a96ce9367bfbccd687a58d3bec4a84a6b226e23'),
}

SQUARE_MODULES = {
    'Multi Osc': (0xf2, 0x1f7, '7ff84f38208052387da2ab9f4232307b6fe81d295a7263099ebb18428d1529b9'),
    'Multi LFO': (0x144, 0x226, '8009c16ef388eaf3b80323cb4ce050d0c20a67f43a18f7925dd5fc278f533f2d'),
    'PWM': (0xd3, 0x1b5, '9935be89d99054fc61e99d626a96ce9367bfbccd687a58d3bec4a84a6b226e23'),
}


DROP_LIST_RECORDS = {
    'Multi LFO Drop List': ('Multi LFO', 0x7eb, 0, '8009c16ef388eaf3b80323cb4ce050d0c20a67f43a18f7925dd5fc278f533f2d'),
    'Detuner Drop List': ('Detuner', 0xcb4, 5, 'bffc0fd90a7779790d4d88bb98c2a46c10b1e01bc80c53051a13e6b9f3301f50'),
    'Sample and Hold Drop List': ('Sample and Hold', 0x601, 0, 'cce3e17254aa7f9c54dbd02a785a0778c5b7e7fe0fc0481471edd3763b8b86ee'),
    'PWM Drop List': ('PWM', 0x800, 1, '9935be89d99054fc61e99d626a96ce9367bfbccd687a58d3bec4a84a6b226e23'),
}

# These contiguous records retain the bundled Items String, Index boundary and
# Drop List bytes. The test bench supplies their two authored functional links.
DROP_LIST_ITEMS = {
    'Multi LFO': (0x75f, 0x7f6, '8009c16ef388eaf3b80323cb4ce050d0c20a67f43a18f7925dd5fc278f533f2d'),
    'Detuner': (0xc2b, 0xcbf, 'bffc0fd90a7779790d4d88bb98c2a46c10b1e01bc80c53051a13e6b9f3301f50'),
    'Sample and Hold': (0x57d, 0x60c, 'cce3e17254aa7f9c54dbd02a785a0778c5b7e7fe0fc0481471edd3763b8b86ee'),
}


# Preset Float (189): count + 32 Float words + lock/current/order/recording cache.
# Only hashes and byte locations are public; the native library stays external.
PRESET_RECORDS = {
    'Multi LFO': (0x834, '8009c16ef388eaf3b80323cb4ce050d0c20a67f43a18f7925dd5fc278f533f2d'),
    'Detuner': (0xcfd, 'bffc0fd90a7779790d4d88bb98c2a46c10b1e01bc80c53051a13e6b9f3301f50'),
    'Sample and Hold': (0x64a, 'cce3e17254aa7f9c54dbd02a785a0778c5b7e7fe0fc0481471edd3763b8b86ee'),
}


# Native Multiplexer is ID 149; 1054 is Last Switch, despite an earlier audit note.
NATIVE_ROUTER_RECORDS = {
    'Alpha Switch Multiplexer': ('Alpha Switch', 0xbe, 15, '3aa0ec27a8081e3fc9c9dd2ef71b869f4371b73176f3ff5b9ff2be5bdf619cce'),
    'Timed Counter Multiplexer': ('0-1 Timed Counter', 0xe4e, 15, '1352145ca7798c5791b6363996bd2e60a773dabf30735cecb1d0d1a16ecc6f36'),
    'Multi LFO Last Switch second': ('Multi LFO', 0x99c, 44, '8009c16ef388eaf3b80323cb4ce050d0c20a67f43a18f7925dd5fc278f533f2d'),
    'Multi LFO Last Switch first': ('Multi LFO', 0xbe8, 44, '8009c16ef388eaf3b80323cb4ce050d0c20a67f43a18f7925dd5fc278f533f2d'),
    'Detuner Last Switch second': ('Detuner', 0xe63, 44, 'bffc0fd90a7779790d4d88bb98c2a46c10b1e01bc80c53051a13e6b9f3301f50'),
    'Detuner Last Switch first': ('Detuner', 0x10b3, 44, 'bffc0fd90a7779790d4d88bb98c2a46c10b1e01bc80c53051a13e6b9f3301f50'),
    'Sample and Hold Last Switch second': ('Sample and Hold', 0x7b7, 44, 'cce3e17254aa7f9c54dbd02a785a0778c5b7e7fe0fc0481471edd3763b8b86ee'),
    'Sample and Hold Last Switch first': ('Sample and Hold', 0xa00, 44, 'cce3e17254aa7f9c54dbd02a785a0778c5b7e7fe0fc0481471edd3763b8b86ee'),
}


# Payload-free MIDI CC (166) and complete native CC/enable/normalization modules.
MIDI_CC_MODULES = {
    'Multi LFO': (0xc3f, 0xddb, 0xc5a, '8009c16ef388eaf3b80323cb4ce050d0c20a67f43a18f7925dd5fc278f533f2d'),
    'Detuner': (0x110a, 0x12a6, 0x1125, 'bffc0fd90a7779790d4d88bb98c2a46c10b1e01bc80c53051a13e6b9f3301f50'),
    'Sample and Hold': (0xa57, 0xbf3, 0xa72, 'cce3e17254aa7f9c54dbd02a785a0778c5b7e7fe0fc0481471edd3763b8b86ee'),
}


# Unchanged inline-PNG records. Vendor pixels remain in the external test corpus.
BITMAP_RECORDS = {
    'Multi LFO Speed Bitmap': ('Multi LFO', 0x2168, 4168, '8009c16ef388eaf3b80323cb4ce050d0c20a67f43a18f7925dd5fc278f533f2d'),
    'Multi LFO Depth Bitmap': ('Multi LFO', 0x493b, 4168, '8009c16ef388eaf3b80323cb4ce050d0c20a67f43a18f7925dd5fc278f533f2d'),
    'Detuner Bitmap': ('Detuner', 0x337e, 4215, 'bffc0fd90a7779790d4d88bb98c2a46c10b1e01bc80c53051a13e6b9f3301f50'),
}

FILE_DIALOG_RECORDS = {
    'Multi LFO Speed Bitmap': 0x31c3,
    'Multi LFO Depth Bitmap': 0x5996,
    'Detuner Bitmap': 0x4408,
}

BUTTON_RECORDS = {
    'Multi LFO Speed Button': ('Multi LFO', 0x31ca, 62, '8009c16ef388eaf3b80323cb4ce050d0c20a67f43a18f7925dd5fc278f533f2d'),
    'Multi LFO Depth Button': ('Multi LFO', 0x599d, 62, '8009c16ef388eaf3b80323cb4ce050d0c20a67f43a18f7925dd5fc278f533f2d'),
    'Detuner Button': ('Detuner', 0x440f, 62, 'bffc0fd90a7779790d4d88bb98c2a46c10b1e01bc80c53051a13e6b9f3301f50'),
    'Switch Animation Button': ('Switch', 0x8c0, 65, '49be7706ea2ce945bf37c5948bb601df4d0387abda15601c0c3ce384b7c6d0af'),
}

def gui_record_document(record, name, version, *, node_count=1, links=()):
    header = struct.pack('<HIIH', version, 1, 1, 0) + bytes(3) + b'\1' + bytes(12)
    if version >= 171: header += bytes(4)
    header += bytes(4)
    if version >= 161: header += bytes(8 + 6 * 8) + b'\1' + bytes(4)
    def module(label, children=1):
        return struct.pack('<HfffB', 1, 8, 0, 0, len(label)) + label.encode('ascii') + struct.pack('<HHB', 10, 10, children)
    wires = b''.join(struct.pack('<BHBHB', 0, source, output, target, input_port) + bytes(2 + (2 if version >= 165 else 0) + (1 if version >= 174 else 0))
                     for source, output, target, input_port in links)
    child_footer = struct.pack('<H', len(links)) + wires + struct.pack('<II', 6, 4) + bytes([0, 254, 0, 0, 0, 1])
    root_footer = struct.pack('<HII', 0, 6, 4) + bytes(6) + bytes(3)
    return header + module('Root') + module(name, node_count) + record + child_footer + root_footer


def wrap(data, name, version, offset):
    assert struct.unpack_from('<H', data)[0] == version
    signature = struct.pack('<HfffB', 1, 8, 0, 0, len(name)) + name.encode('ascii')
    assert data[offset:offset + len(signature)] == signature, 'Unexpected module body'
    header = struct.pack('<HIIH', version, 1, 1, 0) + bytes(3) + b'\1' + bytes(12)
    if version >= 171: header += bytes(4) # empty embedded-font table
    header += bytes(4) # empty image table
    if version >= 161:
        header += bytes(8 + 6 * 8) + b'\1' + bytes(4)
    root = struct.pack('<HfffB', 1, 8, 0, 0, 4) + b'Root' + struct.pack('<HHB', 10, 10, 1)
    footer = struct.pack('<HII', 0, 6, 4) + bytes(6) + struct.pack('<HB', 0, 0)
    return header + root + data[offset:] + footer

def scan_library(args):
    """Report import + save/reopen status; a rejection is an audit result, not a test pass."""
    results, skipped = [], []
    for source in sorted((args.flowstone_root / 'Modules').glob('*.hom')):
        data, name = source.read_bytes(), source.stem
        try:
            signature = struct.pack('<HfffB', 1, 8, 0, 0, len(name)) + name.encode('ascii')
        except (UnicodeEncodeError, struct.error):
            skipped.append(name)
            continue
        offset = data.find(signature)
        if offset < 0:
            skipped.append(name)
            continue
        version = struct.unpack_from('<H', data)[0]
        path = args.output / (name + '.fsm')
        with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('schematic.osm', wrap(data, name, version, offset))
        run = subprocess.run([str(args.probe.resolve()), str(path)], capture_output=True, text=True, timeout=30)
        detail = run.stderr.strip() if run.returncode else run.stdout.splitlines()[0].split(': version ', 1)[-1]
        results.append(dict(module=name, version=version, module_offset=hex(offset),
                            sha256=hashlib.sha256(data).hexdigest(),
                            status='imported' if run.returncode == 0 else 'rejected', detail=detail))
    report = dict(evidence='Unchanged .hom bodies in authored .fsm wrappers; import and save/reopen only.',
                  imported=sum(r['status'] == 'imported' for r in results), checked=len(results),
                  skipped_without_matching_body=skipped, modules=results)
    args.report.write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n')
    print(f"Imported and reopened {report['imported']} of {report['checked']} wrapped modules; report: {args.report}")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--flowstone-root', type=Path, required=True)
    p.add_argument('--probe', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--scan-library', action='store_true', help='Audit every library entry with a verified matching module body')
    p.add_argument('--report', type=Path, help='Write the scan results, including explicit rejection reasons, as JSON')
    args = p.parse_args()
    if args.scan_library and not args.report: p.error('--scan-library requires --report')
    args.output.mkdir(parents=True, exist_ok=True)
    if args.scan_library:
        scan_library(args)
        return
    paths = []
    for name, (version, offset, digest) in MODULES.items():
        data = (args.flowstone_root / 'Modules' / (name + '.hom')).read_bytes()
        if hashlib.sha256(data).hexdigest() != digest:
            raise ValueError('Bundled module hash changed: ' + name)
        path = args.output / (name + '.fsm')
        with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('schematic.osm', wrap(data, name, version, offset))
        paths.append(str(path))
    subprocess.run([str(args.probe.resolve()), '--bundled', *paths], check=True)
    paths = []
    for name, (module, offset, size, digest) in GUI_RECORDS.items():
        data = (args.flowstone_root / 'Modules' / (module + '.hom')).read_bytes()
        if hashlib.sha256(data).hexdigest() != digest: raise ValueError('Bundled GUI module hash changed: ' + module)
        path = args.output / (name + '.fsm')
        with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('schematic.osm', gui_record_document(data[offset:offset + size], name, struct.unpack_from('<H', data)[0]))
        paths.append(str(path))
    subprocess.run([str(args.probe.resolve()), '--gui-records', *paths], check=True)
    paths = []
    for name, (module, offset, size, digest) in DRAWING_RECORDS.items():
        data = (args.flowstone_root / 'Modules' / (module + '.hom')).read_bytes()
        if hashlib.sha256(data).hexdigest() != digest: raise ValueError('Bundled drawing module hash changed: ' + module)
        record = data[offset:offset + size]
        if struct.unpack_from('<H', record)[0] != (214 if name.endswith('Pen') else 215):
            raise ValueError('Unexpected native drawing record: ' + name)
        path = args.output / (name + '.fsm')
        with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('schematic.osm', gui_record_document(record, name, struct.unpack_from('<H', data)[0]))
        paths.append(str(path))
    subprocess.run([str(args.probe.resolve()), '--drawing-records', *paths], check=True)
    paths = []
    for name, (module, offset, size, digest) in FORMAT_RECORDS.items():
        data = (args.flowstone_root / 'Modules' / (module + '.hom')).read_bytes()
        if hashlib.sha256(data).hexdigest() != digest: raise ValueError('Bundled format module hash changed: ' + module)
        path = args.output / (name + '.fsm')
        with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('schematic.osm', gui_record_document(data[offset:offset+size],name,struct.unpack_from('<H',data)[0]))
        paths.append(str(path))
    subprocess.run([str(args.probe.resolve()), '--format-records', *paths], check=True)
    paths = []
    for name, (module, offset, tag, digest) in SHAPE_RECORDS.items():
        data = (args.flowstone_root / 'Modules' / (module + '.hom')).read_bytes()
        if hashlib.sha256(data).hexdigest() != digest: raise ValueError('Bundled shape module hash changed: ' + module)
        record = data[offset:offset+7]
        if struct.unpack_from('<H',record)[0] != tag or record[-1] != 0: raise ValueError('Unexpected native shape record: ' + name)
        path = args.output / (name + '.fsm')
        with zipfile.ZipFile(path,'w',compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('schematic.osm',gui_record_document(record,name,struct.unpack_from('<H',data)[0]))
        paths.append(str(path))
    subprocess.run([str(args.probe.resolve()),'--shape-records',*paths],check=True)
    paths = []
    for module, offset, end in [('Float Graph Recorder',0x151,0x281),('Graph Shaper',0xe77d,0xe871)]:
        data = (args.flowstone_root / 'Modules' / (module + '.hom')).read_bytes()
        digest = next(row[3] for row in SHAPE_RECORDS.values() if row[0] == module)
        if hashlib.sha256(data).hexdigest() != digest: raise ValueError('Bundled background module hash changed: ' + module)
        path = args.output / (module + ' background.fsm')
        with zipfile.ZipFile(path,'w',compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('schematic.osm',wrap(data[:end],'BG',struct.unpack_from('<H',data)[0],offset))
        paths.append(str(path))
    subprocess.run([str(args.probe.resolve()),'--background-modules',*paths],check=True)

    # Complete native graphs exercise Round Rectangle/View Size, and Redraw with
    # nested Selectors. Keep the original bodies and all their original links.
    for source, name, offset, end, mode in [
        ('Group Adornment', 'Label', 0x10d, 0x56e, '--group-label'),
        ('Multi Sample', 'Arc', 0x10eda, 0x11315, '--sweep-arc'),
    ]:
        data = (args.flowstone_root / 'Modules' / (source + '.hom')).read_bytes()
        digest = next(row[3] for row in SHAPE_RECORDS.values() if row[0] == source)
        if hashlib.sha256(data).hexdigest() != digest: raise ValueError('Bundled drawing-control module hash changed: ' + source)
        path = args.output / (source + ' ' + name + '.fsm')
        with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('schematic.osm', wrap(data[:end], name, struct.unpack_from('<H', data)[0], offset))
        subprocess.run([str(args.probe.resolve()), mode, str(path)], check=True)

    data = (args.flowstone_root / 'Modules' / 'Multi Sample.hom').read_bytes()
    if hashlib.sha256(data).hexdigest() != SHAPE_RECORDS['Multi Sample Arc'][3]: raise ValueError('Bundled Multi Sample module hash changed')
    record = data[0x219a:0x21a1]
    if struct.unpack('<HHHB', record) != (131, 86, 5, 0): raise ValueError('Unexpected native Redraw Area record')
    path = args.output / 'Multi Sample Redraw Area.fsm'
    with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr('schematic.osm', gui_record_document(record, 'Multi Sample Redraw Area', struct.unpack_from('<H', data)[0]))
    subprocess.run([str(args.probe.resolve()), '--redraw-area-record', str(path)], check=True)

    for source, name, offset, end, mode, digest in [
        ('Group Adornment', 'Group Adornment', 0x9f, None, '--group-adornment', SHAPE_RECORDS['Group Adornment Ellipse'][3]),
        ('Stream Power', 'Colour', 0xb12, 0xc10, '--color-module', DRAWING_RECORDS['Stream Power Font'][3]),
    ]:
        data = (args.flowstone_root / 'Modules' / (source + '.hom')).read_bytes()
        if hashlib.sha256(data).hexdigest() != digest: raise ValueError('Bundled color module hash changed: ' + source)
        path = args.output / (source + ' color GUI.fsm')
        with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('schematic.osm', wrap(data[:end], name, struct.unpack_from('<H', data)[0], offset))
        subprocess.run([str(args.probe.resolve()), mode, str(path)], check=True)

    data = (args.flowstone_root / 'Modules' / 'Float Graph Recorder.hom').read_bytes()
    if hashlib.sha256(data).hexdigest() != SHAPE_RECORDS['Float Graph Recorder Rect'][3]: raise ValueError('Bundled Float Graph Recorder module hash changed')
    record = data[0x3773:0x377a]
    if struct.unpack('<HHHB', record) != (609, 47, 6, 0): raise ValueError('Unexpected native View Cache record')
    path = args.output / 'Float Graph Recorder View Cache.fsm'
    with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr('schematic.osm', gui_record_document(record, 'View Cache', struct.unpack_from('<H', data)[0]))
    subprocess.run([str(args.probe.resolve()), '--view-cache-record', str(path)], check=True)

    paths = []
    for name,(source,offset,tag,digest) in COMPARISON_RECORDS.items():
        data = (args.flowstone_root / 'Modules' / (source + '.hom')).read_bytes()
        if hashlib.sha256(data).hexdigest() != digest: raise ValueError('Bundled comparison module hash changed: ' + source)
        record = data[offset:offset+43]
        if struct.unpack_from('<H',record)[0] != tag: raise ValueError('Unexpected native comparison record: ' + name)
        path = args.output / (name + '.fsm')
        with zipfile.ZipFile(path,'w',compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('schematic.osm',gui_record_document(record,name,struct.unpack_from('<H',data)[0]))
        paths.append(str(path))
    subprocess.run([str(args.probe.resolve()),'--comparison-records',*paths],check=True)
    data = (args.flowstone_root / 'Modules' / 'Moog Filter.hom').read_bytes()
    if hashlib.sha256(data).hexdigest() != COMPARISON_RECORDS['Moog Filter Equal'][3]: raise ValueError('Bundled Moog Filter module hash changed')
    path = args.output / 'Moog Filter Preset Name.fsm'
    with zipfile.ZipFile(path,'w',compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr('schematic.osm',wrap(data[:0x275],'Preset Name',159,0x162))
    subprocess.run([str(args.probe.resolve()),'--preset-name-module',str(path)],check=True)

    paths = []
    for name, (source, offset, size, tag, digest) in ROUTING_RECORDS.items():
        data = (args.flowstone_root / 'Modules' / (source + '.hom')).read_bytes()
        if hashlib.sha256(data).hexdigest() != digest: raise ValueError('Bundled routing module hash changed: ' + source)
        record = data[offset:offset+size]
        if len(record) != size or struct.unpack_from('<H', record)[0] != tag: raise ValueError('Unexpected native routing record: ' + name)
        path = args.output / (name + '.fsm')
        with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('schematic.osm', gui_record_document(record, name, struct.unpack_from('<H', data)[0]))
        paths.append(str(path))
    subprocess.run([str(args.probe.resolve()), '--routing-records', *paths], check=True)
    data = (args.flowstone_root / 'Modules' / 'Reverb.hom').read_bytes()
    if hashlib.sha256(data).hexdigest() != ROUTING_RECORDS['Reverb Float Select'][4]: raise ValueError('Bundled Reverb module hash changed')
    path = args.output / 'Reverb Left-Right split.fsm'
    with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr('schematic.osm', wrap(data[:0x435d], 'Left/Right split', 159, 0x4109))
    subprocess.run([str(args.probe.resolve()), '--stereo-split', str(path)], check=True)

    paths, state_records = [], {}
    for name, (source, offset, size, tag, digest) in STATE_RECORDS.items():
        data = (args.flowstone_root / 'Modules' / (source + '.hom')).read_bytes()
        if hashlib.sha256(data).hexdigest() != digest: raise ValueError('Bundled state module hash changed: ' + source)
        record = data[offset:offset+size]
        if len(record) != size or struct.unpack_from('<H', record)[0] != tag: raise ValueError('Unexpected native state record: ' + name)
        state_records[name] = record
        path = args.output / (name + '.fsm')
        with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('schematic.osm', gui_record_document(record, name, struct.unpack_from('<H', data)[0]))
        paths.append(str(path))
    subprocess.run([str(args.probe.resolve()), '--state-records', *paths], check=True)
    path = args.output / 'Bundled startup counter.fsm'
    with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
        records = state_records['Ticker Custom After Load'] + state_records['Hysteresis Gate Counter']
        archive.writestr('schematic.osm', gui_record_document(records, 'Startup counter', 159, node_count=2, links=[(0, 0, 1, 0)]))
    subprocess.run([str(args.probe.resolve()), '--startup-counter', str(path)], check=True)

    paths = []
    for name, (source, offset, tag, digest) in AUDIO_RECORDS.items():
        data = (args.flowstone_root / 'Modules' / (source + '.hom')).read_bytes()
        if hashlib.sha256(data).hexdigest() != digest: raise ValueError('Bundled audio module hash changed: ' + source)
        record = data[offset:offset+7]
        if len(record) != 7 or struct.unpack_from('<H', record)[0] != tag or record[-1] != 0: raise ValueError('Unexpected native audio record: ' + name)
        path = args.output / (name + '.fsm')
        with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('schematic.osm', gui_record_document(record, name, struct.unpack_from('<H', data)[0]))
        paths.append(str(path))
    subprocess.run([str(args.probe.resolve()), '--audio-records', *paths], check=True)

    paths = []
    for name, (source, offset, tag, digest) in GENERATOR_RECORDS.items():
        data = (args.flowstone_root / 'Modules' / (source + '.hom')).read_bytes()
        if hashlib.sha256(data).hexdigest() != digest: raise ValueError('Bundled generator module hash changed: ' + source)
        record = data[offset:offset + 7]
        if len(record) != 7 or struct.unpack_from('<H', record)[0] != tag or record[-1] != 0:
            raise ValueError('Unexpected native generator record: ' + name)
        path = args.output / (name + '.fsm')
        with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('schematic.osm', gui_record_document(record, name, struct.unpack_from('<H', data)[0]))
        paths.append(str(path))
    subprocess.run([str(args.probe.resolve()), '--generator-records', *paths], check=True)

    paths = []
    for name, (source, offset, size, digest) in BITMAP_RECORDS.items():
        data = (args.flowstone_root / 'Modules' / (source + '.hom')).read_bytes()
        if hashlib.sha256(data).hexdigest() != digest: raise ValueError('Bundled Bitmap module hash changed: ' + source)
        record = data[offset:offset + size]
        if len(record) != size or struct.unpack_from('<H', record)[0] != 105 or record[6:8] != bytes(2) or struct.unpack_from('<I', record, 8)[0] != size - 12:
            raise ValueError('Unexpected native Bitmap record: ' + name)
        path = args.output / (name + '.fsm')
        with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('schematic.osm', gui_record_document(record, name, struct.unpack_from('<H', data)[0]))
        paths.append(str(path))
    subprocess.run([str(args.probe.resolve()), '--bitmap-records', *paths], check=True)

    dialogs, pickers = [], []
    for name, offset in FILE_DIALOG_RECORDS.items():
        source, bitmap_offset, bitmap_size, digest = BITMAP_RECORDS[name]
        data = (args.flowstone_root / 'Modules' / (source + '.hom')).read_bytes()
        if hashlib.sha256(data).hexdigest() != digest: raise ValueError('Bundled File Dialog module hash changed: ' + source)
        record = data[offset:offset+7]
        if len(record) != 7 or struct.unpack_from('<H', record)[0] != 103 or record[6] != 0:
            raise ValueError('Unexpected native File Dialog record: ' + name)
        version = struct.unpack_from('<H', data)[0]
        path = args.output / (name + ' File Dialog.fsm')
        with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('schematic.osm', gui_record_document(record, name + ' dialog', version))
        dialogs.append(str(path))
        # Reproduce the containing module's Path->Path and OK->Load links in an
        # authored bench. Its Button record (17) remains outside this import pass.
        pair = record + data[bitmap_offset:bitmap_offset+bitmap_size]
        path = args.output / (name + ' picker.fsm')
        with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('schematic.osm', gui_record_document(pair, name + ' picker', version,
                              node_count=2, links=[(0,0,1,0),(0,1,1,1)]))
        pickers.append(str(path))
    subprocess.run([str(args.probe.resolve()), '--file-dialog-records', *dialogs], check=True)
    subprocess.run([str(args.probe.resolve()), '--bitmap-file-dialogs', *pickers], check=True)

    buttons, pickers = [], []
    for name, (source, offset, size, digest) in BUTTON_RECORDS.items():
        data = (args.flowstone_root / 'Modules' / (source + '.hom')).read_bytes()
        if hashlib.sha256(data).hexdigest() != digest: raise ValueError('Bundled Button module hash changed: ' + source)
        record = data[offset:offset+size]
        if len(record) != size or struct.unpack_from('<H', record)[0] != 17:
            raise ValueError('Unexpected native Button record: ' + name)
        version = struct.unpack_from('<H', data)[0]
        path = args.output / (name + '.fsm')
        with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('schematic.osm', gui_record_document(record, name, version))
        buttons.append(str(path))
        bitmap_name = name.replace('Button', 'Bitmap')
        if bitmap_name not in BITMAP_RECORDS: continue
        _, bitmap_offset, bitmap_size, _ = BITMAP_RECORDS[bitmap_name]
        dialog_offset = FILE_DIALOG_RECORDS[bitmap_name]
        records = record + data[dialog_offset:dialog_offset+7] + data[bitmap_offset:bitmap_offset+bitmap_size]
        # Unchanged native records, with the original Button->Open, Path->Path,
        # and OK->Load connections reproduced in an authored three-node bench.
        path = args.output / (name + ' picker.fsm')
        with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('schematic.osm', gui_record_document(records, name + ' picker', version,
                              node_count=3, links=[(0,0,1,0),(1,0,2,0),(1,1,2,1)]))
        pickers.append(str(path))
    subprocess.run([str(args.probe.resolve()), '--button-records', *buttons], check=True)
    subprocess.run([str(args.probe.resolve()), '--button-bitmap-pickers', *pickers], check=True)

    records, modules = [], []
    for source, (start, end, offset, digest) in MIDI_CC_MODULES.items():
        data = (args.flowstone_root / 'Modules' / (source + '.hom')).read_bytes()
        if hashlib.sha256(data).hexdigest() != digest: raise ValueError('Bundled MIDI CC module hash changed: ' + source)
        record = data[offset:offset + 7]
        if len(record) != 7 or struct.unpack_from('<H', record)[0] != 166 or record[6] != 0:
            raise ValueError('Unexpected native MIDI CC record: ' + source)
        record_path = args.output / (source + ' MIDI CC record.fsm')
        version = struct.unpack_from('<H', data)[0]
        with zipfile.ZipFile(record_path, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('schematic.osm', gui_record_document(record, source + ' MIDI CC', version))
        records.append(str(record_path))
        module_path = args.output / (source + ' MIDI CC module.fsm')
        with zipfile.ZipFile(module_path, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('schematic.osm', wrap(data[:end], 'MIDI CC', version, start))
        modules.append(str(module_path))
    subprocess.run([str(args.probe.resolve()), '--midi-cc-records', *records], check=True)
    subprocess.run([str(args.probe.resolve()), '--midi-cc-modules', *modules], check=True)

    paths = []
    for name, (source, offset, size, digest) in NATIVE_ROUTER_RECORDS.items():
        data = (args.flowstone_root / 'Modules' / (source + '.hom')).read_bytes()
        if hashlib.sha256(data).hexdigest() != digest: raise ValueError('Bundled router module hash changed: ' + source)
        record = data[offset:offset + size]
        tag = 149 if size == 15 else 1054
        if len(record) != size or struct.unpack_from('<H', record)[0] != tag or record[6] != 0:
            raise ValueError('Unexpected native router record: ' + name)
        path = args.output / (name + '.fsm')
        with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('schematic.osm', gui_record_document(record, name, struct.unpack_from('<H', data)[0]))
        paths.append(str(path))
    subprocess.run([str(args.probe.resolve()), '--router-records', *paths], check=True)

    paths = []
    for source, (offset, digest) in PRESET_RECORDS.items():
        data = (args.flowstone_root / 'Modules' / (source + '.hom')).read_bytes()
        if hashlib.sha256(data).hexdigest() != digest: raise ValueError('Bundled Preset module hash changed: ' + source)
        record = data[offset:offset + 149]
        if len(record) != 149 or struct.unpack_from('<H', record)[0] != 189 or record[6] != 0 or struct.unpack_from('<I', record, 7)[0] != 32:
            raise ValueError('Unexpected native Preset record: ' + source)
        path = args.output / (source + ' Preset.fsm')
        with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('schematic.osm', gui_record_document(record, source + ' Preset', struct.unpack_from('<H', data)[0]))
        paths.append(str(path))
    subprocess.run([str(args.probe.resolve()), '--preset-records', *paths], check=True)

    paths = []
    for name, (source, offset, selected, digest) in DROP_LIST_RECORDS.items():
        data = (args.flowstone_root / 'Modules' / (source + '.hom')).read_bytes()
        if hashlib.sha256(data).hexdigest() != digest: raise ValueError('Bundled Drop List module hash changed: ' + source)
        record = data[offset:offset + 11]
        if len(record) != 11 or struct.unpack_from('<H', record)[0] != 237 or record[6] != 0 or struct.unpack_from('<i', record, 7)[0] != selected:
            raise ValueError('Unexpected native Drop List record: ' + name)
        path = args.output / (name + '.fsm')
        with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('schematic.osm', gui_record_document(record, name, struct.unpack_from('<H', data)[0]))
        paths.append(str(path))
    subprocess.run([str(args.probe.resolve()), '--drop-list-records', *paths], check=True)

    paths = []
    for source, (offset, end, digest) in DROP_LIST_ITEMS.items():
        data = (args.flowstone_root / 'Modules' / (source + '.hom')).read_bytes()
        if hashlib.sha256(data).hexdigest() != digest: raise ValueError('Bundled Drop List items hash changed: ' + source)
        record = data[offset:end]
        path = args.output / (source + ' Drop List items.fsm')
        with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('schematic.osm', gui_record_document(record, source + ' Items', struct.unpack_from('<H', data)[0],
                node_count=3, links=[(0, 0, 2, 3), (2, 1, 1, 0)]))
        paths.append(str(path))
    subprocess.run([str(args.probe.resolve()), '--drop-list-items', *paths], check=True)

    paths = []
    for source, (offset, end, digest) in SQUARE_MODULES.items():
        data = (args.flowstone_root / 'Modules' / (source + '.hom')).read_bytes()
        if hashlib.sha256(data).hexdigest() != digest: raise ValueError('Bundled Square module hash changed: ' + source)
        path = args.output / (source + ' Square.fsm')
        with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('schematic.osm', wrap(data[:end], 'Square', struct.unpack_from('<H', data)[0], offset))
        paths.append(str(path))
    subprocess.run([str(args.probe.resolve()), '--square-modules', *paths], check=True)
    data = (args.flowstone_root / 'Modules' / 'Echo Delay.hom').read_bytes()
    if hashlib.sha256(data).hexdigest() != AUDIO_RECORDS['Echo Delay native Delay'][3]: raise ValueError('Bundled Echo Delay module hash changed')
    path = args.output / 'Echo Delay Echo module.fsm'
    with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr('schematic.osm', wrap(data[:0x66e], 'Echo', 159, 0x469))
    subprocess.run([str(args.probe.resolve()), '--echo-module', str(path)], check=True)


    # Complete, unchanged GUI submodule: Line + Area/property routes + wired Pen.
    data = (args.flowstone_root / 'Modules' / 'LED-Indicator.hom').read_bytes()
    if hashlib.sha256(data).hexdigest() != GUI_RECORDS['LED GUI'][3]: raise ValueError('Bundled LED module hash changed')
    path = args.output / 'LED separator.fsm'
    with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr('schematic.osm', wrap(data[:0x7f0], 'Sep', 166, 0x66b))
    subprocess.run([str(args.probe.resolve()), '--gui-modules', str(path)], check=True)
    data = (args.flowstone_root / 'Modules' / 'Stream Power.hom').read_bytes()
    if hashlib.sha256(data).hexdigest() != DRAWING_RECORDS['Stream Power Font'][3]: raise ValueError('Bundled Stream Power module hash changed')
    path = args.output / 'Stream Power label.fsm'
    with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr('schematic.osm', wrap(data[:0xa88],'Label',159,0x71a))
    subprocess.run([str(args.probe.resolve()), '--label-module', str(path)],check=True)



if __name__ == '__main__':
    main()
