"""An AT modem on the host end of a UART, written in Python on `popcorn.board.UartStream`.

No device class exists for a UART: the far end of a USART is its `port`, a byte stream in
virtual time. The modem reads one command line at a time from it, looks its reply up in a
script, waits its answer delay in virtual time by running the machine, and writes the reply.
The modem runs in the test's coroutine, no thread of its own. A read of a stream is a
`Machine.wait_for` on a non-address event, which stops at the first event boundary or slice
end after the event became ready, so the machine stops a little after the command's last
frame. The modem times its reply from that frame's own time and runs to it with
`Machine.run(for_=)`. That time stop lands on the first boundary at or after the deadline:
less than one instruction late while the firmware sleeps in WFI, and up to one translated
block late while it executes code.
"""

from collections.abc import Mapping

from popcorn import Machine
from popcorn.board import UartStream
from popcorn.units import VirtualDuration, VirtualTime, ms

LINE_END = b"\r\n"
REPLY_DELAY = ms(5)
COMMAND_WAIT = ms(500)


class AtModem:
    """Answers AT commands on `port` from `replies`, keyed by the command without its line end.

    A command missing from `replies` gets `ERROR`. A command mapped to None gets no answer, as
    a modem that hangs. `commands` records each command and the virtual time its last byte
    arrived, `answered` each reply and the virtual time it was written.

    It takes the port as well as the machine, so it serves whichever `UartStream` the test
    hands it, not a fixed USART. `read_command` reads one frame per call, which
    keeps each byte's own time. `serve` runs the machine: do not run it as a background task
    next to another run or `running()`, which raises MachineBusy.
    """

    def __init__(
        self,
        machine: Machine,
        port: UartStream,
        replies: Mapping[bytes, bytes | None],
        *,
        delay: VirtualDuration = REPLY_DELAY,
    ) -> None:
        """Serve `port` of `machine`, answering `delay` after each command."""
        self._machine = machine
        self._port = port
        self._replies = dict(replies)
        self._delay = delay
        self.commands: list[tuple[bytes, VirtualTime]] = []
        self.answered: list[tuple[bytes, VirtualTime]] = []

    async def read_command(self, *, for_: VirtualDuration) -> bytes | None:
        """The next command line, None when none completes within `for_`."""
        line = b""
        deadline = self._machine.time + for_
        while not line.endswith(LINE_END):
            left = deadline - self._machine.time
            if left.ps <= 0:
                return None
            frames = await self._port.read_frames(1, for_=left)
            if not frames:
                return None
            line += bytes([frames[0].data])
            if line.endswith(LINE_END):
                self.commands.append((line[: -len(LINE_END)], frames[0].at))
        return line[: -len(LINE_END)]

    async def serve(self, count: int, *, for_: VirtualDuration = COMMAND_WAIT) -> None:
        """Answer `count` commands, each expected within `for_` of the previous one."""
        for _ in range(count):
            command = await self.read_command(for_=for_)
            if command is None:
                return
            reply = self._replies.get(command, b"ERROR\r\n")
            if reply is None:
                continue
            _, asked = self.commands[-1]
            await self._machine.run(for_=asked + self._delay - self._machine.time)
            self.answered.append((reply, self._machine.time))
            await self._port.write(reply)
