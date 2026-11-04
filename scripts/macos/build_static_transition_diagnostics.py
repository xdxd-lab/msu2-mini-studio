#!/usr/bin/env python3
"""Generate an isolated copy of the original stream; build/fake test without USB."""
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'results/macos/static_transition_tools'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError('Original source marker is not unique: ' + old[:90])
    return text.replace(old, new, 1)


def generate(original):
    text = replace_once(original, '#include "msu2/capture.hpp"',
                        '#include "static_transition_trace.hpp"\n#include "msu2/capture.hpp"')
    old = '{"stream",{"--input","--pattern","--desktop","--mode","--port","--timeout-ms","--chunk","--ack","--orientation","--frames","--fps","--csv","--update","--keyframe-ms"}},'
    text = replace_once(text, old, old[:-3] + ',"--diag-static-ms","--diag-recovery"}},')
    text = replace_once(text, 'msu2::make_pattern(pattern,tick);',
                        'msu2::make_pattern(pattern,msu2diag::source_tick(tick));')
    text = replace_once(text, 'void connection(msu2::Session& s,const Args& a)',
                        'int connection(msu2::Session& s,const Args& a)')
    text = replace_once(text, 's.set_orientation(a.num("--orientation",0,0,1));\n}',
                        's.set_orientation(a.num("--orientation",0,0,1));\n    return version;\n}')
    text = replace_once(text, 'auto fps=a.num("--fps",30,1,240);', '''auto fps=a.num("--fps",30,1,240);
    msu2diag::configure(fps,a.num("--diag-static-ms",6000,3000,6000));
    const bool diag_recovery=a.num("--diag-recovery",0,0,1)!=0;
    if(a.get("--pattern")!="moving" || a.has("--input") || a.has("--desktop")
       || a.get("--mode")!="pair" || a.get("--update")!="diff"
       || a.num("--keyframe-ms",1000,0,60000)!=0 || options.write_chunk!=60
       || options.ack_mode!=msu2::AckMode::prefix || options.io_timeout!=msu2::Milliseconds(2000)
       || options.handshake_timeout!=msu2::Milliseconds(2000) || a.has("--orientation")
       || count!=fps*5+msu2diag::static_frames)
        throw std::invalid_argument("fixed static-transition diagnostic configuration required");''')
    text = replace_once(text, '''    check_csv_path(a);
    msu2::SerialTransport transport(port);
    msu2::Session session(transport,options);
    connection(session,a);
    std::ofstream csv;''', '''    check_csv_path(a);
    msu2::SerialTransport transport(port);
    msu2diag::ObservedTransport observed(transport);
    msu2::Session session(observed,options);
    observed.handshake_phase();
    try {observed.handshake_done(connection(session,a));}
    catch(const std::exception& e){observed.failure(e.what());throw;}
    std::ofstream csv;''')
    start = '            msu2::TransferStats st{};\n            try {'
    end = '            } catch(...) {planner.invalidate();throw;}'
    if text.count(start) != 1 or text.count(end) != 1:
        raise ValueError('Original send-loop markers changed')
    begin = text.index(start)
    finish = text.index(end, begin) + len(end)
    text = text[:begin] + '''            observed.plan_event(frame.id,plan);
            auto st=msu2diag::send_plan(session,observed,planner,plan,rgb565,mode,
                                      options,frame.id,after,diag_recovery);''' + text[finish:]
    return text


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    original = ROOT / 'msu2_cpp_v0_2/src/main.cpp'
    generated = OUT / 'main_static_transition.cpp'
    generated.write_text(generate(original.read_text()))
    core = [ROOT / 'msu2_cpp_v0_2/src' / (name + '.cpp')
            for name in ('protocol', 'delta', 'image', 'session', 'transport', 'capture')]
    compiler = subprocess.run(['c++', '--version'], capture_output=True, text=True, check=True)
    base = ['c++', '-std=c++17', '-O3', '-DNDEBUG', '-Wall', '-Wextra', '-Wpedantic', '-pthread',
            '-I', str(ROOT / 'msu2_cpp_v0_2/include'), '-I', str(ROOT / 'tests/macos')]
    binary = OUT / 'msu2_static_transition'
    command = base + [str(p) for p in core + [generated]] + ['-o', str(binary)]
    subprocess.run(command, check=True)
    fake_binary = OUT / 'static_transition_selftest'
    fake_source = ROOT / 'tests/macos/test_static_transition.cpp'
    fake_command = base + [str(p) for p in core + [fake_source]] + ['-o', str(fake_binary)]
    subprocess.run(fake_command, check=True)
    check = subprocess.run([str(fake_binary)], capture_output=True, text=True)
    (OUT / 'selftest.log').write_text(check.stdout + check.stderr)
    if check.returncode:
        raise RuntimeError('Fake selftest failed: ' + check.stdout + check.stderr)
    fixture = subprocess.run([str(fake_binary), '--trace-fixture'], capture_output=True, text=True, check=True)
    fixture_path = OUT / 'fake_trace.log'
    fixture_path.write_text(fixture.stdout + fixture.stderr)
    from run_static_transition_suite import parse_trace
    events = parse_trace(fixture_path.read_text())
    if not any(e['phase']=='recovery_full_ok' for e in events):
        raise RuntimeError('Real C++ fake trace lacks full recovery evidence')
    files = list((ROOT / 'msu2_cpp_v0_2/include').rglob('*.hpp')) + core + [original, generated,
            fake_source, ROOT / 'tests/macos/static_transition_trace.hpp', Path(__file__).resolve(),
            ROOT / 'scripts/macos/run_static_transition_suite.py',
            ROOT / 'scripts/macos/serial_guard.py', ROOT / 'scripts/macos/run_hardware.py',
            ROOT / 'scripts/macos/run_fps_sweep.py', ROOT / 'scripts/macos/analyze_results.py',
            binary, fake_binary, OUT / 'selftest.log', fixture_path]
    metadata = dict(schema_version=1, command=command, fake_command=fake_command,
                    compiler_version=compiler.stdout, fake_exit_code=check.returncode,
                    changes='isolated source-tick mapping + memory trace + optional one strict recovery; original tree untouched',
                    sha256={str(p.relative_to(ROOT)): sha(p) for p in files})
    (OUT / 'build.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print(check.stdout.strip())
    print('BINARY_SHA256=' + sha(binary))


if __name__ == '__main__':
    main()
