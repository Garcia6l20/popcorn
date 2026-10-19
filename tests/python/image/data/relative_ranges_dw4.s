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
    .4byte 2f - 1f
1:
    .2byte 4
    .4byte 0
    .byte 4
    .uleb128 1
    .4byte 0x08000000
    .uleb128 2
    .asciz "first"
    .4byte shared_ranges - ranges_start
    .byte 0
2:
    .4byte 4f - 3f
3:
    .2byte 4
    .4byte 0
    .byte 4
    .uleb128 1
    .4byte 0x08001000
    .uleb128 2
    .asciz "second"
    .4byte shared_ranges - ranges_start
    .byte 0
4:

    .section .debug_ranges,"",%progbits
ranges_start:
shared_ranges:
    .4byte 0x20
    .4byte 0x30
    .4byte 0x10
    .4byte 0x18
    .4byte 0
    .4byte 0
