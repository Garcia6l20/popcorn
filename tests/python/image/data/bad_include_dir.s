    .section .debug_abbrev,"",%progbits
    .uleb128 1
    .uleb128 0x11
    .byte 1
    .uleb128 0x10
    .uleb128 0x17
    .uleb128 0x03
    .uleb128 0x08
    .uleb128 0x1b
    .uleb128 0x08
    .byte 0, 0
    .uleb128 2
    .uleb128 0x34
    .byte 0
    .uleb128 0x03
    .uleb128 0x08
    .uleb128 0x3a
    .uleb128 0x0b
    .uleb128 0x3b
    .uleb128 0x0b
    .uleb128 0x49
    .uleb128 0x13
    .uleb128 0x02
    .uleb128 0x18
    .byte 0, 0
    .uleb128 3
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
    .2byte 4
    .4byte 0
    .byte 4
    .uleb128 1
    .4byte 0
    .asciz "a.c"
    .asciz "/w"
int_die:
    .uleb128 3
    .asciz "int"
    .byte 4
    .byte 5
    .uleb128 2
    .asciz "bad"
    .byte 1
    .byte 3
    .4byte int_die - cu_start
    .uleb128 5
    .byte 0x03
    .4byte 0x20000000
    .uleb128 2
    .asciz "good"
    .byte 2
    .byte 4
    .4byte int_die - cu_start
    .uleb128 5
    .byte 0x03
    .4byte 0x20000004
    .byte 0
cu_end:

    .section .debug_line,"",%progbits
    .4byte line_end - line_version
line_version:
    .2byte 4
    .4byte line_program - line_header
line_header:
    .byte 1
    .byte 1
    .byte 1
    .byte 0xfb
    .byte 14
    .byte 13
    .byte 0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1
    .asciz "inc"
    .byte 0
    .asciz "bad.c"
    .uleb128 5
    .uleb128 0
    .uleb128 0
    .asciz "good.c"
    .uleb128 1
    .uleb128 0
    .uleb128 0
    .byte 0
line_program:
line_end:
