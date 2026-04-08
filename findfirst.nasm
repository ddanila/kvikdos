; findfirst.nasm: Test INT 21h/4Eh FindFirst and INT 21h/4Fh FindNext.
;
; Test 1: FindFirst *.* with attrs=0x37 (no volume label) — must find files.
; Test 2: FindFirst *.* with attrs=0x08 (volume label only) — must fail.
; Test 3: FindFirst *.* with attrs=0x3F (all including volume label) — must find files.
; Test 4: FindNext after test 3 — must not crash (may or may not find more).

bits 16
cpu 8086
org 0x100

_start:
	; Set DTA to our buffer.
	mov ah, 0x1a
	mov dx, dta_buf
	int 0x21

	; --- Test 1: FindFirst *.* attrs=0x37 ---
	mov ah, 0x4e
	mov cx, 0x0037
	mov dx, pattern
	int 0x21
	jc .fail1
	cmp byte [dta_buf + 0x1e], 0
	je .fail1

	; --- Test 2: FindFirst *.* attrs=0x08 (volume label only) ---
	mov ah, 0x4e
	mov cx, 0x0008
	mov dx, pattern
	int 0x21
	jnc .fail2		; should fail — no volume label

	; --- Test 3: FindFirst *.* attrs=0x3F (all + volume label) ---
	mov ah, 0x4e
	mov cx, 0x003f
	mov dx, pattern
	int 0x21
	jc .fail3
	cmp byte [dta_buf + 0x1e], 0
	je .fail3

	; --- Test 4: FindNext (should not crash) ---
	mov ah, 0x4f
	int 0x21
	; result doesn't matter

	; All passed.
	mov ah, 9
	mov dx, msg_ok
	int 0x21
	ret

.fail1:	mov dx, msg_f1
	jmp short .die
.fail2:	mov dx, msg_f2
	jmp short .die
.fail3:	mov dx, msg_f3
.die:	mov ah, 9
	int 0x21
	mov ax, 0x4c01
	int 0x21

pattern:	db '*.*', 0
msg_ok:		db 'findfirst OK', 13, 10, '$'
msg_f1:		db 'FAIL: FindFirst attrs=0x37', 13, 10, '$'
msg_f2:		db 'FAIL: volume label found unexpectedly', 13, 10, '$'
msg_f3:		db 'FAIL: FindFirst attrs=0x3F', 13, 10, '$'

dta_buf:	times 128 db 0
