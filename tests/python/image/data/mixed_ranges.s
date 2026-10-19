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
    .uleb128 3
    .uleb128 0x11
    .byte 1
    .uleb128 0x11
    .uleb128 0x01
    .uleb128 0x73
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
    .4byte 0
    .uleb128 2
    .asciz "version4"
    .4byte 12
    .byte 0
2:
    .4byte 4f - 3f
3:
    .2byte 5
    .byte 1
    .byte 4
    .4byte 0
    .uleb128 1
    .4byte 0
    .uleb128 2
    .asciz "version5"
    .4byte 12
    .byte 0
4:
    .4byte 6f - 5f
5:
    .2byte 5
    .byte 1
    .byte 4
    .4byte 0
    .uleb128 3
    .4byte 0
    .4byte first_addresses - addr_start
    .uleb128 2
    .asciz "first_base"
    .4byte indexed_list - rnglists_start
    .byte 0
6:
    .4byte 8f - 7f
7:
    .2byte 5
    .byte 1
    .byte 4
    .4byte 0
    .uleb128 3
    .4byte 0
    .4byte second_addresses - addr_start
    .uleb128 2
    .asciz "second_base"
    .4byte indexed_list - rnglists_start
    .byte 0
8:

    .section .debug_ranges,"",%progbits
    .zero 12
    .4byte 0x08000400
    .4byte 0x08000402
    .4byte 0, 0

    .section .debug_rnglists,"",%progbits
rnglists_start:
    .4byte rnglists_end - rnglists_version
rnglists_version:
    .2byte 5
    .byte 4
    .byte 0
    .4byte 0
    .byte 6
    .4byte 0x08000800
    .4byte 0x08000802
    .byte 0
indexed_list:
    .byte 3
    .uleb128 0
    .uleb128 2
    .byte 0
rnglists_end:

    .section .debug_addr,"",%progbits
addr_start:
    .4byte 10f - 9f
9:
    .2byte 5
    .byte 4
    .byte 0
first_addresses:
    .4byte 0x08000c00
10:
    .4byte 12f - 11f
11:
    .2byte 5
    .byte 4
    .byte 0
second_addresses:
    .4byte 0x08000e00
12:
