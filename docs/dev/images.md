# Images

An image is the firmware a machine runs: segments to load, symbols, and the debug information
that turns addresses into source positions. The type is `image` in
`native/include/popcorn/image/image.hpp`. It is plain data built by the loaders in
`native/src/image`.

## What an image holds

- Segments in address order. An ELF segment is a `PT_LOAD` segment, a bin or HEX file gives
  blocks. A segment has a size in memory at least its byte count, and the rest is zero-filled.
- Symbols in address order, with the Thumb bit cleared for a function.
- The source file table and the line table, ordered by address. They are empty without debug
  information.
- The functions of the debug information, each with its code range and declaration line.
- The entry point from the file header, the floating-point calling convention and a SHA-256 of
  the loadable content, which a saved snapshot checks.

The machine checks the segments against the description's regions when it loads the image.

## Loading

`load_elf.cpp` reads an ELF and `load_raw.cpp` reads bin and HEX files. The floating-point
convention is soft unless the ELF header flags set `EF_ARM_ABI_FLOAT_HARD`. Then it is
`hard_double`, or soft again when `.ARM.attributes` sets `Tag_ABI_VFP_args` to the base
procedure call standard. The code is in `arm_attributes.cpp`.

## DWARF

popcorn reads DWARF itself:

- `dwarf_reader` is a bounded cursor over a section. Any read out of range throws
  `image_error`.
- `dwarf_units` finds the compile units and the attributes the line decoder and the function
  scan need.
- `dwarf_lines` decodes the line program of every compile unit.
- `dwarf_macros` runs the macro program of every compile unit that has one. A define replaces
  the name's definition and an undef removes it. The program has limits on nesting and on the
  number of operations.

Python gets types and variables through `Machine.var` and `Machine.types`, and `Constants`
exposes macros.

## Images in a machine

`MachineDescription.images` lists the images in load order. A symbols-only image has
`loaded=False`: its sections are never written to memory, but its symbols and debug
information feed lookups, the unwinder, coverage and trace. Symbol clashes between images
resolve by image order.
