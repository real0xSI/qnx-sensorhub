ENTRY(_start)
SECTIONS {
  . = 0x40200000;
  .ifs : { *(.ifs) }
  . = 0x40500000;
  .text : { *(.text) }
  . = 0x4f000000;
  .dtb : { *(.dtb) }
}
