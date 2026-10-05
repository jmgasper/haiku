OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)

ENTRY(_start)
SECTIONS
{
	/* Where the Raspberry Pi firmware loads a 64-bit kernel image that has
	   no Linux header. The entry code moves the image here if it was loaded
	   somewhere else. */
	. = 0x80000;

	.text : {
		*(.text.head)
		*(.text)
		*(.text.*)
		*(.gnu.linkonce.t.*)
	}

	. = ALIGN(0x8);
	__ctor_list = .;
	.init_array : { *(.init_array) *(.ctors) }
	__ctor_end = .;

	.rodata : { *(.rodata .rodata.*) }

	. = ALIGN(0x1000);
	__data_start = .;
	.data : {
		*(.data)
		*(.data.*)
		*(.gnu.linkonce.d.*)
	}
	.got : { *(.got.plt) *(.got) }

	/* The firmware places the boot archive right behind the image file
	   ("followkernel"), so the zero-initialized data and the stack have to
	   be part of the file: the marker section at the end keeps them in the
	   binary. */
	. = ALIGN(0x8);
	__bss_start = .;
	.bss : { *(.bss) *(.bss.*) *(COMMON) }
	. = ALIGN(0x8);
	__bss_end = .;

	.bss.stack : { *(.bss.stack) }

	. = ALIGN(0x1000) - 8;
	.image_end : { QUAD(0x6169724f53727069) }
	_image_end = .;
	_end = .;

	/DISCARD/ : { *(.comment .note .eh_frame .dtors .stab .stabstr .debug*) }
}
