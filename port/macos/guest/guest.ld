/*
GUEST.LD

Layout of the macOS guest image (tools/macos_build.py): linked to run at
guest address 0xA0000000, just above the 512 MB Xbox window
(port/linux/src/platform.h), with the image header first. The host maps the
code and read-only data, which the first segment holds, as executable
memory (MAP_JIT), and the writable data on pages of their own
(port/macos/host/host_loader.c).
*/

ENTRY(__guest_header)

SECTIONS
{
	. = 0xA0000000;
	.guest_header : { KEEP(*(.guest_header)) }
	.text : { *(.text .text.*) }
	.rodata : { *(.rodata .rodata.*) }
	. = ALIGN(0x10000);
	.data : { *(.data .data.*) }
	.guest_init : {
		__guest_init_start = .;
		KEEP(*(.guest_init))
		__guest_init_end = .;
	}
	.bss : { *(.bss .bss.*) *(COMMON) }
	. = ALIGN(0x10000);
	__guest_image_end = .;
	/DISCARD/ : { *(.comment) *(.note.*) *(.guest_fini) *(.eh_frame) }
}
