    .text
    .globl _start
_start:
    ldr x0, =0x4f000000
    mov x1, xzr
    mov x2, xzr
    mov x3, xzr
    ldr x30, =0x40200da8
    br x30
    .ltorg
    .section .ifs, "a"
    .incbin "sensorhub.ifs"
    .section .dtb, "a"
    .incbin "/tmp/virt-fixed.dtb"
