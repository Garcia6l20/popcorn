from popcorn.profiling import FrameKind, FrameRow, Report

START = 1


def _name(frame: FrameRow) -> str:
    match frame.kind:
        case FrameKind.ROOT:
            return "(root)"
        case FrameKind.EXCEPTION:
            return f"[{frame.name}]"
        case _:
            return frame.name


def _check(frames: tuple[FrameRow, ...]) -> None:
    if not frames:
        raise ValueError("the call tree has no frames")
    if frames[0].kind is not FrameKind.ROOT:
        raise ValueError("the first frame is not the root")
    for index in range(1, len(frames)):
        if not 0 <= frames[index].parent < index:
            raise ValueError(
                f"frame {index} has parent {frames[index].parent}, expected 0 to {index - 1}"
            )


def to_cpuprofile(report: Report) -> dict[str, object]:
    """The call tree of `report` as a V8 `CPUProfile`, the format Chrome DevTools and speedscope
    load.

    A node is a frame with `id` its index plus one. `hitCount` is the frame's self instructions.
    There is one sample per node with self instructions, in depth first order so that a node's
    samples are contiguous, and one time unit is one instruction. `startTime` is 1, Chrome
    DevTools draws nothing at 0. A last sample on the root, with no weight, starts at `endTime` and
    closes the final real sample, which DevTools would otherwise give the mean interval. The
    time axis lays the weights end to end and is no recording of when they ran.

    Raises:
        ValueError: The report has no hotspots, or its frames are no tree: empty, a first frame
            that is not the root, or a parent that does not precede its child.
    """
    if report.hotspots is None:
        raise ValueError("the cpuprofile export needs the hotspots analysis")
    frames = report.hotspots.frames
    _check(frames)
    children: list[list[int]] = [[] for _ in frames]
    for index in range(1, len(frames)):
        children[frames[index].parent].append(index)
    nodes: list[dict[str, object]] = [
        {
            "id": index + 1,
            "callFrame": {
                "functionName": _name(frame),
                "scriptId": "0",
                "url": "" if frame.kind is FrameKind.ROOT else str(frame.kind),
                "lineNumber": -1,
                "columnNumber": -1,
            },
            "hitCount": frame.self,
            "children": [child + 1 for child in children[index]],
        }
        for index, frame in enumerate(frames)
    ]
    samples: list[int] = []
    weights: list[int] = []
    stack = [0]
    while stack:
        index = stack.pop()
        if frames[index].self > 0:
            samples.append(index + 1)
            weights.append(frames[index].self)
        stack.extend(reversed(children[index]))
    if weights:
        samples.append(1)
        weights.append(0)
    return {
        "nodes": nodes,
        "startTime": START,
        "endTime": START + sum(weights),
        "samples": samples,
        "timeDeltas": [0, *weights[:-1]] if weights else [],
    }
