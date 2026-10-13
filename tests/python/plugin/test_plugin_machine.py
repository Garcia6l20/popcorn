"""The session boot, a declared machine fixture and `machine_factory`, FR-PYT-1, FR-PYT-2."""

from __future__ import annotations

import os
import pwd

import pytest

from .helpers import (
    BOARD_ELF,
    CALLEE_ELF,
    DRIVER_ELF,
    write_conftest,
    write_project,
    write_project_conftest,
)

pytest_plugins = ["pytester"]

NO_ASYNCIO = ("-p", "no:asyncio")


def run(pytester: pytest.Pytester, *args: str) -> pytest.RunResult:
    return pytester.runpytest(*NO_ASYNCIO, *args)


def test_one_boot_restored_for_every_test(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile("""
        import pytest

        FIRST = {}

        @pytest.mark.parametrize("n", range(20))
        async def test_restored(machine, n):
            assert machine.pc == machine.sym("alu_mark")
            FIRST.setdefault("instructions", machine.instructions)
            FIRST.setdefault("machine", machine)
            assert machine.instructions == FIRST["instructions"]
            assert machine is FIRST["machine"]
            assert machine.last_stop is None
            await machine.call("alu_mark")
            assert machine.mem.u32[machine.sym("alu_rounds")] == 1
        """)
    result = run(pytester)
    result.assert_outcomes(passed=20)
    result.stdout.fnmatch_lines(["popcorn: 1 key(s), 1 boot(s), 0 from cache"])


def test_hooks_logs_baseline_and_ring_do_not_leak(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile("""
        import popcorn

        async def test_leaves_hooks(machine):
            def boom(call):
                raise RuntimeError("the previous test's intercept")

            await machine.run(budget=100_000)
            assert machine.ring.records()
            machine.intercept("alu_mark", boom)
            machine.mem.u32[machine.sym("spin_flag")] = 7
            machine.set_call_baseline(machine.snapshot())

        async def test_sees_none(machine):
            assert machine.ring.records() == []
            assert machine.intercept_log("alu_mark") == []
            result = await machine.call("alu_mark", reset=True)
            assert result.stop.reason is popcorn.StopReason.RETURNED
            assert machine.mem.u32[machine.sym("spin_flag")] == 0
        """)
    run(pytester).assert_outcomes(passed=2)


def test_a_spoiled_machine_is_rebuilt_from_the_saved_boot(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile(f"""
        import popcorn

        SEEN = []

        def test_loads_another_image(machine):
            SEEN.append(machine)
            machine.load(popcorn.Image.elf({str(CALLEE_ELF)!r}))
            assert len(machine.images) == 2

        def test_gets_a_clean_machine(machine):
            assert machine is not SEEN[0]
            assert len(machine.images) == 1
            assert machine.pc == machine.sym("alu_mark")

        async def test_leaves_a_run_behind(machine):
            import asyncio

            SEEN.append(machine)
            context = machine.running()
            SEEN.append(context)
            await context.__aenter__()
            assert machine.busy

        def test_after_a_busy_machine(machine):
            assert machine is SEEN[1]
            assert not machine.busy
        """)
    result = run(pytester, "-W", "ignore")
    result.assert_outcomes(passed=4)
    result.stdout.fnmatch_lines(["popcorn: 1 key(s), 1 boot(s), 0 from cache"])


def test_machine_factory(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile("""
        def test_booted(machine, machine_factory, description):
            other = machine_factory(description)
            assert other is not machine
            assert other.pc == machine.pc == machine.sym("alu_mark")
            rounds = machine.sym("alu_rounds")
            assert other.mem.u32[rounds] == machine.mem.u32[rounds] == 0
            assert other.instructions == machine.instructions
            second = machine_factory(description)
            other.mem.u32[rounds] = 5
            assert second.mem.u32[rounds] == 0
            assert machine.mem.u32[rounds] == 0

        def test_not_booted(machine_factory, description):
            fresh = machine_factory(description, booted=False)
            assert fresh.instructions == 0
            assert fresh.pc == 0
            assert fresh.pc != fresh.sym("Reset_Handler") & ~1
        """)
    run(pytester).assert_outcomes(passed=2)


def test_factory_names_its_machines_and_boots_once(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile("""
        import pytest

        def test_named(machine, machine_factory, description):
            assert machine.name == "machine"
            named = machine_factory(description, name="x")
            assert named.name == "x"
            assert named.pc == machine.pc
            assert machine_factory(description, name="y", booted=False).name == "y"
            assert machine_factory(description).name == machine.description.part
            with pytest.raises(ValueError):
                machine_factory(description, name="x.y")

        async def test_named_async(machine_factory, description):
            m = await machine_factory.booted(description, name="z")
            assert m.name == "z"
        """)
    result = run(pytester)
    result.assert_outcomes(passed=2)
    result.stdout.fnmatch_lines(["popcorn: 1 key(s), 1 boot(s), 0 from cache"])


def test_the_factory_takes_a_description_first(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile("""
        import pytest

        MISSING = "missing 1 required positional argument: 'description'"
        WRONG = "^machine_factory takes a popcorn.MachineDescription first, not str$"

        def test_missing(machine_factory):
            with pytest.raises(TypeError, match=MISSING):
                machine_factory()

        async def test_missing_async(machine_factory):
            with pytest.raises(TypeError, match=MISSING):
                await machine_factory.booted()

        def test_a_config_name(machine_factory):
            with pytest.raises(TypeError, match=WRONG):
                machine_factory("variant")

        async def test_a_config_name_async(machine_factory):
            with pytest.raises(TypeError, match=WRONG):
                await machine_factory.booted("variant", name="x")

        def test_the_old_keyword(machine_factory, description):
            with pytest.raises(TypeError, match="project"):
                machine_factory(description, project=description)
        """)
    result = run(pytester)
    result.assert_outcomes(passed=5)
    result.stdout.no_fnmatch_line("popcorn: * key(s)*")


def test_factory_boot_from_async_tests(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile("""
        import pytest

        async def test_sync_form_refuses_to_boot(machine_factory, description):
            with pytest.raises(RuntimeError, match="await machine_factory.booted"):
                machine_factory(description)

        async def test_async_form_boots(machine_factory, description):
            m = await machine_factory.booted(description)
            assert m.pc == m.sym("alu_mark")

        async def test_sync_form_after_the_boot(machine_factory, description):
            m = machine_factory(description)
            assert m.pc == m.sym("alu_mark")
        """)
    result = run(pytester)
    result.assert_outcomes(passed=3)
    result.stdout.fnmatch_lines(["popcorn: 1 key(s), 1 boot(s), 0 from cache"])


def test_concurrent_async_boots_of_one_key_boot_once(pytester: pytest.Pytester) -> None:
    write_conftest(
        pytester,
        """
        import asyncio

        import popcorn

        def pytest_configure(config):
            boot = popcorn.MachineDescription.boot_machine

            async def slow_boot(self, machine):
                await asyncio.sleep(0.01)
                return await boot(self, machine)

            popcorn.MachineDescription.boot_machine = slow_boot
        """,
    )
    write_project(pytester)
    pytester.makepyfile("""
        import asyncio

        async def test_two_tasks(machine_factory, description):
            first, second = await asyncio.gather(
                machine_factory.booted(description), machine_factory.booted(description)
            )
            assert first is not second
            assert first.pc == second.pc == first.sym("alu_mark")
        """)
    result = run(pytester)
    result.assert_outcomes(passed=1)
    result.stdout.fnmatch_lines(["popcorn: 1 key(s), 1 boot(s), 0 from cache"])


def test_a_pty_spoils_the_machine(
    pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch
) -> None:
    from popcorn.stm32 import mcu
    from popcorn.stm32.description import DescriptionError

    monkeypatch.setenv("HOME", pwd.getpwuid(os.getuid()).pw_dir)
    try:
        mcu("STM32U575QGIx")
    except DescriptionError as error:
        pytest.skip(f"no STM32U575 description: {error}")
    write_conftest(pytester)
    write_project_conftest(
        pytester.path,
        f'popcorn.MachineDescription(part="STM32U575QGIx", '
        f"images=[popcorn.Elf(Path({str(BOARD_ELF)!r}))], "
        'boot=popcorn.Boot(until="ready", budget=1_000_000))',
    )
    pytester.makepyfile("""
        SEEN = []

        def test_opens_a_pty(machine):
            SEEN.append(machine)
            assert machine.board.pty("USART1")

        def test_gets_a_new_machine(machine):
            assert machine is not SEEN[0]
            assert machine.board.ptys == {}
            assert machine.pc == machine.sym("ready")
        """)
    result = run(pytester)
    result.assert_outcomes(passed=2)
    result.stdout.fnmatch_lines(["popcorn: 1 key(s), 1 boot(s), 0 from cache"])


def test_a_failed_boot_raises_fresh_copies(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    pytester.makepyfile("""
        from popcorn.pytest._session import fresh_copy

        class KeywordOnly(Exception):
            def __init__(self, message, *, code):
                super().__init__(message)
                self.code = code

        def test_keeps_type_message_and_attributes():
            missing = FileNotFoundError(2, "No such file", "x.toml")
            copied = fresh_copy(missing)
            assert type(copied) is FileNotFoundError
            assert copied is not missing
            assert str(copied) == str(missing)
            assert copied.filename == "x.toml"
            keyword = fresh_copy(KeywordOnly("bad", code=3))
            assert type(keyword) is KeywordOnly
            assert (str(keyword), keyword.code) == ("bad", 3)

        def test_does_not_share_notes_or_traceback():
            try:
                raise ValueError("boot")
            except ValueError as error:
                error.add_note("first")
                caught = error
            copied = fresh_copy(caught)
            assert copied.__traceback__ is None
            copied.add_note("second")
            assert caught.__notes__ == ["first"]
            assert copied.__notes__ == ["first", "second"]
        """)
    run(pytester).assert_outcomes(passed=2)


def test_a_failed_boot_errors_every_machine_test_once(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester, vtor="vt_fault")
    pytester.makepyfile("""
        import pytest

        @pytest.mark.parametrize("n", range(3))
        def test_uses_the_machine(machine, n):
            pass

        def test_without_popcorn():
            pass
        """)
    result = run(pytester)
    result.assert_outcomes(passed=1, errors=3)
    result.stdout.fnmatch_lines(["*FaultError: boot did not reach alu_mark*"])
    result.stdout.fnmatch_lines(["popcorn: 1 key(s), 1 boot(s), 0 from cache"])


def test_without_boot_until_the_snapshot_is_the_reset_state(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester, until=None)
    pytester.makepyfile("""
        def test_reset_state(machine):
            assert machine.pc == machine.sym("Reset_Handler") & ~1
            assert machine.instructions == 0
        """)
    run(pytester).assert_outcomes(passed=1)


def test_the_plugin_declares_no_machine_fixture(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    pytester.makepyfile("""
        def test_uses_the_machine(machine):
            pass

        def test_without_popcorn():
            pass
        """)
    result = run(pytester)
    result.assert_outcomes(passed=1, errors=1)
    result.stdout.fnmatch_lines(["*fixture 'machine' not found*"])


@pytest.mark.parametrize(
    ("inifile", "content"),
    [
        ("pyproject.toml", '[tool.pytest.ini_options]\naddopts = "-p no:asyncio"\n'),
        ("pytest.ini", "[pytest]\naddopts = -p no:asyncio\n"),
    ],
)
def test_the_project_of_an_ini_rootdir(
    pytester: pytest.Pytester, inifile: str, content: str
) -> None:
    example = pytester.mkdir("example")
    (example / inifile).write_text(content)
    (example / "conftest.py").write_text('pytest_plugins = ["popcorn.pytest"]\n')
    write_project(pytester, directory=example)
    (example / "test_example.py").write_text(
        "from pathlib import Path\n\nimport popcorn\n\n\n"
        "def test_booted(machine, description, request):\n"
        "    assert request.config.rootpath.name == 'example'\n"
        f"    assert description.images == (popcorn.Elf(Path({str(DRIVER_ELF)!r})),)\n"
        "    assert machine.pc == machine.sym('alu_mark')\n"
    )
    run(pytester, "example").assert_outcomes(passed=1)
