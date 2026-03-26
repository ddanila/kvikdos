; a20wrap.nasm: verify 20-bit physical address wraparound on 8086 real mode.
;
; The write to ff00:5031 should wrap to physical address 0x04031.

bits 16
cpu 8086
org 0x100

_start:
		xor ax, ax
		mov ds, ax
		mov bx, 0x4031
		xor al, al
		mov [bx], al

		mov ax, 0xff00
		mov es, ax
		mov di, 0x5031
		mov al, 'Z'
		stosb

		xor dx, dx
		mov ds, dx
		cmp [bx], al
		jne strict short .fail

		push cs
		pop ds
		mov dx, ok_msg
		mov ah, 9
		int 0x21
		ret

.fail:
		push cs
		pop ds
		mov dx, fail_msg
		mov ah, 9
		int 0x21
		mov ax, 0x4c01
		int 0x21

ok_msg:		db 'A20 wrap OK', 13, 10, '$'
fail_msg:	db 'A20 wrap FAIL', 13, 10, '$'
