"""The frame record `UartTap` and `UartDevice` taps share."""

from __future__ import annotations

from dataclasses import dataclass

from popcorn.units import VirtualTime


@dataclass(frozen=True, slots=True)
class TapFrame:
    """One frame on one direction of a link, as `UartTap` shows it, or of a device's port, as
    `UartDevice.tap_frames` shows it. Times are system times on a link, `Machine.time` on a
    device's port.

    Attributes:
        data: Data bits, 0 for a break, the parity bit stripped as in `Frame.data`.
        index: Number of the frame among its sender's frames in this direction, from 0 at
            system start, or among the frames injected in this direction for an injected one:
            the index `UartTap.replace` and `drop` take for a sender's frame.
        start: Start-bit edge at the sender, at the receiver for an injected frame.
        end: Stop-bit end at the sender, the abort time for a frame the sender aborted, at the
            receiver for an injected frame. A frame a cut made lost keeps its stop-bit end.
        arrival: When the receiver sees the frame end: `end` plus the link's latency, `end` for
            an injected frame.
        towards: The receiving end, `"<member>.<UART>"`. On a device's port `"device"` for a
            frame the firmware sent and the UART's name, such as `"USART1"`, for one the device
            sent.
        injected: Whether `UartTap.inject` put it on the line.
        brk: Whether it is a break.
        lost: Whether the sender aborted it, its UART disabled mid-frame, or a cut made it
            cross the cut's start or end.
        dropped: Whether an armed `UartTap.drop` kept it off the line: the receiver saw an idle
            line.
        cut: Whether a line cut touched it, `UartTap.cut`: it starts inside the cut, or crosses
            the cut's start, also `lost` then. A dropped frame is never `cut`.
    """

    data: int
    index: int
    start: VirtualTime
    end: VirtualTime
    arrival: VirtualTime
    towards: str
    injected: bool
    brk: bool
    lost: bool
    dropped: bool
    cut: bool
