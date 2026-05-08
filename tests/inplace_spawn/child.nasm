;
; child.nasm: minimal child for the in-place-spawn repro.
;
; Reads the parent PSP segment from PSP[0x16] (DOS records it there
; on AH=4B AL=00 spawn). The parent stamps a sentinel "KVKDPRT1" in
; its image (right after the 3-byte `jmp short begin; nop` prologue,
; so at segment offset 0x103). If the parent's memory survived the
; spawn intact, we read it back and exit AL=0. Otherwise (kvikdos's
; pre-fix reset_emu+reload behaviour) we read zeros and exit AL=0xFF.
;
; This mirrors what Volkov Commander 4.99.09 does in VC.OVL — its
; first action is to look back at parent's load segment for the
; "RESIDENT" signature. See PLAN_INPLACE_SPAWN.md.

bits 16
cpu 8086
org 0100h

_start:
        push    cs
        pop     ds              ; DS = CS = our PSP segment

        ; Read parent PSP segment from our PSP[0x16].
        mov     ax, [0x16]
        cmp     ax, 0
        je      .no_parent

        ; Set ES to parent PSP. Read 8 bytes at parent_psp:0103h
        ; (first byte after the parent's `jmp short begin; nop`
        ; prologue, where the sentinel actually starts) and compare.
        mov     es, ax
        mov     si, sentinel_expected
        mov     di, 0x103
        mov     cx, 8
        cld
.cmp_loop:
        lodsb
        cmp     al, [es:di]
        jne     .mismatch
        inc     di
        loop    .cmp_loop

        ; Match.
        mov     dx, msg_match
        mov     ah, 9
        int     21h
        mov     ax, 4C00h
        int     21h

.mismatch:
        mov     dx, msg_mismatch
        mov     ah, 9
        int     21h
        mov     ax, 4Cffh
        int     21h

.no_parent:
        mov     dx, msg_no_parent
        mov     ah, 9
        int     21h
        mov     ax, 4Cffh
        int     21h

sentinel_expected:
        db      'KVKDPRT1'

msg_match:      db 'child: parent sentinel found',13,10,'$'
msg_mismatch:   db 'child: parent sentinel MISSING (memory wiped?)',13,10,'$'
msg_no_parent:  db 'child: PSP[0x16] is zero (no parent recorded)',13,10,'$'
