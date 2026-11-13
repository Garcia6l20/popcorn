"""The console blocks of the "popcorn" report section: which machines show one, what it holds,
and how a bad console name fails the setup."""

from __future__ import annotations

import re
from pathlib import Path

import popcorn
import pytest
from popcorn.pytest._report import names_culprit
from popcorn.units import VirtualTime

from .helpers import BOARD_ELF, FAULT_ELF, write_conftest, write_project_conftest

pytest_plugins = ["pytester"]

NO_ASYNCIO = ("-p", "no:asyncio")
HEADER = "*- popcorn -*"
CONSOLE = 'console=popcorn.MachineConsole(uart="USART1"), '

FIXTURES = """
@pytest.fixture
def fault_description():
    return FAULT
"""

ECHO = """
import popcorn
from popcorn.units import ms


async def echo(machine, data):
    await machine.periph.USART1.port.write(data)
    await machine.run(for_=ms(20))
"""


@pytest.fixture(scope="module")
def u575_json() -> Path:
    from popcorn.stm32.description import DescriptionError, cached_description

    try:
        return Path(cached_description("STM32U575QGIx"))
    except DescriptionError as error:
        pytest.skip(f"no STM32U575 description: {error}")


def description(u575_json: Path, elf: Path, extra: str, until: str = "ready") -> str:
    return (
        "popcorn.MachineDescription(\n"
        '    part="STM32U575QGIx",\n'
        f"    description=Path({str(u575_json)!r}),\n"
        f"    images=[popcorn.Elf(Path({str(elf)!r}))],\n"
        f'    boot=popcorn.Boot(until="{until}", budget=1_000_000),\n'
        f"    {extra}\n"
        ")"
    )


def project(
    pytester: pytest.Pytester,
    u575_json: Path,
    extra: str = CONSOLE,
    *,
    configs: str = "",
    imports: str = "",
) -> None:
    write_conftest(pytester)
    (pytester.path / "console_echo.py").write_text(f"import popcorn\n{ECHO}")
    base = description(u575_json, BOARD_ELF, extra)
    fault = description(u575_json, FAULT_ELF, CONSOLE)
    expression = f"BASE.with_configs({configs})" if configs else "BASE"
    write_project_conftest(
        pytester.path,
        expression,
        f"{FIXTURES}\n{imports}\n\nBASE = {base}\nFAULT = {fault}\n",
    )


def run(pytester: pytest.Pytester, *args: str) -> pytest.RunResult:
    return pytester.runpytest(*NO_ASYNCIO, "-p", "no:cacheprovider", *args)


def blocks(result: pytest.RunResult) -> dict[str, str]:
    text = result.stdout.str()
    text = text[text.index("- popcorn -") :]
    found: dict[str, list[str]] = {}
    current = ""
    for line in text.splitlines():
        match = re.match(r"^(\S.*?) \((?:config|description)=[^)]*\)( \(raised\))?:$", line)
        if match:
            current = match.group(1) + (match.group(2) or "")
            found[current] = []
        elif current:
            found[current].append(line)
    return {name: "\n".join(lines) for name, lines in found.items()}


def test_a_console_shows_the_unread_output_and_no_console_shows_none(
    pytester: pytest.Pytester, u575_json: Path
) -> None:
    project(pytester, u575_json)
    pytester.makepyfile("""
        from console_echo import *

        async def test_with(machine):
            await echo(machine, b"hello\\r\\n")
            assert False

        async def test_without(machine_factory, description):
            machine = await machine_factory.booted(description.replace(console=None))
            await echo(machine, b"hello\\r\\n")
            assert False
        """)
    result = run(pytester)
    result.assert_outcomes(failed=2)
    text = result.stdout.str()
    assert text.count("console USART1 (unread):") == 1
    assert text.count("  hello") == 1


def test_two_uarts_give_two_blocks_in_order(pytester: pytest.Pytester, u575_json: Path) -> None:
    project(pytester, u575_json, 'console=popcorn.MachineConsole(uart=("USART2", "USART1")),')
    pytester.makepyfile("""
        from console_echo import *

        async def test_two(machine):
            await echo(machine, b"x")
            assert False
        """)
    shown = blocks(run(pytester))["machine"]
    assert shown.index("console USART2") < shown.index("console USART1")


def test_a_duplicate_name_is_refused() -> None:
    with pytest.raises(popcorn.ConfigError):
        popcorn.MachineConsole(uart=("USART1", "USART1"))


def test_the_tail_cuts_to_the_last_bytes(pytester: pytest.Pytester, u575_json: Path) -> None:
    project(pytester, u575_json, 'console=popcorn.MachineConsole(uart="USART1", tail=4),')
    pytester.makepyfile("""
        from console_echo import *

        async def test_cut(machine):
            await echo(machine, b"0123456789")
            assert False

        async def test_fit(machine_factory, description):
            machine = await machine_factory.booted(
                description.replace(console=popcorn.MachineConsole(uart="USART1", tail=10))
            )
            await echo(machine, b"0123456789")
            assert False
        """)
    text = run(pytester).stdout.str()
    assert "console USART1 (unread, last 4 of 10 bytes):" in text
    assert "  6789" in text
    assert text.count("console USART1 (unread):") == 1
    assert "  0123456789" in text


def test_only_the_unread_half_shows_and_a_teardown_still_takes_it(
    pytester: pytest.Pytester, u575_json: Path
) -> None:
    project(pytester, u575_json)
    pytester.makepyfile("""
        from console_echo import *

        from pathlib import Path

        import pytest

        @pytest.fixture(autouse=True)
        def drain(machine):
            yield
            Path("taken.bin").write_bytes(machine.periph.USART1.port.take())

        async def test_half(machine):
            await echo(machine, b"0123456789")
            await machine.periph.USART1.port.read(5, for_=ms(1))
            assert False
        """)
    shown = blocks(run(pytester))["machine"]
    assert "  56789" in shown
    assert "01234" not in shown
    assert (pytester.path / "taken.bin").read_bytes() == b"56789"


def test_control_and_invalid_bytes_are_escaped(pytester: pytest.Pytester, u575_json: Path) -> None:
    project(pytester, u575_json)
    pytester.makepyfile("""
        from console_echo import *

        async def test_raw(machine):
            await echo(machine, b"a\\xffb\\x1b[31m\\r\\n")
            assert False
        """)
    text = run(pytester).stdout.str()
    assert "  a\\xffb\\x1b[31m" in text
    assert "\x1b" not in text
    assert "popcorn report failed" not in text


def test_a_budget_stop_names_no_culprit_so_every_console_shows(
    pytester: pytest.Pytester, u575_json: Path
) -> None:
    project(pytester, u575_json)
    pytester.makepyfile("""
        from console_echo import *

        async def test_budget(machine, machine_factory, description):
            other = await machine_factory.booted(description)
            await echo(machine, b"one")
            await echo(other, b"two")
            stop = await machine.run(budget=500)
            raise popcorn.EmulationError("budget", stop=stop)
        """)
    result = run(pytester)
    shown = blocks(result)
    assert "  one" in shown["machine (raised)"]
    assert "  two" in shown["machine_factory #1"]


def test_a_fault_shows_only_the_console_of_the_machine_that_faulted(
    pytester: pytest.Pytester, u575_json: Path
) -> None:
    project(pytester, u575_json)
    pytester.makepyfile("""
        from console_echo import *

        async def test_fault(machine, machine_factory, fault_description):
            await echo(machine, b"one")
            faulty = await machine_factory.booted(fault_description)
            faulty.nvic.pend(0)
            async with faulty.running():
                await faulty.wait_for(popcorn.Reached("ready"))
        """)
    shown = blocks(run(pytester))
    assert "console USART1" not in shown["machine"]
    assert "  one" not in shown["machine"]
    assert "console USART1 (unread):" in shown["machine_factory #1 (raised)"]


def test_an_assertion_without_a_stop_shows_every_console(
    pytester: pytest.Pytester, u575_json: Path
) -> None:
    project(pytester, u575_json)
    pytester.makepyfile("""
        from console_echo import *

        async def test_plain(machine, machine_factory, description):
            other = await machine_factory.booted(description)
            await echo(machine, b"one")
            await echo(other, b"two")
            assert False
        """)
    shown = blocks(run(pytester))
    assert "  one" in shown["machine"]
    assert "  two" in shown["machine_factory #1"]


def test_every_stop_of_a_group_names_a_culprit(pytester: pytest.Pytester, u575_json: Path) -> None:
    project(pytester, u575_json)
    pytester.makepyfile("""
        from console_echo import *

        async def test_group(machine_factory, description, fault_description):
            first = await machine_factory.booted(fault_description)
            second = await machine_factory.booted(fault_description)
            healthy = await machine_factory.booted(description)
            await echo(healthy, b"healthy")
            errors = []
            await second.run(budget=50)
            for faulty in (first, second):
                faulty.nvic.pend(0)
                try:
                    async with faulty.running():
                        await faulty.wait_for(popcorn.Reached("ready"))
                except popcorn.EmulationError as error:
                    errors.append(error)
            raise ExceptionGroup("both", errors)
        """)
    result = run(pytester)
    shown = blocks(result)
    assert list(shown) == [
        "machine_factory #1 (raised)",
        "machine_factory #2",
        "machine_factory #3",
    ]
    assert "console USART1" in shown["machine_factory #1 (raised)"]
    assert "console USART1" in shown["machine_factory #2"]
    assert "console USART1" not in shown["machine_factory #3"]


def test_an_error_stop_that_is_no_machines_stop_shows_every_console(
    pytester: pytest.Pytester, u575_json: Path
) -> None:
    project(pytester, u575_json)
    pytester.makepyfile("""
        from console_echo import *

        async def test_chained(machine):
            await echo(machine, b"one")
            first = await machine.run(budget=100)
            await machine.run(budget=5000)
            raise popcorn.EmulationError("inner", stop=first)
        """)
    result = run(pytester)
    assert "exception stop: BUDGET" in result.stdout.str()
    assert "  one" in blocks(result)["machine"]


def test_names_culprit_pins_every_reason() -> None:
    culprits = {popcorn.StopReason.FAULT, popcorn.StopReason.CONTRACT}
    for reason in popcorn.StopReason:
        stop = popcorn.Stop(reason, 0x0800_0000, popcorn.Location(0x0800_0000), 1, VirtualTime(1))
        assert names_culprit(stop) is (reason in culprits), reason


DECLARED = """
from popcorn.units import us


@popcorn.pytest.machine
def plain():
    return BASE.replace(console=None)


@popcorn.pytest.machine
def typo(request):
    if request.node.name == "test_typo":
        return BASE.replace(console=popcorn.MachineConsole(uart="NOPE"))
    return BASE


@popcorn.pytest.machine
def short():
    return BASE.replace(boot=popcorn.Boot(until="ready", budget=5))


@popcorn.pytest.system
def duo():
    return popcorn.SystemDescription(
        members={
            "a": BASE.replace(console=popcorn.MachineConsole(uart="USART2")),
            "b": BASE.replace(console=None),
        },
        links=[popcorn.UartLink("a.USART1", "b.USART1", latency=us(20), baud=460_800)],
    )
"""


def test_two_descriptions_of_one_key_each_report_their_own_console(
    pytester: pytest.Pytester, u575_json: Path
) -> None:
    project(pytester, u575_json, imports=DECLARED)
    pytester.makepyfile("""
        from console_echo import *

        async def test_console_first(machine, plain):
            await echo(machine, b"one")
            await echo(plain, b"two")
            assert False

        async def test_console_second(plain, machine_factory, description):
            other = await machine_factory.booted(description)
            await echo(plain, b"one")
            await echo(other, b"two")
            assert False
        """)
    first = run(pytester, "-k", "console_first")
    second = run(pytester, "-k", "console_second")
    one, two = blocks(first), blocks(second)
    assert "  one" in one["machine"] and "console USART1" not in one["plain"]
    assert "console USART1" in blocks(second)["machine_factory #1"]
    assert "console USART1" not in two["plain"]


def test_a_console_typo_fails_the_setup_and_leaves_the_next_test_clean(
    pytester: pytest.Pytester, u575_json: Path
) -> None:
    project(pytester, u575_json, imports=DECLARED)
    pytester.makepyfile("""
        from pathlib import Path

        import popcorn
        import pytest

        @pytest.fixture(autouse=True)
        def spy(request, monkeypatch):
            calls = []
            real = popcorn.Machine.restore

            def restore(self, *args, **kwargs):
                calls.append(1)
                return real(self, *args, **kwargs)

            monkeypatch.setattr(popcorn.Machine, "restore", restore)
            yield
            Path(f"{request.node.name}.restores").write_text(str(len(calls)))

        def test_shared(machine):
            pass

        def test_typo(typo):
            pass

        def test_after(typo):
            assert typo.pc == typo.sym("ready")
            assert typo.board.ptys == {}
        """)
    result = run(pytester)
    result.assert_outcomes(passed=2, errors=1)
    result.stdout.fnmatch_lines(["*ConfigError*NOPE*"])
    assert (pytester.path / "test_typo.restores").read_text() == "0"
    assert int((pytester.path / "test_after.restores").read_text()) >= 1


def test_a_failed_boot_reports_the_console(pytester: pytest.Pytester, u575_json: Path) -> None:
    project(pytester, u575_json, imports=DECLARED)
    pytester.makepyfile("""
        def test_boot(short):
            pass
        """)
    result = run(pytester)
    result.assert_outcomes(errors=1)
    assert "console USART1" in blocks(result)["short (raised)"]


def test_each_member_of_a_system_reports_its_own_console(
    pytester: pytest.Pytester, u575_json: Path
) -> None:
    project(pytester, u575_json, imports=DECLARED)
    pytester.makepyfile("""
        async def test_system(duo):
            assert False
        """)
    shown = blocks(run(pytester))
    assert "console USART2" in shown["duo member a"]
    assert "console USART2" not in shown["duo member b"]


def test_a_config_carries_its_own_console(pytester: pytest.Pytester, u575_json: Path) -> None:
    project(
        pytester,
        u575_json,
        extra="",
        configs='loud=BASE.replace(console=popcorn.MachineConsole(uart="USART1"))',
        imports='@popcorn.pytest.machine(config="loud")\ndef loud():\n    return PROJECT\n',
    )
    pytester.makepyfile("""
        def test_both(machine, loud):
            assert False
        """)
    shown = blocks(run(pytester))
    assert "console USART1" in shown["loud"]
    assert "console USART1" not in shown["machine"]


def test_a_bridged_uart_and_a_failing_read_leave_the_section_intact(
    pytester: pytest.Pytester, u575_json: Path
) -> None:
    project(pytester, u575_json)
    pytester.makepyfile("""
        from popcorn._streams import UartStream

        def test_bridged(machine):
            machine.board.pty("USART1")
            assert False

        def test_failing(machine, monkeypatch):
            def boom(self, count):
                raise ValueError("boom")

            monkeypatch.setattr(UartStream, "_tail", boom)
            assert False
        """)
    text = run(pytester).stdout.str()
    assert "console USART1: not readable: USART1 is bridged to" in text
    assert "console USART1: read failed: ValueError: boom" in text
    assert text.count("registers:") == 2


def test_the_console_read_adds_nothing_to_the_ring(
    pytester: pytest.Pytester, u575_json: Path
) -> None:
    project(pytester, u575_json)
    pytester.makepyfile("""
        from console_echo import *

        async def test_ring(machine_factory, description):
            with_console = await machine_factory.booted(description)
            without = await machine_factory.booted(description.replace(console=None))
            await echo(with_console, b"abc")
            await echo(without, b"abc")
            assert False
        """)
    shown = blocks(run(pytester, "--popcorn-ring=50"))

    def ring(text: str) -> list[str]:
        lines = text.splitlines()
        start = lines.index("  trace ring:") if "  trace ring:" in lines else len(lines)
        return [line for line in lines[start:] if not line.lstrip().startswith("console")]

    first, second = shown["machine_factory #1"], shown["machine_factory #2"]
    assert ring(first) and ring(first)[:5] == ring(second)[:5]
    assert "console USART1" in first and "console USART1" not in second


def test_a_passing_test_reads_no_console(pytester: pytest.Pytester, u575_json: Path) -> None:
    project(pytester, u575_json)
    pytester.makepyfile("""
        from pathlib import Path

        from popcorn._streams import UartStream

        def test_passing(machine, monkeypatch):
            def read(self, count):
                Path("read.txt").write_text("read")
                return b"", 0, 0

            monkeypatch.setattr(UartStream, "_tail", read)
        """)
    run(pytester).assert_outcomes(passed=1)
    assert not (pytester.path / "read.txt").exists()


FACTORY_CONFIGS = (
    'bad=BASE.replace(console=popcorn.MachineConsole(uart="NOPE")), '
    'loud=BASE.replace(console=popcorn.MachineConsole(uart="USART2")), '
    "short=BASE.replace("
    'boot=popcorn.Boot(until="ready", budget=5), '
    'console=popcorn.MachineConsole(uart="USART2"))'
)

FACTORY_SYSTEM = """
@popcorn.pytest.system
def broken():
    return popcorn.SystemDescription(
        members={"a": SHORT, "b": BASE},
        links=[popcorn.UartLink("a.USART1", "b.USART1", latency=us(20), baud=460_800)],
    )
"""


def factory_project(pytester: pytest.Pytester, u575_json: Path) -> None:
    project(
        pytester,
        u575_json,
        extra="",
        configs=FACTORY_CONFIGS,
        imports="from popcorn.units import us\n" + FACTORY_SYSTEM,
    )
    with (pytester.path / "conftest.py").open("a") as handle:
        handle.write(
            "\nSHORT = BASE.replace(boot=popcorn.Boot(until='ready', budget=5),"
            " console=popcorn.MachineConsole(uart='USART2'))\n"
        )


def test_a_console_typo_through_machine_factory_fails_in_both_forms(
    pytester: pytest.Pytester, u575_json: Path
) -> None:
    factory_project(pytester, u575_json)
    pytester.makepyfile("""
        import popcorn
        import pytest

        def test_sync(machine_factory, description):
            machine_factory(description)
            with pytest.raises(popcorn.ConfigError, match="NOPE"):
                machine_factory(description, config="bad")

        async def test_async(machine_factory, description):
            await machine_factory.booted(description)
            with pytest.raises(popcorn.ConfigError, match="NOPE"):
                await machine_factory.booted(description, config="bad")
        """)
    run(pytester).assert_outcomes(passed=2)


def test_a_factory_config_carries_its_own_console(
    pytester: pytest.Pytester, u575_json: Path
) -> None:
    factory_project(pytester, u575_json)
    pytester.makepyfile("""
        def test_sync(machine_factory, description):
            machine_factory(description, config="loud")
            machine_factory(description)
            assert False

        async def test_async(machine_factory, description):
            await machine_factory.booted(description, config="loud")
            await machine_factory.booted(description)
            assert False
        """)
    result = run(pytester)
    result.assert_outcomes(failed=2)
    text = result.stdout.str()
    assert text.count("console USART2 (unread):") == 2


def test_a_failed_factory_boot_reports_the_console_of_its_config(
    pytester: pytest.Pytester, u575_json: Path
) -> None:
    factory_project(pytester, u575_json)
    pytester.makepyfile("""
        import popcorn
        import pytest

        def test_sync(machine_factory, description):
            with pytest.raises(popcorn.PopcornError):
                machine_factory(description, config="short")
            assert False

        async def test_async(machine_factory, description):
            with pytest.raises(popcorn.PopcornError):
                await machine_factory.booted(description, config="short")
            assert False
        """)
    result = run(pytester)
    result.assert_outcomes(failed=2)
    assert result.stdout.str().count("console USART2 (unread):") == 2


def test_a_failed_system_member_boot_reports_the_console(
    pytester: pytest.Pytester, u575_json: Path
) -> None:
    factory_project(pytester, u575_json)
    pytester.makepyfile("""
        async def test_member(broken):
            pass
        """)
    result = run(pytester)
    result.assert_outcomes(errors=1)
    shown = blocks(result)
    boot = [text for label, text in shown.items() if "boot" in label]
    assert boot and "console USART2" in boot[0]
