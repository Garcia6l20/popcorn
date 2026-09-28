"""The examples of spec 03, on popcorn's own test firmware and names."""

from __future__ import annotations

import ctypes
from pathlib import Path

import popcorn
from popcorn.model import IrqLine, Model, on_read, on_write
from popcorn.units import ms, us


async def project_and_machine(project_file: Path) -> popcorn.Snapshot:
    """Section 1: a project file, a machine, a boot snapshot."""
    project = popcorn.Project.load(project_file)
    m: popcorn.Machine = project.machine()
    m.reset()
    stop = await m.run(until=m.sym("alu_mark"), budget=500_000)
    assert stop.reason is popcorn.StopReason.ADDRESS
    return m.snapshot()


def machine_without_project(description: popcorn.MachineDescription, elf: Path) -> popcorn.Machine:
    """Section 1: a machine built directly, for small tests."""
    return popcorn.Machine(description, image=popcorn.Image.elf(elf))


async def running(m: popcorn.Machine) -> popcorn.Stop:
    """Section 2: run, step, wait, a stop predicate."""
    await m.step(4)
    await m.run(for_=us(10))
    await m.wait_for(popcorn.Reached("alu_mark"), for_=ms(10))
    return await m.run(budget=1_000, stop_when=lambda machine: machine.instructions > 100)


async def calling(m: popcorn.Machine) -> tuple[int, int]:
    """Section 3: arena buffers and DWARF-typed arguments."""
    values = m.arena.alloc_array(ctypes.c_int32, 4)
    values.write([1, 2, 3, 4])
    point = m.arena.alloc(m.types["struct point"])
    await m.call("fill", point, 21)
    r = await m.call("add", 20, 22, budget=2_000)
    assert r.stop.reason is popcorn.StopReason.RETURNED
    return int(r.value), int(point.var.x.value)


def memory(m: popcorn.Machine) -> tuple[bytes, int]:
    """Section 4: memory, symbols, DWARF views, registers."""
    m.mem.u32[0x2000_0000] = 5
    v = m.mem.read(m.sym("table"), 4)
    table = m.var("table")
    target = int(table.entries[3].target.value)
    m.reg.r0 = 1
    _ = m.reg.basepri
    _ = m.reg[popcorn.arm.Reg.PSP]
    return v, target


async def intercepts(m: popcorn.Machine) -> list[popcorn.LogEntry]:
    """Section 5: intercepts, scoped intercepts, logs, return hooks, scripted replies."""

    @m.intercept("hal_delay")
    def _(call: popcorn.Call) -> popcorn.Return:
        return call.returns()

    with m.intercepted("next_value", lambda call: call.returns(7)):
        await m.call("run_script")

    @m.on_return("triple")
    def _check(ret: popcorn.Returned) -> None:
        a, b, c = (int(x) for x in ret.args)
        assert ret.value == a * 100 + b * 10 + c

    m.script("next_value", [1, 2])
    await m.call("use_triple")
    return m.intercept_log("next_value")


async def board(m: popcorn.Machine) -> bytes:
    """Section 6: peripheral registers, pins, UART streams, ptys, the NVIC."""
    _ = m.periph.USART1.BRR.value
    m.periph.GPIOA.pin(0).level = 1
    uart = m.periph.USART1.port
    await uart.write(b"ping\r\n")
    data = await uart.read(6, for_=ms(10))
    _ = m.board.pty("USART1")
    m.nvic.pend(m.irq.WWDG_IRQn)
    return data


class Counter(Model):
    """Section 7: a peripheral model on the register layer."""

    irq: IrqLine
    expirations: int = 0

    @on_write("CR")
    def _cr(self, old: int, new: int) -> None:
        if new & self.fields.CR.EN and not old & self.fields.CR.EN:
            self.schedule(us(self.regs.ARR), self._expire)

    @on_read("CNT")
    def _cnt(self) -> int:
        return self.expirations

    @on_write("SR")
    def _sr(self, old: int, new: int) -> None:
        if not new & self.fields.SR.UIF:
            self.irq.lower()

    def _expire(self) -> None:
        self.expirations += 1
        self.regs.SR |= self.fields.SR.UIF
        self.irq.raise_()


def snapshots(m: popcorn.Machine, path: Path) -> popcorn.Snapshot:
    """Section 8: snapshot, restore, save, load."""
    s = m.snapshot()
    m.restore(s)
    s.save(path)
    return m.load_snapshot(path)


async def observability(m: popcorn.Machine, directory: Path) -> str:
    """Section 9: traces, coverage, the ring."""
    with m.trace(popcorn.trace.ALL, directory / "trace.pctr"):
        await m.run(budget=1_000)
    records = list(popcorn.trace.read(directory / "trace.pctr"))
    assert records
    m.coverage.enable()
    await m.run(budget=1_000)
    m.coverage.save(directory / "w0.pcov")
    cov = popcorn.coverage.merge([directory / "w0.pcov"])
    cov.export_lcov(directory / "cov.info", branches=True)
    popcorn.trace.to_chrome(directory / "trace.pctr", directory / "trace.json")
    return m.ring.dump()
