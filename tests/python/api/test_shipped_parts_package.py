"""Every shipped part reads the manual's package code from its package data register.

A package with no code in the manual leaves the register erased, 0x1F on the U5 and 0x3F on the
H5. That is the description's rule for the 10 shipped parts it applies to, not a manual value."""

import struct

import popcorn
import pytest
from popcorn.stm32 import shipped_parts

PACKAGE_OF_NAME = {
    ("A", "I"): "UFBGA169",
    ("C", "T"): "LQFP48",
    ("C", "U"): "UFQFPN48",
    ("I", "K"): "UFBGA176",
    ("I", "T"): "LQFP176",
    ("L", "H"): "TFBGA225",
    ("M", "Y"): "WLCSP80",
    ("O", "Y"): "WLCSP90",
    ("Q", "I"): "UFBGA132",
    ("R", "T"): "LQFP64",
    ("R", "V"): "VFQFPN68",
    ("V", "T"): "LQFP100",
    ("Z", "T"): "LQFP144",
}

U5_CODES = {
    "LQFP64": 0b00000,
    "WLCSP72 SMPS": 0b00001,
    "LQFP100": 0b00010,
    "UFBGA132": 0b00011,
    "LQFP144": 0b00100,
    "LQFP48": 0b00101,
    "UFBGA169": 0b00111,
    "TFBGA169": 0b00111,
    "LQFP64 SMPS": 0b01000,
    "WLCSP90 SMPS": 0b01001,
    "LQFP100 SMPS": 0b01010,
    "UFBGA132 SMPS": 0b01011,
    "LQFP144 SMPS": 0b01100,
    "LQFP48 SMPS": 0b01101,
    "UFBGA169 SMPS": 0b01111,
    "TFBGA169 SMPS": 0b01111,
    "UFBGA64": 0b10010,
    "UFBGA100": 0b10011,
    "LQFP100 DSI SMPS": 0b10100,
    "LQFP144 DSI SMPS": 0b10101,
    "UFBGA144 DSI SMPS": 0b11001,
    "WLCSP208 DSI SMPS": 0b11011,
    "TFBGA216 DSI SMPS": 0b11100,
    "UFBGA100 SMPS": 0b11101,
    "WLCSP56 SMPS": 0b11110,
    "WLCSP150 SMPS": 0b11111,
    "WLCSP150 DSI SMPS": 0b11111,
}
U5_ERASED = 0x1F

H5_CODES = {
    "LQFP64": 0b000000,
    "WLCSP63": 0b000000,
    "VFQFPN68": 0b000001,
    "LQFP100": 0b000010,
    "UFBGA176": 0b000011,
    "LQFP144": 0b000100,
    "LQFP48": 0b000101,
    "UFBGA169": 0b000110,
    "LQFP176": 0b000111,
    "LQFP100 SMPS": 0b001010,
    "UFBGA176 SMPS": 0b001011,
    "LQFP144 SMPS": 0b001100,
    "LQFP176 SMPS": 0b001101,
    "UFBGA169 SMPS": 0b001110,
    "UFQFPN48": 0b010000,
    "WLCSP39": 0b010001,
    "LQFP80": 0b010010,
    "UFBGA100": 0b010100,
    "UFBGA144": 0b010101,
    "LQFP144-EP": 0b010111,
    "LQFP100-EP": 0b011110,
}
H5_ERASED = 0x3F


def _expected(refname: str) -> int:
    package = PACKAGE_OF_NAME[refname[9], refname[11]]
    if refname.endswith("Q"):
        package += " SMPS"
    if refname.startswith("STM32U5"):
        return U5_CODES.get(package, U5_ERASED)
    return H5_CODES.get(package, H5_ERASED)


def _package_field(machine: popcorn.Machine, refname: str) -> int:
    if refname.startswith("STM32U5"):
        (word,) = struct.unpack("<H", machine.mem.read(0x0BFA0500, 2))
        return word & 0x1F
    (word,) = struct.unpack("<H", machine.mem.read(0x08FFF80E, 2))
    return word & 0x3F


@pytest.mark.parametrize("part", shipped_parts())
def test_the_package_register_holds_the_manual_code(part: str) -> None:
    refname = part.split("-")[0]
    machine = popcorn.MachineDescription(part=part, images=[]).machine()
    assert _package_field(machine, refname) == _expected(refname)
