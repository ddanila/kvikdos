; vidtest.nasm: smoke test for B800:0000 video buffer and INT 10h AH=00/06.
;
; 1. Set video mode 3 (INT 10h AH=00, AL=03) — clears buffer.
; 2. Write "Hi" directly to B800:0000 (char+attr pairs).
; 3. Read back from B800:0000 and verify.
; 4. Scroll up 1 line (INT 10h AH=06) — "Hi" should move from row 0 to gone.
; 5. Read back row 0 col 0 — should be space (cleared by scroll).
; 6. Print result via INT 21h.

org 0x100

start:
    ; Step 1: set video mode 3.
    mov ax, 0x0003
    int 0x10

    ; Step 2: write "Hi" to B800:0000.
    mov ax, 0xB800
    mov es, ax
    mov byte [es:0], 'H'       ; char at row 0, col 0
    mov byte [es:1], 0x07      ; attr
    mov byte [es:2], 'i'       ; char at row 0, col 1
    mov byte [es:3], 0x07      ; attr

    ; Step 3: read back and verify.
    cmp byte [es:0], 'H'
    jne .fail
    cmp byte [es:2], 'i'
    jne .fail

    ; Step 4: scroll up 1 line, full screen.
    mov ax, 0x0601      ; AH=06 (scroll up), AL=01 (1 line)
    mov bh, 0x07        ; fill attribute
    mov cx, 0x0000      ; upper-left: row=0, col=0
    mov dx, 0x184F      ; lower-right: row=24, col=79
    int 0x10

    ; Step 5: row 0 col 0 should now be space (from scroll fill).
    cmp byte [es:0], ' '
    jne .fail

    ; Success: print message and exit 0.
    mov dx, msg_ok
    mov ah, 0x09
    int 0x21
    mov ax, 0x4C00
    int 0x21

.fail:
    mov dx, msg_fail
    mov ah, 0x09
    int 0x21
    mov ax, 0x4C01
    int 0x21

msg_ok:   db 'vidtest OK', 13, 10, '$'
msg_fail: db 'vidtest FAIL', 13, 10, '$'
