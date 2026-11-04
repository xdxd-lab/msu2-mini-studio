import importlib.util
from pathlib import Path
import sys
import unittest

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'scripts/macos'))
import run_static_transition_suite as suite
from build_static_transition_diagnostics import generate


def event(**changes):
    data=dict(t_ms='1.0',phase='window_ack_wait',kind='R',frame='315',patch='0',patch_count='2',
              size='6',captured='6',value1='0',value2='0',data='004d534e3031')
    data.update(changes)
    return 'OBS '+' '.join(key+'='+value for key,value in data.items())+'\nTRACE events=1 overflow=0\n'


class TraceTests(unittest.TestCase):
    def test_full_rx(self):
        events=suite.parse_trace(event())
        self.assertEqual(bytes.fromhex(events[0]['data']),b'\x00MSN01')

    def test_nonfinite_clock(self):
        for value in ('nan','inf','-1'):
            with self.subTest(value=value), self.assertRaises(ValueError):
                suite.parse_trace(event(t_ms=value))

    def test_truncated_or_malformed_rx(self):
        for text in (event(captured='5',data='004d534e30'),event(data='zzzzzzzzzzzz'),
                     event(captured='7'),event(size='7'),event(size='nan')):
            with self.subTest(text=text),self.assertRaises(ValueError): suite.parse_trace(text)

    def test_overflow_missing_trailer(self):
        for text in (event().replace('overflow=0','overflow=1'),event().split('TRACE')[0],
                     event().replace('events=1','events=2')):
            with self.subTest(text=text),self.assertRaises(ValueError): suite.parse_trace(text)

    def test_decreasing_clock_and_duplicate_field(self):
        text=event().split('TRACE')[0]+event(t_ms='0.9').replace('events=1','events=2')
        with self.assertRaises(ValueError): suite.parse_trace(text)
        with self.assertRaises(ValueError): suite.parse_trace(event().replace('kind=R','kind=R kind=T'))

    def test_matrix(self):
        cases=suite.cases()
        self.assertEqual(len(cases),6)
        self.assertEqual([c['expected_frames'] for c in cases],[360,495,660,495,660,360])
        self.assertEqual([c['resume_frame'] for c in cases],[180,315,420,315,420,180])
        self.assertEqual(sum(c['recovery'] for c in cases),2)
        for c in cases:
            self.assertEqual((c['chunk'],c['pattern'],c['update'],c['keyframe_ms']),(60,'moving','diff',0))

    def test_generated_copy_preserves_producer_and_commit(self):
        original=(ROOT/'msu2_cpp_v0_2/src/main.cpp').read_text()
        copy=generate(original)
        producer=original[original.index('    std::thread producer'):original.index('    unsigned sent=')]
        self.assertIn(producer,copy)
        commit=original[original.index('            auto done=Clock::now();++processed;'):original.index('double percentile')]
        self.assertIn(commit,copy)
        self.assertEqual(copy.count('msu2diag::send_plan('),1)

    def test_source_change_fails_closed(self):
        original=(ROOT/'msu2_cpp_v0_2/src/main.cpp').read_text().replace('auto fps=a.num("--fps",30,1,240);','')
        with self.assertRaises(ValueError): generate(original)


if __name__=='__main__': unittest.main()
