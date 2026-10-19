    .section .debug_abbrev,"",%progbits
    .uleb128 1
    .uleb128 0x11
    .byte 1
    .uleb128 0x11
    .uleb128 0x01
    .byte 0, 0
    .uleb128 2
    .uleb128 0x2e
    .byte 0
    .uleb128 0x03
    .uleb128 0x08
    .uleb128 0x55
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
    .4byte 0
    .rept FUNCTIONS
    .uleb128 2
    .asciz "f"
    .4byte shared_ranges - rnglists_start
    .endr
    .byte 0
2:
    .endr

    .section .debug_rnglists,"",%progbits
rnglists_start:
    .4byte rnglists_end - rnglists_version
rnglists_version:
    .2byte 5
    .byte 4
    .byte 0
    .4byte 0
shared_ranges:
    .byte 6
    .4byte 0x08000040
    .4byte 0x08000042
    .rept ENTRIES
    .byte 6
    .4byte 0x08000100
    .4byte 0x08000102
    .endr
    .byte 0
rnglists_end:
