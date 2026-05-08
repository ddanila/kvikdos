;
; parent.nasm: resident-stub parent that spawns child.com via INT 21h
; AH=4Bh AL=00. Mirrors the architecture used by Volkov Commander
; 4.99.09 (a 9 KB resident VC.COM that swap-spawns the 117 KB VC.OVL).
;
; A real-DOS implementation of AH=4B AL=00 must keep the parent's
; memory in-place while the child runs above it. The child sees the
; parent at its original load segment via PSP[0x16] (parent PSP).
;
; Sentinel "KVKDPRT1" lives right after the parent's 3-byte
; `jmp short begin; nop` prologue, at segment offset 0x103. The child
; reads it back as parent_psp:0103h.
;
; Currently fails under kvikdos: kvikdos snapshots and wipes parent
; memory on spawn, so the sentinel reads as zeros and the child exits
; with AL=0xFF. See PLAN_INPLACE_SPAWN.md.

bits 16
cpu 8086
org 0100h

_start:
        jmp     short begin
        nop
        ; The sentinel below is the very first thing in the loaded image.
        ; Child verifies parent_psp:0100h reads "KVKDPRT1" — proves
        ; parent memory survived the spawn.
sentinel:
        db      'KVKDPRT1'

begin:
        push    cs
        pop     ds              ; DS = CS = parent PSP

        ; Shrink our DOS allocation to ~4 KB so there's room for child
        ; in the freed paragraphs above us.
        mov     ah, 4Ah
        mov     bx, 256         ; 256 paragraphs = 4 KB resident
        int     21h
        jc      .shrink_failed

        ; Set malloc strategy to "last fit, high memory" (BX=0080h).
        mov     ax, 5801h
        mov     bx, 0080h
        int     21h

        ; Build the AH=4B parameter block in our own segment.
        push    cs
        pop     ax
        mov     [exec_block.cmd_seg], ax
        mov     [exec_block.fcb1_seg], ax
        mov     [exec_block.fcb2_seg], ax

        ; Spawn child.com.
        push    cs
        pop     es
        mov     bx, exec_block
        mov     dx, child_path
        mov     ax, 4B00h
        int     21h
        jc      .spawn_failed

        ; Get child exit code (AL = exit code, AH = exit type).
        mov     ah, 4Dh
        int     21h
        cmp     al, 0
        jne     .child_failed

        ; All good.
        mov     dx, msg_ok
        mov     ah, 9
        int     21h
        mov     ax, 4C00h
        int     21h

.shrink_failed:
        mov     dx, msg_shrink
        mov     ah, 9
        int     21h
        mov     ax, 4C01h
        int     21h

.spawn_failed:
        mov     dx, msg_spawn
        mov     ah, 9
        int     21h
        mov     ax, 4C02h
        int     21h

.child_failed:
        mov     dx, msg_child
        mov     ah, 9
        int     21h
        mov     ax, 4C03h
        int     21h

child_path:     db 'CHILD.COM',0

msg_ok:         db 'parent: in-place spawn OK',13,10,'$'
msg_shrink:     db 'parent: AH=4A shrink failed',13,10,'$'
msg_spawn:      db 'parent: AH=4B spawn failed',13,10,'$'
msg_child:      db 'parent: child exit code != 0',13,10,'$'

; AH=4B parameter block. The segment fields are filled in at runtime
; (CS may not be a known constant at assembly time).
exec_block:
        dw      0                       ; +00: env seg (0 = inherit parent's)
        dw      cmd_tail                ; +02: cmd-tail offset
.cmd_seg:
        dw      0                       ; +04: cmd-tail seg (set at runtime)
        dw      fcb_blank               ; +06: FCB1 offset
.fcb1_seg:
        dw      0                       ; +08: FCB1 seg (set at runtime)
        dw      fcb_blank               ; +0A: FCB2 offset
.fcb2_seg:
        dw      0                       ; +0C: FCB2 seg (set at runtime)

cmd_tail:
        db      0,13                    ; empty command tail terminated with CR

fcb_blank:
        db      0                       ; default drive
        db      '           '           ; 11 spaces (no filename)
        times   25 db 0                 ; rest of FCB
