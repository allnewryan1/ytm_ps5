/* The stripped ytmusicd ELF, embedded so the app can deploy it with no extra files. */
	.section .rodata
	.balign 16
	.globl ytm_daemon_elf
	.globl ytm_daemon_elf_end
ytm_daemon_elf:
	.incbin "build/ytmusicd.stripped.elf"
ytm_daemon_elf_end:
	.section .note.GNU-stack,"",@progbits
