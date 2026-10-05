#!/usr/bin/env python3
"""Offline fixtures for the Host VF ftrace analyzer."""

from pathlib import Path
import importlib.util
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parent
ANALYZER_PATH = ROOT / "host_vf_trace_analyzer.py"
SPEC = importlib.util.spec_from_file_location("host_vf_trace_analyzer", ANALYZER_PATH)
assert SPEC and SPEC.loader
ANALYZER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = ANALYZER
SPEC.loader.exec_module(ANALYZER)


NORMAL = """\
 1.000000 | 0) marker-1   | /* NG_VF_TRACE_BEGIN */
 1.000010 | 1) kworker-42 | pf_state_worker_func [i915]() {
 1.000020 | 3) kworker-42 | i915_ggtt_set_space_owner [i915]() {
 1.000030 | 2) render-77  | /* intel_pipe_update_vblank_evaded: dev 0000:00:02.0, pipe A, frame=10, scanline=800, min=1064, max=1080 */
 1.000040 | 4) kworker-42 | 20.000 us | }
 1.000050 | 2) render-77  | /* intel_pipe_update_end: dev 0000:00:02.0, pipe A, frame=10, scanline=803 */
 1.000060 | 5) kworker-42 | 50.000 us | }
 1.000070 | 0) marker-1   | /* NG_VF_TRACE_END */
"""

MISMATCH = NORMAL.replace("frame=10, scanline=803", "frame=11, scanline=2")
MALFORMED = NORMAL.replace("/* NG_VF_TRACE_END */", "/* marker missing */").replace(
    "/* intel_pipe_update_end: dev 0000:00:02.0, pipe A, frame=10, scanline=803 */\n",
    "",
)
LOST = NORMAL + "CPU:1 [LOST 7 EVENTS]\n"
NO_VF = NORMAL.replace(
    " 1.000010 | 1) kworker-42 | pf_state_worker_func [i915]() {\n"
    " 1.000020 | 3) kworker-42 | i915_ggtt_set_space_owner [i915]() {\n",
    "",
).replace(
    " 1.000040 | 4) kworker-42 | 20.000 us | }\n",
    "",
).replace(
    " 1.000060 | 5) kworker-42 | 50.000 us | }\n",
    "",
)


def run_fixture(text: str, *arguments: str) -> subprocess.CompletedProcess[str]:
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", delete=False) as stream:
        stream.write(text)
        path = Path(stream.name)
    try:
        return subprocess.run(
            [sys.executable, "-B", str(ANALYZER_PATH), *arguments, str(path)],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    finally:
        path.unlink()


def main() -> None:
    normal = ANALYZER.analyze_text(NORMAL)
    assert not normal.issues
    assert not normal.frame_mismatches
    assert len(normal.updates) == 1
    assert normal.updates[0].usecs == ANALYZER.Decimal("20.000000")
    assert [interval.name for interval in normal.function_intervals] == [
        "i915_ggtt_set_space_owner", "pf_state_worker_func",
    ]
    rendered = ANALYZER.render(normal)
    assert "overlapping_pipe_updates=1" in rendered
    assert rendered.endswith("PASS: Host ftrace is complete and no pipe update crossed vblank")

    required = run_fixture(NORMAL, "--require-vf-flr")
    assert required.returncode == 0

    no_vf = run_fixture(NO_VF)
    assert no_vf.returncode == 0
    missing_vf = run_fixture(NO_VF, "--require-vf-flr")
    assert missing_vf.returncode == 1
    assert "required VF FLR trace function was not captured" in missing_vf.stdout

    mismatch = run_fixture(MISMATCH)
    assert mismatch.returncode == 2
    assert "FRAME_MISMATCH pipe=A frame=10->11" in mismatch.stdout
    assert "crossed vblank" in mismatch.stdout

    malformed = run_fixture(MALFORMED)
    assert malformed.returncode == 1
    assert "STRUCTURE_ERROR expected one END marker" in malformed.stdout
    assert "STRUCTURE_ERROR pipe A has an unterminated update" in malformed.stdout

    lost = run_fixture(LOST)
    assert lost.returncode == 1
    assert "STRUCTURE_ERROR ftrace reported lost events" in lost.stdout

    refusal = subprocess.run(
        [sys.executable, "-B", str(ANALYZER_PATH), "relative.log"],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    assert refusal.returncode == 64

    print("PASS: Host VF ftrace parsing, overlap and fail-closed fixtures")


if __name__ == "__main__":
    main()
