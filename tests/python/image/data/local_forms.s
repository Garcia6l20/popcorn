    .section .debug_abbrev,"",%progbits
    .uleb128 1
    .uleb128 0x11
    .byte 1
    .uleb128 0x11
    .uleb128 0x01
    .uleb128 0x12
    .uleb128 0x06
    .uleb128 0x73
    .uleb128 0x17
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
    .uleb128 0x0b
    .byte 1
    .uleb128 0x11
    .uleb128 0x29
    .uleb128 0x12
    .uleb128 0x29
    .byte 0, 0
    .uleb128 4
    .uleb128 0x34
    .byte 0
    .uleb128 0x03
    .uleb128 0x08
    .uleb128 0x49
    .uleb128 0x13
    .uleb128 0x02
    .uleb128 0x18
    .byte 0, 0
    .uleb128 5
    .uleb128 0x34
    .byte 0
    .uleb128 0x03
    .uleb128 0x08
    .uleb128 0x49
    .uleb128 0x13
    .uleb128 0x3c
    .uleb128 0x19
    .byte 0, 0
    .uleb128 6
    .uleb128 0x24
    .byte 0
    .uleb128 0x03
    .uleb128 0x08
    .uleb128 0x0b
    .uleb128 0x0b
    .uleb128 0x3e
    .uleb128 0x0b
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
    .4byte addr_table - addr_start
int_die:
    .uleb128 6
    .asciz "int"
    .byte 4
    .byte 5
    .uleb128 2
    .asciz "f"
    .4byte 0x08000000
    .4byte 0x100
    .uleb128 3
    .byte 0
    .byte 1
    .uleb128 4
    .asciz "kept"
    .4byte int_die - cu_start
    .uleb128 1
    .byte 0x50
    .uleb128 5
    .asciz "g"
    .4byte int_die - cu_start
    .byte 0
    .byte 0
    .byte 0
cu_end:

    .section .debug_addr,"",%progbits
addr_start:
    .4byte addr_end - addr_version
addr_version:
    .2byte 5
    .byte 4
    .byte 0
addr_table:
    .4byte 0x08000010
    .4byte 0x08000020
addr_end:
