#!/usr/bin/env python3
"""Analyze the bounded Host ftrace captured around one contained VF run."""

from __future__ import annotations

from dataclasses import dataclass
from decimal import Decimal
from pathlib import Path
import re
import sys


LINE = re.compile(
    r"^\s*(?P<time>[0-9]+\.[0-9]+)\s+\|\s*(?P<cpu>[0-9]+)\)"
    r"\s+(?:(?P<task>\S+)\s+)?\|"
)
TASK_PID = re.compile(r"-(?P<pid>[0-9]+)$")
EVENT = re.compile(
    r"/\* intel_pipe_update_(?P<kind>vblank_evaded|end): .*"
    r"pipe (?P<pipe>[A-Z]), frame=(?P<frame>[0-9]+), "
    r"scanline=(?P<scanline>[0-9]+)"
)
FUNCTION = re.compile(
    r"\|\s+(?P<name>pf_state_worker_func|i915_ggtt_set_space_owner|"
    r"intel_pipe_update_start|intel_pipe_update_end) \[i915\]\(\) \{$"
)
FUNCTION_CLOSE = re.compile(
    r"\|\s+\}\s*(?:/\*\s*(?P<name>pf_state_worker_func|"
    r"i915_ggtt_set_space_owner|intel_pipe_update_start|"
    r"intel_pipe_update_end) \[i915\]\s*\*/)?$"
)
TARGETS = ("pf_state_worker_func", "i915_ggtt_set_space_owner")


@dataclass(frozen=True)
class Interval:
    name: str
    start: Decimal
    end: Decimal


@dataclass(frozen=True)
class PipeUpdate:
    pipe: str
    start: Decimal
    end: Decimal
    start_frame: int
    end_frame: int
    start_scanline: int
    end_scanline: int

    @property
    def usecs(self) -> Decimal:
        return (self.end - self.start) * Decimal(1_000_000)


@dataclass(frozen=True)
class Analysis:
    begin_markers: int
    end_markers: int
    updates: tuple[PipeUpdate, ...]
    function_intervals: tuple[Interval, ...]
    issues: tuple[str, ...]

    @property
    def frame_mismatches(self) -> tuple[PipeUpdate, ...]:
        return tuple(
            update for update in self.updates
            if update.start_frame != update.end_frame
        )


def analyze_text(text: str, require_vf_flr: bool = False) -> Analysis:
    begin_markers = text.count("NG_VF_TRACE_BEGIN")
    end_markers = text.count("NG_VF_TRACE_END")
    issues: list[str] = []
    pending: dict[str, tuple[Decimal, int, int]] = {}
    updates: list[PipeUpdate] = []
    stacks: dict[str, list[tuple[str, Decimal]]] = {}
    function_intervals: list[Interval] = []

    for line in text.splitlines():
        prefix = LINE.match(line)
        if not prefix:
            continue
        timestamp = Decimal(prefix.group("time"))
        cpu = int(prefix.group("cpu"))
        task = prefix.group("task")
        if task:
            task_pid = TASK_PID.search(task)
            context = (
                f"pid:{task_pid.group('pid')}"
                if task_pid else f"task:{task}"
            )
        else:
            context = f"cpu:{cpu}"

        event = EVENT.search(line)
        if event:
            kind = event.group("kind")
            pipe = event.group("pipe")
            frame = int(event.group("frame"))
            scanline = int(event.group("scanline"))
            if kind == "vblank_evaded":
                if pipe in pending:
                    issues.append(f"pipe {pipe} has two starts without an end")
                pending[pipe] = (timestamp, frame, scanline)
            elif pipe not in pending:
                issues.append(f"pipe {pipe} has an end without a start")
            else:
                start, start_frame, start_scanline = pending.pop(pipe)
                if timestamp < start:
                    issues.append(f"pipe {pipe} end timestamp precedes start")
                updates.append(PipeUpdate(
                    pipe, start, timestamp, start_frame, frame,
                    start_scanline, scanline,
                ))

        function = FUNCTION.search(line)
        if function:
            stacks.setdefault(context, []).append((function.group("name"), timestamp))
        else:
            close = FUNCTION_CLOSE.search(line)
            if not close:
                continue
            stack = stacks.get(context)
            close_name = close.group("name")
            if not stack:
                issues.append(
                    f"function graph context {context} closes "
                    f"{close_name or 'a function'} without a start"
                )
                continue
            name, start = stack[-1]
            if close_name is not None and close_name != name:
                issues.append(
                    f"function graph context {context} closes {close_name} "
                    f"while {name} is active"
                )
                continue
            stack.pop()
            if timestamp < start:
                issues.append(
                    f"function graph context {context} closes {name} "
                    "before its start"
                )
            elif name in TARGETS:
                function_intervals.append(Interval(name, start, timestamp))

    if begin_markers != 1:
        issues.append(f"expected one BEGIN marker, observed {begin_markers}")
    if end_markers != 1:
        issues.append(f"expected one END marker, observed {end_markers}")
    if re.search(r"\[LOST [0-9]+ EVENTS\]", text):
        issues.append("ftrace reported lost events")
    for pipe in sorted(pending):
        issues.append(f"pipe {pipe} has an unterminated update")
    for context in sorted(stacks):
        stack = stacks[context]
        if stack:
            active = ",".join(name for name, _ in stack)
            issues.append(
                f"function graph context {context} has unterminated stack: "
                f"{active}"
            )
    if not updates:
        issues.append("no complete pipe update was captured")
    if require_vf_flr:
        observed_targets = {interval.name for interval in function_intervals}
        for target in TARGETS:
            if target not in observed_targets:
                issues.append(f"required VF FLR trace function was not captured: {target}")

    return Analysis(
        begin_markers,
        end_markers,
        tuple(updates),
        tuple(function_intervals),
        tuple(issues),
    )


def percentile(values: list[Decimal], numerator: int, denominator: int) -> Decimal:
    ordered = sorted(values)
    index = max(0, (len(ordered) * numerator + denominator - 1) // denominator - 1)
    return ordered[index]


def overlaps(left_start: Decimal, left_end: Decimal,
             right_start: Decimal, right_end: Decimal) -> bool:
    return left_start <= right_end and right_start <= left_end


def render(analysis: Analysis) -> str:
    lines = [
        f"markers begin={analysis.begin_markers} end={analysis.end_markers}",
        f"pipe_updates={len(analysis.updates)} "
        f"frame_mismatches={len(analysis.frame_mismatches)}",
    ]
    if analysis.updates:
        durations = [update.usecs for update in analysis.updates]
        lines.append(
            "critical_us "
            f"min={min(durations):.3f} "
            f"p50={percentile(durations, 1, 2):.3f} "
            f"p95={percentile(durations, 95, 100):.3f} "
            f"max={max(durations):.3f}"
        )

    for target in TARGETS:
        intervals = [
            interval for interval in analysis.function_intervals
            if interval.name == target
        ]
        overlap_count = sum(
            1 for update in analysis.updates
            if any(overlaps(update.start, update.end, interval.start, interval.end)
                   for interval in intervals)
        )
        total_usecs = sum(
            ((interval.end - interval.start) * Decimal(1_000_000)
             for interval in intervals),
            Decimal(0),
        )
        interval_usecs = [
            (interval.end - interval.start) * Decimal(1_000_000)
            for interval in intervals
        ]
        duration_summary = ""
        if interval_usecs:
            duration_summary = (
                f" interval_us_min={min(interval_usecs):.3f}"
                f" interval_us_p50={percentile(interval_usecs, 1, 2):.3f}"
                f" interval_us_max={max(interval_usecs):.3f}"
            )
        lines.append(
            f"{target} intervals={len(intervals)} "
            f"total_us={total_usecs:.3f} overlapping_pipe_updates={overlap_count}"
            f"{duration_summary}"
        )

    for update in analysis.frame_mismatches:
        overlap_names = [
            target for target in TARGETS
            if any(
                interval.name == target and
                overlaps(update.start, update.end, interval.start, interval.end)
                for interval in analysis.function_intervals
            )
        ]
        lines.append(
            f"FRAME_MISMATCH pipe={update.pipe} "
            f"frame={update.start_frame}->{update.end_frame} "
            f"scanline={update.start_scanline}->{update.end_scanline} "
            f"critical_us={update.usecs:.3f} "
            f"overlaps={','.join(overlap_names) if overlap_names else 'none'}"
        )
    lines.extend(f"STRUCTURE_ERROR {issue}" for issue in analysis.issues)
    if analysis.issues:
        lines.append("FAIL: Host ftrace structure is incomplete")
    elif analysis.frame_mismatches:
        lines.append("FAIL: a display atomic critical section crossed vblank")
    else:
        lines.append("PASS: Host ftrace is complete and no pipe update crossed vblank")
    return "\n".join(lines)


def main(argv: list[str]) -> int:
    require_vf_flr = False
    if len(argv) == 3 and argv[1] == "--require-vf-flr":
        require_vf_flr = True
        path = Path(argv[2])
    elif len(argv) == 2:
        path = Path(argv[1])
    else:
        print(
            f"usage: {Path(argv[0]).name} [--require-vf-flr] "
            "/absolute/host-ftrace.log",
            file=sys.stderr,
        )
        return 64
    if not path.is_absolute() or not path.is_file() or path.is_symlink():
        print("FAIL: trace must be an absolute regular non-symlink file", file=sys.stderr)
        return 64
    analysis = analyze_text(
        path.read_text(encoding="utf-8", errors="strict"),
        require_vf_flr=require_vf_flr,
    )
    print(render(analysis))
    if analysis.issues:
        return 1
    if analysis.frame_mismatches:
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
