#!/usr/bin/env python3
"""Build isolated diagnostics; never run hardware or overwrite the original binary."""
from pathlib import Path
import hashlib
import json
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'results/macos/idle_ack_tools'
OUT.mkdir(parents=True, exist_ok=True)
SRC = ROOT / 'msu2_cpp_v0_2/src'
INCLUDE = ROOT / 'msu2_cpp_v0_2/include'

def replace_once(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError('Source marker is not unique: ' + old[:80])
    return text.replace(old, new, 1)

# A diagnostic copy adds only frame/patch attribution; planner logic stays intact.
main = (SRC / 'main.cpp').read_text()
main = 'void diagnostic_patch(unsigned, unsigned, unsigned);\n' + main
main = replace_once(main, '            try {\n                // Complete all rectangles',
                    '            unsigned diagnostic_index=0;\n            try {\n                // Complete all rectangles')
main = replace_once(main, '                    auto part=session.send(patch.frame);',
                    '                    diagnostic_patch(frame.id,diagnostic_index,unsigned(plan.patches.size()));\n'
                    '                    auto part=session.send(patch.frame);\n                    ++diagnostic_index;')
main = replace_once(main, '            } catch(...) {planner.invalidate();throw;}',
                    '            } catch(const std::exception& e) {\n'
                    '                std::cerr << "FAIL_CONTEXT frame_id=" << frame.id << " patch_index=" << diagnostic_index\n'
                    '                          << " patch_count=" << plan.patches.size() << " kind=" << msu2::update_kind_name(plan.kind)\n'
                    '                          << " error=" << e.what() << "\\n";\n'
                    '                planner.invalidate();throw;\n            }')
(OUT / 'main_patch_trace.cpp').write_text(main)
transport = (ROOT / 'tests/macos/transport_timed.cpp').read_text()
transport = replace_once(transport, 'namespace msu2 {',
    'namespace { unsigned context_frame=0, context_patch=0, context_count=0; }\n'
    'void diagnostic_patch(unsigned frame,unsigned patch,unsigned count) {\n'
    '    context_frame=frame; context_patch=patch; context_count=count;\n}\nnamespace msu2 {')
transport = replace_once(transport, 'int error; std::uint8_t data[6];',
    'int error; std::uint8_t data[6]; unsigned frame; unsigned patch; unsigned patch_count;')
transport = replace_once(transport, 'error, {}};',
    'error, {}, context_frame, context_patch, context_count};')
transport = replace_once(transport, 'errno=%d data=%02x%02x%02x%02x%02x%02x\\n',
    'errno=%d data=%02x%02x%02x%02x%02x%02x frame=%u patch=%u patch_count=%u\\n')
transport = replace_once(transport, 'e.data[4],e.data[5]);',
    'e.data[4],e.data[5],e.frame,e.patch,e.patch_count);')
(OUT / 'transport_patch_trace.cpp').write_text(transport)
common = ['c++', '-std=c++17', '-O3', '-DNDEBUG', '-Wall', '-Wextra', '-Wpedantic', '-pthread', '-I', str(INCLUDE)]
probe = common + [str(SRC / f) for f in ('protocol.cpp','image.cpp','session.cpp','transport.cpp')]
probe += [str(ROOT / 'tests/macos/idle_full_probe.cpp'), '-o', str(OUT / 'idle_full_probe')]
trace = common + [str(SRC / f) for f in ('protocol.cpp','delta.cpp','image.cpp','session.cpp','capture.cpp')]
trace += [str(OUT / 'transport_patch_trace.cpp'), str(OUT / 'main_patch_trace.cpp'), '-o', str(OUT / 'msu2_patch_trace')]
for command in (probe, trace):
    subprocess.run(command, check=True)
subprocess.run([str(OUT / 'idle_full_probe'), '--self-test'], check=True)
paths = [OUT / 'idle_full_probe', OUT / 'msu2_patch_trace', OUT / 'main_patch_trace.cpp', OUT / 'transport_patch_trace.cpp']
metadata = {'commands': [probe,trace], 'sha256': {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}}
(OUT / 'build.json').write_text(json.dumps(metadata, indent=2) + '\n')
print(json.dumps(metadata['sha256'], indent=2))
