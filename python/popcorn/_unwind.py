"""Call stacks of an Armv8-M core without CFI: prologues, exception frames, a stack scan."""

from __future__ import annotations

import bisect
from collections.abc import Callable, Iterable
from dataclasses import dataclass
from enum import Enum

type Read32 = Callable[[int], int | None]
"""Reads a guest word, None when the address is not readable."""

MAX_FRAMES = 64
SCAN_BYTES = 1024
_EXC_RETURN = 0xFF00_0000
_BASIC_FRAME = 32
_EXTENDED_FRAME = 104


class Via(Enum):
    """How a stack frame was found."""

    PC = "pc"
    """The stopped pc."""
    PROLOGUE = "prologue"
    """The return address a decoded prologue saved, or lr for a function that saved none."""
    EXCEPTION = "exception"
    """The pc stacked by an exception entry."""
    SCAN = "scan"
    """A word of the stack that looks like a return address after a call."""

    @property
    def exact(self) -> bool:
        """Whether the frame is certain, not a guess from the stack contents."""
        return self is not Via.SCAN


@dataclass(frozen=True, slots=True)
class RawFrame:
    """One frame: the address running or returned to, and how it was found."""

    pc: int
    via: Via


@dataclass(frozen=True, slots=True)
class Function:
    """Code range of a function symbol."""

    name: str
    start: int
    end: int


class Functions:
    """Function symbols by address. A symbol of size 0 reaches the next function."""

    __slots__ = ("_functions", "_starts")

    def __init__(self, symbols: Iterable[tuple[str, int, int]]) -> None:
        """Index `(name, value, size)` function symbols, Thumb bit ignored."""
        ordered = sorted({(value & ~1, size, name) for name, value, size in symbols})
        functions: list[Function] = []
        for index, (start, size, name) in enumerate(ordered):
            following = ordered[index + 1][0] if index + 1 < len(ordered) else start
            end = start + size if size else max(following, start + 2)
            functions.append(Function(name, start, end))
        self._functions = functions
        self._starts = [f.start for f in functions]

    def at(self, address: int) -> Function | None:
        """The function holding `address`, None when none does."""
        index = bisect.bisect_right(self._starts, address) - 1
        if index < 0:
            return None
        found = self._functions[index]
        return found if found.start <= address < found.end else None


@dataclass(frozen=True, slots=True)
class Prologue:
    """What a function's prologue did to the stack, as far as pc reached into it."""

    size: int
    lr_offset: int | None


def _read16(read32: Read32, address: int) -> int | None:
    word = read32(address & ~3)
    if word is None:
        return None
    return (word >> (16 * ((address >> 1) & 1))) & 0xFFFF


def _thumb_expand(imm12: int) -> int:
    if imm12 >> 10 == 0:
        byte = imm12 & 0xFF
        match (imm12 >> 8) & 3:
            case 0:
                return byte
            case 1:
                return byte << 16 | byte
            case 2:
                return byte << 24 | byte << 8
            case _:
                return byte << 24 | byte << 16 | byte << 8 | byte
    value = 0x80 | (imm12 & 0x7F)
    rotation = imm12 >> 7
    return ((value >> rotation) | (value << (32 - rotation))) & 0xFFFF_FFFF


def prologue(read32: Read32, function: Function, pc: int) -> Prologue | None:
    """Decode the stack effect of the instructions from `function.start` up to `pc`.

    Understands PUSH, STMDB SP!, STR LR [SP, #-4]!, SUB SP #imm, SUB.W and SUBW SP, and VPUSH,
    the forms compilers emit in prologues. Other instructions are stepped over.

    Returns:
        The stack effect, None when the code cannot be read.
    """
    depth = 0
    lr_depth: int | None = None
    address = function.start
    while address < pc:
        first = _read16(read32, address)
        if first is None:
            return None
        if first >> 11 in (0b11101, 0b11110, 0b11111):
            second = _read16(read32, address + 2)
            if second is None:
                return None
            if first == 0xE92D:
                count = second.bit_count()
                if second >> 14 & 1:
                    lr_depth = depth + 4
                depth += 4 * count
            elif first == 0xF84D and second == 0xED04:
                lr_depth = depth + 4
                depth += 4
            elif first & 0xFBEF == 0xF1AD and second >> 8 & 0xF == 0xD:
                imm12 = (first >> 10 & 1) << 11 | (second >> 12 & 7) << 8 | (second & 0xFF)
                depth += _thumb_expand(imm12)
            elif first & 0xFBFF == 0xF2AD and second >> 8 & 0xF == 0xD:
                depth += (first >> 10 & 1) << 11 | (second >> 12 & 7) << 8 | (second & 0xFF)
            elif first & 0xFFBF == 0xED2D and second >> 9 & 7 == 0b101:
                depth += 4 * (second & 0xFF)
            address += 4
            continue
        if first & 0xFE00 == 0xB400:
            count = (first & 0xFF).bit_count() + (first >> 8 & 1)
            if first >> 8 & 1:
                lr_depth = depth + 4
            depth += 4 * count
        elif first & 0xFF80 == 0xB080:
            depth += 4 * (first & 0x7F)
        address += 2
    return Prologue(depth, None if lr_depth is None else depth - lr_depth)


def follows_call(read32: Read32, address: int) -> bool:
    """Whether the Thumb return address `address` follows a BL or a BLX."""
    target = address & ~1
    before = _read16(read32, target - 2)
    if before is not None and before & 0xFF87 == 0x4780:
        return True
    high = _read16(read32, target - 4)
    low = _read16(read32, target - 2)
    return high is not None and low is not None and high >> 11 == 0b11110 and low >> 14 == 0b11


def _is_exc_return(value: int) -> bool:
    """Armv8-M EXC_RETURN: prefix 0xFF, bit 1 reserved 0. 0xFFFFFFFF is lr out of reset."""
    return value & _EXC_RETURN == _EXC_RETURN and not value & 0b10


def _foreign_frame(value: int) -> bool:
    """Whether an EXC_RETURN names a frame this walk does not follow: one on the Non-secure
    stack (S = 0) or with additional state context (DCRS = 0). ES has no effect: popcorn runs
    the core in the Secure state and stacks S, DCRS and ES set."""
    return not value >> 5 & 1 or not value >> 6 & 1


@dataclass(frozen=True, slots=True)
class Epilogue:
    """How the rest of an epilogue returns from pc.

    Attributes:
        size: Bytes it pops before the return address, or in all when it returns through lr.
        lr_offset: Offset of the return address from sp, None when it returns through the live
            lr.
    """

    size: int
    lr_offset: int | None


def epilogue(read32: Read32, pc: int, limit: int = 8) -> Epilogue | bool | None:
    """Simulate forward from `pc` when it stands on an epilogue.

    Understands ADD SP #imm, ADD.W and ADDW SP, VPOP, POP without pc, POP.W and LDR LR [SP],
    #4, then POP {.., pc}, POP.W {.., pc}, LDR PC [SP], #4 or BX LR. A BX LR after LR was
    popped returns through the popped slot, not the live lr.

    Returns:
        False when pc is not on an epilogue form, the epilogue's effect when it returns within
        `limit` instructions, None when it starts like one but cannot be followed.
    """
    depth = 0
    lr_slot: int | None = None
    address = pc
    for step in range(limit):
        first = _read16(read32, address)
        if first is None:
            return None if step else False
        wide = first >> 11 in (0b11101, 0b11110, 0b11111)
        second = _read16(read32, address + 2) if wide else 0
        if second is None:
            return None if step else False
        if first == 0x4770:
            return Epilogue(depth, lr_slot)
        if first & 0xFE00 == 0xBC00:
            count = (first & 0xFF).bit_count()
            if first >> 8 & 1:
                return Epilogue(depth + 4 * (count + 1), depth + 4 * count)
            depth += 4 * count
        elif first == 0xE8BD and second >> 13 & 1 == 0:
            count = (second & 0x7FFF).bit_count()
            if second >> 15 & 1:
                return Epilogue(depth + 4 * (count + 1), depth + 4 * count)
            if second >> 14 & 1:
                lr_slot = depth + 4 * (second & 0x3FFF).bit_count()
            depth += 4 * count
        elif first == 0xF85D and second == 0xFB04:
            return Epilogue(depth + 4, depth)
        elif first == 0xF85D and second == 0xEB04:
            lr_slot = depth
            depth += 4
        elif first & 0xFF80 == 0xB000:
            depth += 4 * (first & 0x7F)
        elif first & 0xFBEF == 0xF10D and second >> 8 & 0xF == 0xD:
            depth += _thumb_expand(
                (first >> 10 & 1) << 11 | (second >> 12 & 7) << 8 | (second & 0xFF)
            )
        elif first & 0xFBFF == 0xF20D and second >> 8 & 0xF == 0xD:
            depth += (first >> 10 & 1) << 11 | (second >> 12 & 7) << 8 | (second & 0xFF)
        elif first & 0xFFBF == 0xECBD and second >> 9 & 7 == 0b101:
            depth += 4 * (second & 0xFF)
        else:
            return None if step else False
        address += 4 if wide else 2
    return None


def _caller(address: int) -> int:
    """Address inside the call instruction a Thumb return address follows."""
    return (address & ~1) - 2


def scan(read32: Read32, functions: Functions, sp: int, limit: int = SCAN_BYTES) -> list[RawFrame]:
    """Words of the stack from `sp` that are Thumb return addresses after a call inside a
    function. The call, not the return address, lies in the function: a call to a noreturn
    function can end its caller.
    """
    frames: list[RawFrame] = []
    for address in range(sp & ~3, (sp & ~3) + limit, 4):
        word = read32(address)
        if word is None:
            break
        if word & 1 and functions.at(_caller(word)) is not None and follows_call(read32, word):
            frames.append(RawFrame(word & ~1, Via.SCAN))
    return frames


def unwind(
    read32: Read32,
    functions: Functions,
    pc: int,
    lr: int,
    sp: int,
    psp: int,
) -> list[RawFrame]:
    """The call stack from a stopped core, innermost first.

    Follows an epilogue forward when pc stands on one, else decodes the prologue up to pc, and
    goes through exception frames recognized by an EXC_RETURN return value, until a return
    address leaves every function. When an epilogue or prologue cannot be followed, a return
    address does not follow a call, or an exception frame is on the Non-secure stack or has
    additional state context, the rest is a stack scan, marked SCAN.
    """
    frames = [RawFrame(pc, Via.PC)]
    lr_valid = True
    returned = False

    def rest() -> list[RawFrame]:
        return frames + scan(read32, functions, sp)[: MAX_FRAMES - len(frames)]

    while len(frames) < MAX_FRAMES:
        function = functions.at(_caller(pc + 1) if returned else pc)
        if function is None:
            break
        ending = epilogue(read32, pc)
        if ending is None:
            return rest()
        if isinstance(ending, Epilogue):
            size, lr_offset = ending.size, ending.lr_offset
        else:
            effect = prologue(read32, function, pc)
            if effect is None:
                return rest()
            size, lr_offset = effect.size, effect.lr_offset
        if lr_offset is not None:
            saved = read32(sp + lr_offset)
            if saved is None:
                break
            ret = saved
        elif lr_valid:
            ret = lr
        else:
            return rest()
        sp += size
        if _is_exc_return(ret):
            if _foreign_frame(ret):
                return rest()
            base = psp if ret & 0b100 else sp
            stacked = [read32(base + 4 * i) for i in range(8)]
            if any(word is None for word in stacked):
                break
            words = [w or 0 for w in stacked]
            frame = _BASIC_FRAME if ret & 0b1_0000 else _EXTENDED_FRAME
            sp = base + frame + (4 if words[7] >> 9 & 1 else 0)
            pc, lr, lr_valid, returned = words[6] & ~1, words[5], True, False
            frames.append(RawFrame(pc, Via.EXCEPTION))
            continue
        if not ret & 1 or functions.at(_caller(ret)) is None:
            break
        if not follows_call(read32, ret):
            return rest()
        pc, lr_valid, returned = ret & ~1, False, True
        frames.append(RawFrame(pc, Via.PROLOGUE))
    return frames


def source_address(read32: Read32, frame: RawFrame) -> int:
    """The address whose source line a frame shows: the call for a return address, the SVC
    for an exception frame that returns after one, the frame's pc otherwise."""
    if frame.via in (Via.PROLOGUE, Via.SCAN):
        return frame.pc - 2
    if frame.via is Via.EXCEPTION:
        before = _read16(read32, frame.pc - 2)
        if before is not None and before & 0xFF00 == 0xDF00:
            return frame.pc - 2
    return frame.pc
