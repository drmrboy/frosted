 tar ext :3333
 monitor reset
 layout src
 symbol-file
 file kernel.elf
 add-symbol-file ./frosted-mini-userspace-bflt/init.gdb 0x10090 -s .data 0x20008014 -s .bss 0x20008954
 add-symbol-file frosted-mini-userspace-bflt/idling.gdb 0x12314 -s .data 0x20008dec -s .bss 0x20009684
 add-symbol-file frosted-mini-userspace-bflt/fresh.gdb 0x14204 -s .data 0x20009ae8 -s .bss 0x2000a884
 add-symbol-file frosted-mini-userspace-bflt/binutils.gdb 0x18de0 -s .data 0x2000acf0 -s .bss 0x2000c610
 mon reset
 mon halt
 stepi
 focus c