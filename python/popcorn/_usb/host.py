from __future__ import annotations

import functools
from collections.abc import Callable, Iterable, Mapping
from typing import TYPE_CHECKING, cast, override

from popcorn import _core
from popcorn._errors import ConfigError, EmulationError, ModelError, UsbError, UsbStall, UsbTimeout
from popcorn._streams import (
    Poster,
    PtyBridge,
    Waiter,
    check_wait,
    written,
)
from popcorn._usb.cdc import CdcStream
from popcorn._usb.descriptors import (
    LANGUAGES,
    UsbHostOptions,
    UsbInterface,
    UsbRequest,
    UsbState,
    interface_of,
    requests_of,
)
from popcorn._usb.hid import HidStream
from popcorn._usb.transfer import UsbData
from popcorn._usb.uac import UacFunction
from popcorn.units import VirtualDuration, VirtualTime

if TYPE_CHECKING:
    from popcorn._refs import Pending


class UsbHost:
    """The virtual USB host of a device controller, `m.periph.OTG_FS.host`.

    The host lives in the machine, inside the controller model: it sees the pull-up, debounces,
    resets the bus and enumerates without Python, in virtual time. Its state, the cable
    included, is in snapshots, so a snapshot taken once the device is configured restores a
    configured device. Results of control requests not collected yet are dropped by
    `Machine.restore`.

    `attached`, `state`, `address`, `configuration`, `failed_step`, the descriptors, `strings`,
    `interfaces`, `requests` and the first `cdc`, `hid` or `audio` call of a function read the
    host: they raise MachineRunning while the machine is busy, outside its callbacks. The queue
    and state properties of CDC, HID and audio streams read their link and work while it runs.
    """

    __slots__ = (
        "_audio",
        "_audio_link",
        "_bridges",
        "_cdc",
        "_cdc_link",
        "_hid",
        "_hid_link",
        "_link",
        "_post",
        "_wait",
        "name",
    )

    def __init__(
        self,
        name: str,
        link: _core.UsbLink,
        wait: Waiter,
        post: Poster,
        bridges: Mapping[str, PtyBridge],
        cdc_link: Callable[[int], _core.CdcLink],
        hid_link: Callable[[int], _core.HidLink],
        audio_link: Callable[[int], _core.UacLink],
    ) -> None:
        """Wrap a native link. `cdc_link`, `hid_link` and `audio_link` give the link of a CDC,
        HID or audio function by index. Use `m.periph.<NAME>.host` instead."""
        self.name = name
        self._link = link
        self._wait = wait
        self._post = post
        self._bridges = bridges
        self._cdc_link = cdc_link
        self._cdc: dict[int, CdcStream] = {}
        self._hid_link = hid_link
        self._hid: dict[int, HidStream] = {}
        self._audio_link = audio_link
        self._audio: dict[int, UacFunction] = {}

    @property
    def attached(self) -> bool:
        """Whether the cable is plugged.

        Raises:
            MachineRunning: The machine is busy, read outside its callbacks.
        """
        return self._link.plugged()

    async def attach(self) -> VirtualTime:
        """Plug the cable, as one command at the next block boundary, at once while the
        machine is parked: the host sees the device's pull-up, then debounces and enumerates as
        the machine runs.

        Returns:
            The virtual time the cable was plugged at.

        Raises:
            RuntimeError: Called from a handler or a model.
        """
        link = self._link
        _, at = await self._post(link.command_plug(True))
        return at

    def detach(self, *, at: VirtualTime | None = None) -> Pending[VirtualTime]:
        """Unplug the cable, posted at the call as one command, as `UacStream.stop`: VBUS
        drops, the device is gone for the host, transfers in flight end cancelled. Await the
        result for the virtual time it ran at.

        Raises:
            RuntimeError: Called from a handler or a model.
            ValueError: `at` before the time of a parked machine, from the await when running.
        """
        link = self._link
        return written(self._post(link.command_plug(False), at))

    @property
    def state(self) -> UsbState:
        """Device state as the host sees it."""
        return UsbState(self._link.state().value)

    @property
    def address(self) -> int:
        """Address the host gave the device, 0 before SET_ADDRESS."""
        return self._link.address()

    @property
    def configuration(self) -> int:
        """bConfigurationValue the host set, 0 when not configured."""
        return self._link.configuration_value()

    @property
    def failed_step(self) -> str | None:
        """The enumeration step that failed, such as `get_configuration`, None while none did."""
        return self._link.failed_step()

    @property
    def device_descriptor(self) -> bytes:
        """The device descriptor read during enumeration, empty before."""
        return self._link.device_descriptor()

    @property
    def configuration_descriptor(self) -> bytes:
        """The configuration descriptor read during enumeration, all wTotalLength bytes once
        read, empty before."""
        return self._link.configuration_descriptor()

    @property
    def strings(self) -> dict[int, str]:
        """The strings read during enumeration by index, in the first language the device
        offers. String 0, the language list, is not a string and is left out."""
        return {
            record.index: bytes(record.bytes[2:]).decode("utf-16-le", errors="replace")
            for record in self._link.strings()
            if record.index != LANGUAGES
        }

    @property
    def interfaces(self) -> tuple[UsbInterface, ...]:
        """Every interface and alternate setting of the configuration read, in descriptor
        order, empty before it is read."""
        return tuple(interface_of(record) for record in self._link.interfaces())

    @property
    def requests(self) -> tuple[UsbRequest, ...]:
        """Every control request the host sent since the machine was built, oldest first:
        enumeration, class drivers and `control` alike. Not part of snapshots."""
        return requests_of(self._link.requests())

    def cdc(self, function: int = 0) -> CdcStream:
        """The stream of a CDC ACM function of the device, the same one on later calls.

        Args:
            function: The n-th CDC ACM function in interface order, 0 for the first.

        Raises:
            ValueError: `function` is negative, or the configuration is read and has no such
                function. Before it is read, any index is taken.
        """
        stream = self._cdc.get(function)
        if stream is None:
            link = self._cdc_link(function)
            stream = self._cdc[function] = CdcStream(
                self.name, function, link, self._wait, self._bridges, self._post
            )
        return stream

    def hid(self, function: int = 0) -> HidStream:
        """The stream of a HID interface of the device, the same one on later calls.

        Args:
            function: The n-th HID interface in interface order, 0 for the first.

        Raises:
            ValueError: `function` is negative, or the configuration is read and has no such
                interface. Before it is read, any index is taken.
        """
        stream = self._hid.get(function)
        if stream is None:
            link = self._hid_link(function)
            stream = self._hid[function] = HidStream(
                self.name, function, link, self._wait, self._post
            )
        return stream

    def audio(self, function: int = 0) -> UacFunction:
        """A USB Audio 2.0 function of the device, the same one on later calls.

        Args:
            function: The n-th audio control interface in interface order, 0 for the first.

        Raises:
            ValueError: `function` is negative, or the configuration is read and has no such
                function. Before it is read, any index is taken.
        """
        found = self._audio.get(function)
        if found is None:
            link = self._audio_link(function)
            found = self._audio[function] = UacFunction(
                self.name, function, link, self._wait, self._post
            )
        return found

    async def wait_configured(
        self,
        *,
        for_: VirtualDuration | None = None,
        deadline: VirtualTime | None = None,
    ) -> None:
        """Run the machine until the host configured the device, or `for_` elapsed.

        Inside `running()`, this awaits the background run instead of driving the machine. The
        state and failed step it reports are read at the boundary after the one where the
        device settled, so a change in between, a disconnect right after configuration, shows.

        `deadline` stops waiting at this virtual time instead, exclusive with `for_`. Already
        past, the wait ends at once.

        Raises:
            TypeError: Both `for_` and `deadline` given, or `deadline` is no `VirtualTime`.
            UsbTimeout: The device is not configured when the run ends: `for_` elapsed,
                `deadline` passed, the core went idle, or the enumeration stopped on a failed
                step.
            EmulationError: The run stopped for another reason, such as a fault, or, inside
                `running()`, the free run ended by itself, a handler's USER stop included.
        """
        check_wait(for_, deadline)
        configured = UsbState.CONFIGURED.value
        link = self._link
        await self._wait(link.predicate_settled(), for_, deadline)
        settled, _ = await self._post(link.op_settled())
        state, failed = cast(tuple[int, str | None], settled)
        if state != configured:
            status = failed or UsbState(state).name
            reason = f"enumeration failed at {failed}" if failed else f"state {status}"
            raise UsbTimeout(f"{self.name}: the device is not configured, {reason}", status=status)

    async def control(
        self, request_type: int, request: int, value: int, index: int, data_or_length: bytes | int
    ) -> UsbData:
        """Send a control request on the default pipe and run the machine until it ends.

        A call cancelled or failing at any point gives its result up: the machine drops it when
        it waits or once it lands.

        Args:
            request_type: bmRequestType. Bit 7 set is IN, device to host.
            request: bRequest.
            value: wValue.
            index: wIndex.
            data_or_length: wLength for an IN request or an OUT request with no data stage,
                the data stage of an OUT request.

        Returns:
            The data stage received for IN, at most wLength bytes, empty for OUT, with the
            virtual time the request ended as `.time`.

        Raises:
            ValueError: A field is out of range, an IN request is given data, or an OUT request
                a length above 0 instead of its data.
            RuntimeError: Called from a handler or a model.
            UsbStall: The device answered STALL.
            UsbTimeout: The request did not end within the host's timeout, 5 s by default.
            UsbError: The request ended otherwise: no handshake three times in a row, more data
                than wLength, or a detach.
            EmulationError: The run stopped for another reason, such as a fault, or, inside
                `running()`, the free run ended by itself, a handler's USER stop included.
        """
        for label, field, top in (
            ("request_type", request_type, 0xFF),
            ("request", request, 0xFF),
            ("value", value, 0xFFFF),
            ("index", index, 0xFFFF),
        ):
            if not 0 <= field <= top:
                raise ValueError(f"{label} is 0 to {top:#x}, not {field}")
        is_in = bool(request_type & 0x80)
        if isinstance(data_or_length, int):
            length, data = data_or_length, b""
            if not 0 <= length <= 0xFFFF:
                raise ValueError(f"wLength is 0 to 0xffff, not {length}")
            if length and not is_in:
                raise ValueError("an OUT request takes its data stage as bytes, not a length")
        else:
            data = bytes(data_or_length)
            length = len(data)
            if is_in:
                raise ValueError("an IN request takes wLength, not data")
            if length > 0xFFFF:
                raise ValueError(f"an OUT data stage is at most 0xffff bytes, not {length}")
        link = self._link
        ticket = link.ticket()
        took = False
        try:
            transfer, _ = await self._post(
                link.command_control(ticket, request_type, request, value, index, length, data)
            )
            await self._wait(link.predicate_control_ended(transfer), None, None)
            taken, _ = await self._post(link.op_take_result(ticket))
            took = taken is not None
        finally:
            if not took:
                link.forget(ticket)
        result = cast(_core.UsbControlResult | None, taken)
        setup = f"{request_type:#04x} {request:#04x} {value:#06x} {index:#06x} {length}"
        if result is None:
            raise EmulationError(f"{self.name}: the run ended before control {setup} did")
        status = result.status.name
        at = VirtualTime(result.at_ps)
        if result.status is _core.UsbTransferStatus.completed:
            return UsbData(bytes(result.data) if is_in else b"", at)
        message = f"{self.name}: control {setup} ended {status} at {at}"
        if result.status is _core.UsbTransferStatus.stalled:
            raise UsbStall(message, status=status)
        if result.status is _core.UsbTransferStatus.timed_out:
            raise UsbTimeout(message, status=status)
        raise UsbError(message, status=status)

    def discard(self) -> None:
        """Drop the results not collected yet, at a restore."""
        for stream in self._cdc.values():
            stream.discard()
        for hid in self._hid.values():
            hid.discard()
        for audio in self._audio.values():
            audio.discard()

    @override
    def __repr__(self) -> str:
        return f"UsbHost({self.name})"


class UsbHosts:
    """The virtual USB hosts of a machine, one per device controller given one."""

    __slots__ = (
        "_aliases",
        "_audio_links",
        "_bridges",
        "_cdc_links",
        "_connect",
        "_hid_links",
        "_hosts",
        "_links",
        "_names",
        "_post",
        "_wait",
    )

    def __init__(
        self,
        connect: Callable[[str], _core.UsbLink],
        names: Iterable[str],
        aliases: Mapping[str, str],
        wait: Waiter,
        post: Poster,
        bridges: Mapping[str, PtyBridge] | None = None,
    ) -> None:
        """Serve the hosts of the controllers `names` through `connect`. `bridges` are the
        machine's ptys, which CDC streams check before a read."""
        self._connect = connect
        self._names = frozenset(names)
        self._aliases = dict(aliases)
        self._wait = wait
        self._post = post
        self._bridges: Mapping[str, PtyBridge] = {} if bridges is None else bridges
        self._hosts: dict[str, UsbHost] = {}
        self._cdc_links: dict[tuple[str, int], _core.CdcLink] = {}
        self._hid_links: dict[tuple[str, int], _core.HidLink] = {}
        self._audio_links: dict[tuple[str, int], _core.UacLink] = {}
        self._links: dict[str, _core.UsbLink] = {}

    def serves(self, name: str) -> bool:
        """Whether `name`, or the controller its alias names, has a virtual host."""
        return self._aliases.get(name, name) in self._names

    def host(self, name: str) -> UsbHost:
        """The host of controller `name`, an alias naming the controller serving it.

        Raises:
            ModelError: The controller has no virtual host.
        """
        name = self._aliases.get(name, name)
        found = self._hosts.get(name)
        if found is None:
            if name not in self._names:
                raise ModelError(
                    f"{name} has no virtual USB host: add `Periph(host=UsbHostOptions())` to its"
                    f" board entry, or pass usb={{{name!r}: UsbHostOptions()}} to Machine",
                    peripheral=name,
                )
            found = self._hosts[name] = UsbHost(
                name,
                self._links.setdefault(name, self._connect(name)),
                self._wait,
                self._post,
                self._bridges,
                functools.partial(self.cdc_link, name),
                functools.partial(self.hid_link, name),
                functools.partial(self.audio_link, name),
            )
        return found

    def cdc_link(self, name: str, function: int) -> _core.CdcLink:
        """The native link of CDC ACM function `function` of the host of controller `name`,
        shared by its stream and its pty.

        Raises:
            ModelError: The controller has no virtual host.
            ValueError: `function` is negative, or the configuration is read and has no such
                function.
        """
        host = self.host(name)
        if function < 0:
            raise ValueError(f"a CDC function index is 0 or more, not {function}")
        native = self._links[host.name]
        count = native.cdc_functions()
        if count is not None and function >= count:
            raise ValueError(
                f"{host.name}: the device has {count} CDC ACM functions, no {function}"
            )
        key = (host.name, function)
        link = self._cdc_links.get(key)
        if link is None:
            link = self._cdc_links[key] = native.cdc(function)
        return link

    def hid_link(self, name: str, function: int) -> _core.HidLink:
        """The native link of HID function `function` of the host of controller `name`.

        Raises:
            ModelError: The controller has no virtual host.
            ValueError: `function` is negative, or the configuration is read and has no such
                function.
        """
        host = self.host(name)
        if function < 0:
            raise ValueError(f"a HID function index is 0 or more, not {function}")
        native = self._links[host.name]
        count = native.hid_functions()
        if count is not None and function >= count:
            raise ValueError(f"{host.name}: the device has {count} HID functions, no {function}")
        key = (host.name, function)
        link = self._hid_links.get(key)
        if link is None:
            link = self._hid_links[key] = native.hid(function)
        return link

    def audio_link(self, name: str, function: int) -> _core.UacLink:
        """The native link of USB Audio 2.0 function `function` of the host of controller
        `name`.

        Raises:
            ModelError: The controller has no virtual host.
            ValueError: `function` is negative, or the configuration is read and has no such
                function.
        """
        host = self.host(name)
        if function < 0:
            raise ValueError(f"an audio function index is 0 or more, not {function}")
        native = self._links[host.name]
        count = native.audio_functions()
        if count is not None and function >= count:
            raise ValueError(f"{host.name}: the device has {count} audio functions, no {function}")
        key = (host.name, function)
        link = self._audio_links.get(key)
        if link is None:
            link = self._audio_links[key] = native.audio(function)
        return link

    def discard(self) -> None:
        """Drop the results not collected yet of every host, at a restore."""
        for host in self._hosts.values():
            host.discard()


def checked_usb(
    usb: Mapping[str, UsbHostOptions], aliases: Mapping[str, str]
) -> dict[str, _core.UsbHostConfig]:
    """`usb` keyed by the controller serving each key, as the native configuration takes it.

    Raises:
        ConfigError: Two keys name one controller, or a duration is not above 0 where it must
            be, named as `usb.<key>`.
    """
    checked: dict[str, _core.UsbHostConfig] = {}
    for key, options in usb.items():
        name = aliases.get(key, key)
        at = f"usb.{key}"
        if name in checked:
            raise ConfigError(
                f"{at}: {name} is listed twice, once through an alias",
                key=at,
                expected="one entry per USB device controller",
            )
        if options.debounce.ps < 0 or options.reset.ps <= 0 or options.timeout.ps <= 0:
            raise ConfigError(
                f"{at}: a debounce of 0 or more, a bus reset and a timeout above 0",
                key=at,
                expected="a debounce of 0 or more, a bus reset and a timeout above 0",
            )
        checked[name] = options.native()
    return checked
