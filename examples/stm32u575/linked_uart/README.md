# One controller and two responders linked by UART, HAL DMA

Three STM32U575 machines run on one virtual clock as a `popcorn.System`. A controller talks to
two responders over UART at 460800 baud, 8N1, OVER16:

- The controller's UART4 runs to responder 1's USART3, its USART3 to responder 2's USART3.
- Every link sends by GPDMA with `HAL_UART_Transmit_DMA` and receives with
  `HAL_UARTEx_ReceiveToIdle_DMA` into a 256-byte circular ring, a GPDMA linked list of one
  node. `HAL_UARTEx_RxEventCallback` copies the ring from its last read position up to the
  position the DMA remaining count gives, at half, complete and IDLE. IDLE ends a message.

The protocol, `firmware/controller.c` and `firmware/responder.c`:

- The controller sends each responder its node id, `N` and the id. The responder answers `n`
  and the id. Until then it answers nothing, and the controller sends the id again every 1.5 s.
- Every 16 ms the controller sends each responder a 512-byte block, `B`, a sequence number,
  then a counter. The responder answers with a 512-byte block holding its id, the sequence
  number and the block's checksum. The controller compares that checksum with its own.
- Every 100 ms the controller sends a 4-byte poll, the responder answers with its id.
- Both directions carry about 70 percent of the line.
- The controller marks a responder lost after 3 s without an answer and sends its node id
  again. A responder reports the controller lost after 5 s without a message.
- Both run at 160 MHz from HSI16 through PLL1, refresh a 500 ms IWDG before each WFI and set
  `DBG_IWDG_STOP`. A test sets `starve` to stop the refresh. Each prints link events on
  USART1 at 115200 baud.

## Layout

One root `conftest.py` declares the descriptions and the fixtures. The tests sit in three
folders, one per kind of test:

- `conftest.py`: `CONTROLLER` and `RESPONDER`, two `popcorn.MachineDescription` values, and
  `SYSTEM`, the `popcorn.SystemDescription` linking them. A plain `system_description` fixture
  hands `SYSTEM` to the tests that read or vary it.
- `controller/` and `responder/`: each image alone, on the `controller` and `responder1`
  machine fixtures.
- `system/`: the three machines linked, on the `system` fixture.

`responder1` and `responder2` are two machines of one description, restored from one boot. A
system member boots from the boot snapshot of its own description, the one the machine fixtures
use, so the system adds no boot.

## Declaring the fixtures

This is the end of `conftest.py`:

```python
@popcorn.pytest.machine
def controller() -> popcorn.MachineDescription:
    return CONTROLLER


@popcorn.pytest.machine
def responder1() -> popcorn.MachineDescription:
    return RESPONDER


@popcorn.pytest.machine
def responder2() -> popcorn.MachineDescription:
    return RESPONDER


@popcorn.pytest.system
def system() -> popcorn.SystemDescription:
    return SYSTEM
```

`SYSTEM` is built above them:

```python
LATENCY = us(20)

SYSTEM = popcorn.SystemDescription(
    members={"controller": CONTROLLER, "responder1": RESPONDER, "responder2": RESPONDER},
    links=[
        popcorn.UartLink("controller.UART4", "responder1.USART3", latency=LATENCY, baud=460_800),
        popcorn.UartLink("controller.USART3", "responder2.USART3", latency=LATENCY, baud=460_800),
    ],
)
```

A test takes the linked system and runs it:

```python
async def test_each_responder_gets_its_node_id(system: System) -> None:
    await system.run(for_=ms(1))
    assert system.members["responder1"].var("node_id").value == 1
```

`system.members[name]` is the member's `Machine`. `system.tap("controller.UART4")` shows every
frame of that link with its system times and injects, replaces or drops frames.
`system.snapshot()` and `system.restore()` save and restore every member and every link
together.

## Latency

A frame whose stop bit ends at the sender's system time t ends at the receiver at exactly
t + L. L is the link's `latency`. It has no default: the test author picks it as part of the
model, like a baud rate or a cable. Every hop adds L, so a request and its answer take 2 L more
than the firmware's own time. 20 us here is an example value.

The members meet every Q of system time, Q the smallest L of the system. Q sets how often they
meet, never what happens: the frames, their times and every member's state are the same at any
Q, any slicing and any thread timing. `test_the_run_is_the_same_at_any_slicing_quantum_or_cpu_count`
checks it against a 997-instruction slicing, a third 3 us link lowering Q, and every thread
pinned to one CPU.

## Threads

- Each member runs on its own native host thread.
- The members meet at a barrier every Q. Between barriers they run at once.
- A sender pushes each frame to its link's queue. After the barrier each receiver's thread
  drains its links up to the count sealed at the barrier, senders in member order, then links
  in their order in `links`.
- No Python runs on the link path.
- The Python callbacks of different members, intercepts, models and devices, run at the same
  wall time on their own member's thread, each in the context copied when it was installed.
  Python state two members share sees an order that depends on thread timing. Keep callback
  state per member, as `record_idles` in `system/test_linked_uart.py` does, and couple members
  through links.

## Halted members

A member without a goal while another runs one, `await controller.run(...)`, is halted: its core
executes nothing, its clocks, peripherals and DMA follow system time, and the DBGMCU freeze
bits it set apply. Both images set `DBG_IWDG_STOP`, so a halted member's watchdog does not
bite. Halted time counts in `Stop.instructions` and `Machine.instructions`, as a sleeping core's
time does. A goal's `budget` counts executed instructions only, halted time excluded.

## A pulled cable

A link's tap cuts the line over a window of system time, both directions, as a cable pulled
and plugged back:

```python
tap = system.tap("controller.UART4")
now = max(system.time_of(end.partition(".")[0]) for end in tap.ends)
start = VirtualTime(now.ps + ms(15).ps)
tap.cut(start, VirtualTime(start.ps + s(4).ps), line="break")
```

- `tap.cut(start, end)` schedules the window. `tap.break_line()` opens a cut now and
  `tap.restore_line()` closes it: stopped, or from a callback of the link's `tap.owner`, by
  default the member of `UartLink.a`, while the system runs.
- Times are system times at the senders, the base of `TapFrame.start`. Each receiver's line is
  cut over `[start + L, end + L)`.
- "Now" when stopped is the later end's `system.time_of`, never `system.time`: after a run a
  member stop ended, the members stand where the run left them, which depends on the quantum.
- A frame starting inside the cut never reaches the receiver, tapped `cut`. A frame crossing
  `start` or `end` is tapped `cut` and `lost`. A frame crossing `start` reaches the receiver up
  to `start + L` only.
- `line="idle"`, the default, keeps the line high: the frame crossing `start + L` puts
  nothing in RDR, and IDLE follows one frame later. `line="break"` holds it low: the receiver
  gets one break character with FE, then nothing until the line rises. Here FE ends the DMA
  reception in the HAL, and `HAL_UART_ErrorCallback` in `firmware/link.c` starts it again.

`test_a_pulled_cable_loses_the_responder_and_it_rejoins` cuts the controller's link to
responder 1 for 4 s, starting inside a block and its answer, with an idle line and with a
break. The controller marks the responder lost 3 s after its last answer, sends its node id
again every 1.5 s and finds it back after the restore, then blocks run again. Responder 1 never
reports the controller lost, 4 s being under its 5 s. Responder 2 is never lost, and no
checksum is wrong on either link.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32u575/linked_uart
```

## Read first

- `system/test_linked_uart.py`:
  - every block comes back with its checksum, checked from the tap and from the controller.
  - each IDLE at a responder comes one frame after the sender's stop-bit end plus L, the ring
    position equal to the bytes sent. The core wakes at its first cycle at or after IDLE and
    enters `USART3_IRQHandler` 4 cycles later.
  - the intercepts of two members run on two threads, neither the test's.
  - a 50 us link moves responder 1's IDLE by 30 us and the controller's answer by 60 us.
  - the same frames and IDLE log at any slicing, quantum or CPU count.
  - a starved responder's IWDG bites 500 ms after its last refresh, to the picosecond. Its
    frame in flight shows `lost` in the tap. It reboots, the controller marks it lost 3 s
    after its last answer and assigns it again, and responder 1 never misses a block.
  - a cable pulled for 4 s, the line idle or held low: the frames cut and lost at the cut's
    edges, the controller marking responder 1 lost 3 s after its last answer and finding it
    back after the restore, one FE on each side with a break.
  - a frame replaced from inside `system.running()` changes the checksum of its block.
  - a poll injected towards responder 2 is answered.
  - a snapshot restored replays the same frames.
- `firmware/link.c`: the ring, the IDLE cut and the TX gap.
- `firmware/controller.c` and `firmware/responder.c`: the protocol.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware/examples/stm32u575/linked_uart/linked_*.elf examples/stm32u575/linked_uart/prebuilt/
```
