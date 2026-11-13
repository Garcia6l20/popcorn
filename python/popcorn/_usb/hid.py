from __future__ import annotations

from collections.abc import Iterable
from dataclasses import dataclass
from typing import TYPE_CHECKING, cast, override

from popcorn import _core
from popcorn._errors import EmulationError, UsbError, UsbTimeout
from popcorn._streams import (
    Poster,
    Waiter,
    check_wait,
    cutoff,
)
from popcorn._usb.transfer import UsbData, await_result, queue_untaken, raise_for
from popcorn.units import VirtualDuration, VirtualTime

if TYPE_CHECKING:
    from popcorn._refs import Pending

FEATURE = 3


@dataclass(frozen=True, slots=True)
class HidReport:
    """One input report a HID function sent on its interrupt IN endpoint.

    Attributes:
        data: Its bytes as the packet carried them, the report ID first when the device numbers
            its reports, as hidraw reads them.
        at: Virtual time of the transaction that carried it.
    """

    data: bytes
    at: VirtualTime


class HidStream:
    """A HID interface of a USB device, `m.periph.OTG_FS.host.hid()`: input reports with their
    times, output and feature reports, in virtual time.

    The host's HID driver binds each interface of class 0x03 whose HID descriptor announces a
    report descriptor. At configuration it probes as Linux usbhid does: SET_IDLE with duration 0
    for every report, then GET_DESCRIPTOR of the report descriptor, tried up to 4 times while it
    comes back short or fails. Once the descriptor is read it keeps one interrupt IN transfer of
    one packet pending, polled every bInterval frames, as if hidraw were open: an input report is
    one packet. An interrupt IN STALL is cleared as usbhid does, with
    CLEAR_FEATURE(ENDPOINT_HALT), and reading goes on. A failed descriptor read leaves the
    function idle, with a warning on the controller's log.

    Reports follow hidraw: the report ID comes first, and an ID of 0, for a device that does not
    number its reports, is not sent. Output reports go out one at a time on the interrupt OUT
    endpoint when the interface has one, else as SET_REPORT(Output) on the default pipe. Input
    reports are queued up to 65536, the oldest dropped first and counted in `dropped`.

    The stream outlives the driver: it exists before the device is configured, an unplug keeps
    the reports received and the output reports not sent yet, and a replug binds the same stream
    again. `Machine.restore` drops the input reports not read yet, and puts back the output
    reports not yet taken by the device as the snapshot had them, bound or not.
    """

    __slots__ = (
        "__weakref__",
        "_link",
        "_pending",
        "_post",
        "_wait",
        "_writes",
        "function",
        "name",
    )

    def __init__(
        self,
        name: str,
        function: int,
        link: _core.HidLink,
        wait: Waiter,
        post: Poster,
    ) -> None:
        """Wrap a native link. Use `m.periph.<NAME>.host.hid()` instead."""
        self.name = name
        self.function = function
        self._link = link
        self._wait = wait
        self._post = post
        self._pending: set[int] = set()
        self._writes: set[int] = set()

    @property
    def bound(self) -> bool:
        """Whether the host has bound its HID driver to the interface.

        True from the moment the device is configured, before SET_IDLE and the report descriptor
        read: no input report is polled and output reports still wait. `ready` tells when they
        start. False again once the device is unplugged, reset by the bus or restored to a
        snapshot taken before configuration.
        """
        return self._link.bound()

    @property
    def ready(self) -> bool:
        """Whether the interface is bound, the driver has read the report descriptor and has
        started polling.

        At that point the driver has submitted its first interrupt IN transfer and output reports
        may go out. A function whose descriptor read failed stays idle and never becomes ready.
        """
        return self._link.bound() and self._link.ready()

    @property
    def report_descriptor(self) -> bytes:
        """The report descriptor the driver read, raw, empty before. An unplug keeps it."""
        return self._link.report_descriptor()

    @property
    def available(self) -> int:
        """Input reports received, not read yet."""
        return self._link.available()

    @property
    def queued(self) -> int:
        """Output reports written and not yet taken by the device, the one in progress
        included."""
        return self._link.queued()

    @property
    def dropped(self) -> int:
        """Input reports dropped because the queue held its limit, since the machine was built
        or the last `Machine.restore`."""
        return self._link.dropped()

    async def wait_ready(
        self,
        *,
        for_: VirtualDuration | None = None,
        deadline: VirtualTime | None = None,
    ) -> None:
        """Run the machine until `ready`, or `for_` elapsed, as `CdcStream.wait_ready`.

        `deadline` stops waiting at this virtual time instead, exclusive with `for_`. Already
        past, the wait ends at once.

        Raises:
            TypeError: Both `for_` and `deadline` given, or `deadline` is no `VirtualTime`.
            UsbTimeout: The function is not ready when the run ends: `for_` elapsed,
                `deadline` passed or the core went idle. Its status is `unbound` while no
                driver is bound, else `not_ready`.
            EmulationError: The run stopped for another reason, such as a fault, or, inside
                `running()`, the free run ended by itself, a handler's USER stop included.
        """
        check_wait(for_, deadline)
        if not self.ready:
            await self._wait(self._link.predicate_ready(), for_, deadline)
        if not self.ready:
            status = "not_ready" if self.bound else "unbound"
            raise UsbTimeout(f"{self._where}: not ready, {status}", status=status)

    async def read_reports(
        self,
        n: int,
        *,
        for_: VirtualDuration | None = None,
        deadline: VirtualTime | None = None,
    ) -> list[HidReport]:
        """Run the machine until `n` input reports arrived, or `for_` elapsed, and take them.

        `deadline` stops waiting at this virtual time instead, exclusive with `for_`, and caps
        what the read takes: no report the host queued past it, even one queued at the call.
        Already past, the read ends at once with the reports queued by it.

        A read that waits takes only the reports the host queued by the time its wait stopped,
        in one native step: later reports stay queued for the next read, even those a free run
        queued before this read resumed. So with `deadline` or neither, the reports depend only
        on what the device sent, the same parked and running. Inside a free run `for_` counts
        from the boundary where the wait registered, which wall time sets. The host runs every
        transaction of a frame at the frame's start, so a report taken can carry an `at` up to
        about 1 ms past that time.

        Returns:
            Up to `n` reports, oldest first, fewer when `for_` elapsed, `deadline` passed or
                the core went idle.

        Raises:
            TypeError: Both `for_` and `deadline` given, or `deadline` is no `VirtualTime`.
            ValueError: `n` is negative.
            EmulationError: The run stopped for another reason, such as a fault, or, inside
                `running()`, the free run ended by itself, a handler's USER stop included.
        """
        check_wait(for_, deadline)
        if n < 0:
            raise ValueError(f"cannot read {n} reports")
        if self._link.available() >= n:
            return _hid_reports(self._link.drain_until(n, cutoff(None, deadline)))
        stop = await self._wait(self._link.predicate_available(n), for_, deadline)
        return _hid_reports(self._link.drain_until(n, cutoff(stop, deadline)))

    def take_reports(self) -> list[HidReport]:
        """Every input report received and not read yet, at once, never running the machine.
        Allowed parked and running, from any thread. A report arriving during the call stays
        queued for the next one. A take while a `read_reports` of the same stream waits can
        leave that read short."""
        link = self._link
        return _hid_reports(link.drain(link.available()))

    def write_report(self, data: bytes, *, at: VirtualTime | None = None) -> Pending[VirtualTime]:
        """Send an output report and run the machine until the device took it.

        The report is queued at the call as one command, at the next block boundary or the
        first one at or after `at`, as `UartStream.write`. Awaiting the result waits for the
        device and returns the time the report was queued at. It waits behind the output
        reports written before, and until the device is configured and its report descriptor
        read. When the await raises EmulationError or is cancelled, or the result is dropped
        unawaited, the report stays queued and is sent later, as the machine runs: a failure is
        then logged as a popcorn warning. A report in progress when the device detaches that
        nobody awaits is dropped, and the unplug or bus reset logs one popcorn warning for it
        per machine and raises `ModelError` for every such drop under `strict_peripherals`.

        Args:
            data: The report as hidraw takes it: the report ID first, 0 when the device does not
                number its reports, which is then not sent.

        Raises:
            ValueError: `data` is shorter than 2 bytes, as hidraw refuses, or carries more than
                0xffff bytes on the bus, the ID 0 dropped.
            UsbStall: The device answered STALL.
            UsbTimeout: The SET_REPORT did not end within the host's timeout.
            UsbError: The transfer ended otherwise, a detach included.
            EmulationError: The run ended before the device took the report.
            RuntimeError: Called from a handler or a model.
        """
        data = bytes(data)
        if len(data) < 2:
            raise ValueError(f"an output report is at least 2 bytes, not {len(data)}")
        stage = len(data) - 1 if data[0] == 0 else len(data)
        if stage > 0xFFFF:
            raise ValueError(
                f"an output report carries at most 0xffff bytes on the bus, not {stage}"
            )
        report = data[0]
        claim = self._link.write_claim()
        where = self._where
        posted = self._post(
            claim.command_write(data),
            at,
            cancel=False,
            follow=lambda queued, future: self._taken(queued, report, future),
            dropped=lambda future: queue_untaken(future, where, report, claim.release()),
        )
        return cast("Pending[VirtualTime]", posted)

    async def _taken(
        self, queued: tuple[int, VirtualTime], report: int, future: _core.HostFuture
    ) -> VirtualTime:
        write, at = queued
        self._writes.add(write)
        results: list[_core.UsbTransferStatus] = []

        def ended() -> bool:
            if not results and write in self._writes:
                status = self._link.take_written(write)
                if status is not None:
                    results.append(status)
            return bool(results)

        try:
            if not ended():
                await self._wait(self._link.predicate_written(write), None, None)
                ended()
        except BaseException:
            if not results:
                queue_untaken(future, self._where, report, self._link.release_written(write))
            raise
        finally:
            self._writes.discard(write)
        if not results:
            late = self._link.release_written(write)
            if late is not None:
                results.append(late)
        label = f"output report {report}"
        if not results:
            raise EmulationError(f"{self._where}: the run ended before {label} did")
        raise_for(results[0], f"{self._where}: {label}")
        return at

    async def get_feature(self, report_id: int, length: int) -> UsbData:
        """Send GET_REPORT(Feature) and run the machine until it ends, as hidraw's
        HIDIOCGFEATURE.

        Args:
            report_id: Report ID, 0 when the device does not number its reports.
            length: Bytes asked for, the report ID byte included. For ID 0 the request asks one
                byte fewer, the ID is not on the bus.

        Returns:
            The report, its ID first: the bytes the device sent, after a 0 for ID 0, with the
            virtual time the request was seen ended as `.time`.

        Raises:
            ValueError: `report_id` is not 0 to 255, or `length` not 1 to 0xffff.
            UsbError: No HID driver is bound to the interface, or the request ended neither
                completed, stalled nor timed out.
            UsbStall: The device answered STALL.
            UsbTimeout: The request did not end within the host's timeout.
            RuntimeError: Called from a handler or a model.
        """
        if not 0 <= report_id <= 0xFF:
            raise ValueError(f"a report ID is 0 to 255, not {report_id}")
        if not 1 <= length <= 0xFFFF:
            raise ValueError(f"length is 1 to 0xffff, not {length}")
        w_length = length - 1 if report_id == 0 else length
        link = self._link
        transfer, at = await self._post(link.command_get_report(FEATURE, report_id, w_length))
        reply, done_at = await self._request(transfer, f"GET_REPORT feature {report_id}", at)
        return UsbData((b"\x00" if report_id == 0 else b"") + bytes(reply.data), done_at)

    async def set_feature(self, data: bytes) -> VirtualTime:
        """Send SET_REPORT(Feature) and run the machine until it ends, as hidraw's
        HIDIOCSFEATURE, the virtual time it was seen ended as the result.

        Args:
            data: The report, its ID first, 0 when the device does not number its reports, which
                is then not sent.

        Raises:
            ValueError: `data` is empty, or its data stage longer than 0xffff bytes.
            UsbError: No HID driver is bound to the interface, or the request ended neither
                completed, stalled nor timed out.
            UsbStall: The device answered STALL.
            UsbTimeout: The request did not end within the host's timeout.
            RuntimeError: Called from a handler or a model.
        """
        data = bytes(data)
        if not data:
            raise ValueError("a feature report has at least its report ID byte")
        stage = data[1:] if data[0] == 0 else data
        if len(stage) > 0xFFFF:
            raise ValueError(f"a data stage is at most 0xffff bytes, not {len(stage)}")
        link = self._link
        transfer, at = await self._post(link.command_set_report(FEATURE, data[0], stage))
        _, done_at = await self._request(transfer, f"SET_REPORT feature {data[0]}", at)
        return done_at

    def discard(self) -> None:
        """Forget the requests and reports waited for, at a restore."""
        self._pending.clear()
        self._writes.clear()

    @property
    def _where(self) -> str:
        return f"{self.name} HID function {self.function}"

    async def _request(
        self, transfer: int, label: str, posted_at: VirtualTime
    ) -> tuple[_core.HidReplyRecord, VirtualTime]:
        if transfer == 0:
            raise UsbError(f"{self._where}: {label} with no HID driver bound", status="unbound")
        result, done_at = await await_result(
            self._link,
            self._pending,
            self._wait,
            transfer,
            posted_at,
            f"{self._where}: the run ended before {label} did",
        )
        raise_for(result.status, f"{self._where}: {label}")
        return result, done_at

    @override
    def __repr__(self) -> str:
        return f"HidStream({self.name}, {self.function})"


def _hid_reports(records: Iterable[_core.HidReportRecord]) -> list[HidReport]:
    return [HidReport(bytes(record.data), VirtualTime(record.at_ps)) for record in records]
