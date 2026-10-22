from pathlib import Path

import popcorn
import popcorn.pytest
from popcorn import StopReason
from popcorn.model import IrqLine, Model, on_read, on_write
from popcorn.units import ms, us

ELF = Path(__file__).parents[3] / "examples/stm32u575/python_model/prebuilt/rng_numbers.elf"


# --8<-- [start:model]
class Counter(Model):
    irq: IrqLine
    served: int
    pending: int

    def __init__(self, start: int = 1) -> None:
        super().__init__()
        self.served = start
        self.pending = -1

    @on_write("CR")
    def _cr(self, old: int, new: int) -> None:
        if new & self.fields.CR.CONDRST:
            self._stop()
            self.regs.SR = 0
        elif not new & self.fields.CR.RNGEN:
            self._stop()
        elif not self.regs.SR & self.fields.SR.DRDY:
            self._start()
        self._follow()

    @on_read("DR")
    def _dr(self) -> int:
        if not self.regs.SR & self.fields.SR.DRDY:
            return 0
        word = self.served
        self.served += 1
        self.regs.SR &= ~self.fields.SR.DRDY
        if self.regs.CR & self.fields.CR.RNGEN:
            self._start()
        self._follow()
        return word

    def _start(self) -> None:
        if self.pending < 0:
            self.pending = self.schedule(us(40), self._ready)

    def _stop(self) -> None:
        if self.pending >= 0:
            self.cancel(self.pending)
            self.pending = -1

    def _ready(self) -> None:
        self.pending = -1
        self.regs.SR |= self.fields.SR.DRDY
        self._follow()

    def _follow(self) -> None:
        if self.regs.SR & self.fields.SR.DRDY and self.regs.CR & self.fields.CR.IE:
            self.irq.raise_()
        else:
            self.irq.lower()


# --8<-- [end:model]

# --8<-- [start:describe]
COUNTING = popcorn.MachineDescription(
    part="STM32U575QGIx",
    images=[popcorn.Elf(ELF)],
    boot=popcorn.Boot(until="main", budget=1_000_000),
    board={"RNG": popcorn.Periph(model=popcorn.Use(Counter, start=100))},
)


@popcorn.pytest.machine
def counting() -> popcorn.MachineDescription:
    return COUNTING


# --8<-- [end:describe]


async def test_the_firmware_reads_the_words_of_the_model(counting: popcorn.Machine) -> None:
    # --8<-- [start:run]
    stop = await counting.run(until="rng_done", for_=ms(20))
    assert stop.reason is StopReason.ADDRESS
    result = counting.var("rng_result")
    assert [int(result.words[i].value) for i in range(8)] == list(range(100, 108))

    model = counting.model("RNG")
    assert isinstance(model, Counter)
    assert model.served == 108
    # --8<-- [end:run]


async def test_a_register_answers_to_its_svd_name(counting: popcorn.Machine) -> None:
    # --8<-- [start:names]
    rcc = next(p for p in counting.description.peripherals if p.name == "RCC")
    assert rcc.register("RCC_CR").name == "CR"
    assert counting.periph.RCC.CR.value == counting.periph.RCC.RCC_CR.value
    # --8<-- [end:names]
