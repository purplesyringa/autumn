#!/bin/sh
objdump -D --disassembler-color=color -m i386:x86-64 -M intel -b binary --adjust-vma=0x400000 --start-address=0x400068 interp-small | less -R
