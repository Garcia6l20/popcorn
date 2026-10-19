    .section .debug_abbrev,"",%progbits
    .uleb128 1
    .uleb128 0x11
    .byte 1
    .uleb128 0x11
    .uleb128 0x01
    .uleb128 0x12
    .uleb128 0x06
    .byte 0, 0
    .uleb128 2
    .uleb128 0x2e
    .byte 1
    .uleb128 0x03
    .uleb128 0x08
    .uleb128 0x11
    .uleb128 0x01
    .uleb128 0x12
    .uleb128 0x06
    .byte 0, 0
    .uleb128 3
    .uleb128 0x34
    .byte 0
    .uleb128 0x03
    .uleb128 0x08
    .uleb128 0x49
    .uleb128 0x13
    .uleb128 0x02
    .uleb128 0x17
    .byte 0, 0
    .uleb128 4
    .uleb128 0x24
    .byte 0
    .uleb128 0x03
    .uleb128 0x08
    .uleb128 0x0b
    .uleb128 0x0b
    .uleb128 0x3e
    .uleb128 0x0b
    .byte 0, 0
    .uleb128 5
    .uleb128 0x0b
    .byte 1
    .uleb128 0x55
    .uleb128 0x17
    .byte 0, 0
    .byte 0

    .section .debug_info,"",%progbits
cu_start:
    .4byte cu_end - cu_version
cu_version:
    .2byte 5
    .byte 1
    .byte 4
    .4byte 0
    .uleb128 1
    .4byte 0x08000000
    .4byte 0x100
int_die:
    .uleb128 4
    .asciz "int"
    .byte 4
    .byte 5
    .uleb128 2
    .asciz "f"
    .4byte 0x08000000
    .4byte 0x100
    .rept LOCALS
    .uleb128 3
    .asciz "v"
    .4byte int_die - cu_start
    .4byte shared_list - loclists_start
    .endr
    .rept BLOCKS
    .uleb128 5
    .4byte shared_ranges - rnglists_start
    .uleb128 3
    .asciz "w"
    .4byte int_die - cu_start
    .4byte shared_list - loclists_start
    .byte 0
    .endr
    .byte 0
    .byte 0
cu_end:

    .section .debug_loclists,"",%progbits
loclists_start:
    .4byte loclists_end - loclists_version
loclists_version:
    .2byte 5
    .byte 4
    .byte 0
    .4byte 0
shared_list:
    .rept ENTRIES
    .byte 7
    .4byte 0x08000000
    .4byte 0x08000002
    .uleb128 1
    .byte 0x50
    .endr
    .byte 0
loclists_end:

    .section .debug_rnglists,"",%progbits
rnglists_start:
    .4byte rnglists_end - rnglists_version
rnglists_version:
    .2byte 5
    .byte 4
    .byte 0
    .4byte 0
shared_ranges:
    .rept RANGES
    .byte 6
    .4byte 0x08000000
    .4byte 0x08000002
    .endr
    .byte 0
rnglists_end:
