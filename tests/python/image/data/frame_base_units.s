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
    .byte 0
    .uleb128 0x03
    .uleb128 0x08
    .uleb128 0x11
    .uleb128 0x01
    .uleb128 0x12
    .uleb128 0x06
    .uleb128 0x40
    .uleb128 0x17
    .byte 0, 0
    .byte 0

    .section .debug_info,"",%progbits
    .rept UNITS
    .4byte 2f - 1f
1:
    .2byte 5
    .byte 1
    .byte 4
    .4byte 0
    .uleb128 1
    .4byte 0x08000000
    .4byte 0x100
    .uleb128 2
    .asciz "f"
    .4byte 0x08000000
    .4byte 0x100
    .4byte shared_list - loclists_start
    .byte 0
2:
    .endr

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
