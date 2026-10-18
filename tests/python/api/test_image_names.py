"""Named and symbols-only images: `image=` on the symbol lookups, `Elf(name=, loaded=)`."""

from __future__ import annotations

import asyncio
from pathlib import Path

import popcorn
import pytest
from popcorn import Image

ROOT = Path(__file__).parents[3]
PREBUILT = ROOT / "tests" / "firmware" / "periph" / "prebuilt"
RCC_ELF = PREBUILT / "rcc_ll.elf"
GPIO_ELF = PREBUILT / "gpio_ll.elf"


def pair(u575: popcorn.PartDescription, load: bool = True) -> popcorn.Machine:
    """gpio_ll then rcc_ll, both defining main and SystemInit at different addresses."""
    return popcorn.Machine(
        u575,
        image=[
            Image.elf(GPIO_ELF, name="app"),
            Image.elf(RCC_ELF, name="extra", load=load),
        ],
    )


def test_without_image_the_last_image_wins_and_image_picks_one(
    u575: popcorn.PartDescription,
) -> None:
    m = pair(u575)
    gpio, rcc = Image.elf(GPIO_ELF).lookup("main"), Image.elf(RCC_ELF).lookup("main")
    assert gpio.value != rcc.value
    assert m.sym("main") == rcc.value
    assert m.sym("main", image="extra") == rcc.value
    assert m.sym("main", image="app") == gpio.value
    assert m.fn_ptr("SystemInit", image="app") == Image.elf(GPIO_ELF).lookup("SystemInit").value | 1


def test_an_unknown_image_name_lists_the_names(u575: popcorn.PartDescription) -> None:
    m = pair(u575)
    with pytest.raises(popcorn.SymbolError, match=r"no image named secure.*app, extra") as caught:
        m.sym("main", image="secure")
    assert caught.value.close_matches == ("app", "extra")
    with pytest.raises(popcorn.SymbolError, match="no image named secure"):
        m.var("log_buf", image="secure")
    with pytest.raises(popcorn.SymbolError, match="no image named secure"):
        m.image.var("log_buf", image="secure")


def test_a_variable_is_looked_up_in_the_named_image(u575: popcorn.PartDescription) -> None:
    m = pair(u575)
    app = Image.elf(GPIO_ELF).lookup("log_buf").value
    extra = Image.elf(RCC_ELF).lookup("log_buf").value
    assert app != extra
    assert m.var("log_buf").address == extra
    assert m.var("log_buf", image="app").address == app
    assert m.image.var("log_buf", image="app").address == app


def test_a_symbols_only_image_writes_nothing_and_still_resolves(
    u575: popcorn.PartDescription,
) -> None:
    """Its bytes never reach memory: flash holds gpio_ll alone, while rcc_ll's symbols and
    DWARF answer the lookups."""
    alone = popcorn.Machine(u575, image=Image.elf(GPIO_ELF))
    m = pair(u575, load=False)
    rcc = Image.elf(RCC_ELF)
    start = min(s.start for s in rcc.segments if s.executable)
    size = 0x400
    assert m.mem.read(start, size) == alone.mem.read(start, size)
    assert pair(u575).mem.read(start, size) != alone.mem.read(start, size)
    assert m.sym("main") == rcc.lookup("main").value
    assert m.var("SystemCoreClock").address == rcc.lookup("SystemCoreClock").value
    assert [i.loads for i in m.images] == [True, False]
    assert m.snapshot().image_digests == alone.snapshot().image_digests


def test_two_images_of_one_name_are_refused_before_the_machine_is_built(
    u575: popcorn.PartDescription,
) -> None:
    images = [Image.elf(GPIO_ELF, name="app"), Image.elf(RCC_ELF, name="app")]
    with pytest.raises(popcorn.ConfigError, match="two images are named app"):
        popcorn.Machine(u575, image=images)
    m = popcorn.Machine(u575, image=images[0])
    with pytest.raises(popcorn.ConfigError, match="two images are named app"):
        m.load(images[1])
    assert [i.name for i in m.images] == ["app"]
    m.load(Image.elf(RCC_ELF, name="other"))
    assert m.sym("main", image="other") == Image.elf(RCC_ELF).lookup("main").value
    with pytest.raises(popcorn.ConfigError, match=r"images\[1\]\.name"):
        popcorn.MachineDescription(
            part="STM32U575QGIx",
            images=[popcorn.Elf(GPIO_ELF, name="app"), popcorn.Elf(RCC_ELF, name="app")],
        )


def test_a_description_carries_the_names_and_the_symbols_only_flag(
    u575: popcorn.PartDescription,
) -> None:
    plain = popcorn.MachineDescription(part="STM32U575QGIx", images=[popcorn.Elf(GPIO_ELF)])
    named = popcorn.MachineDescription(
        part="STM32U575QGIx",
        images=[popcorn.Elf(GPIO_ELF, name="app"), popcorn.Elf(RCC_ELF, loaded=False)],
        boot=popcorn.Boot(until="main", image="app", budget=1_000_000),
    )
    assert len({plain.digest, named.digest, named.replace(boot=popcorn.Boot()).digest}) == 3
    m = named.machine()
    assert [(i.name, i.loads) for i in m.images] == [("app", True), (None, False)]
    stop = asyncio.run(named.boot_machine(m))
    assert stop.pc == Image.elf(GPIO_ELF).lookup("main").value
    wrong = named.replace(boot=popcorn.Boot(until="main", image="nope"))
    with pytest.raises(popcorn.ConfigError, match=r"boot\.image"):
        asyncio.run(wrong.boot_machine(wrong.machine()))


def test_the_new_fields_leave_the_digest_of_a_plain_image_unchanged() -> None:
    from popcorn._machine_description import (
        _canonical,  # pyright: ignore[reportPrivateUsage]
    )

    assert _canonical(popcorn.Elf(GPIO_ELF)) == {"$": "Elf", "path": {"path": GPIO_ELF.as_posix()}}
    assert "image" not in str(_canonical(popcorn.Boot(until="main")))
