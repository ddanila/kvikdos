;
; parent_exe.nasm: variant of parent.nasm that spawns child_exe.exe
; (an MZ EXE with maxalloc=0xFFFF) instead of child.com. Targets the
; AH=4B AL=00 fallback path when memsize_max_para can't fit and the
; loader has to clamp to the largest available block.
;
; Sentinel "KVKDPRT1" sits at offset 0x103 (right after the
; `jmp short begin; nop` prologue) — child_exe checks it via PSP[0x16].

bits 16
cpu 8086
org 0100h

_start:
        jmp     short begin
        nop
sentinel:
        db      'KVKDPRT1'

begin:
        push    cs
        pop     ds

        ; Shrink to ~4 KB so the freed space goes to the child.
        mov     ah, 4Ah
        mov     bx, 256
        int     21h
        jc      .shrink_failed

        ; Last-fit, high memory.
        mov     ax, 5801h
        mov     bx, 0080h
        int     21h

        ; Patch parameter-block segments at runtime.
        push    cs
        pop     ax
        mov     [exec_block.cmd_seg], ax
        mov     [exec_block.fcb1_seg], ax
        mov     [exec_block.fcb2_seg], ax

        ; Spawn CHILD_EX.EXE.
        push    cs
        pop     es
        mov     bx, exec_block
        mov     dx, child_path
        mov     ax, 4B00h
        int     21h
        jc      .spawn_failed

        ; Get child exit code.
        mov     ah, 4Dh
        int     21h
        cmp     al, 0
        jne     .child_failed

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

child_path:     db 'CHILD_EX.EXE',0

msg_ok:         db 'parent_exe: in-place EXE spawn OK',13,10,'$'
msg_shrink:     db 'parent_exe: AH=4A shrink failed',13,10,'$'
msg_spawn:      db 'parent_exe: AH=4B spawn failed',13,10,'$'
msg_child:      db 'parent_exe: child exit code != 0',13,10,'$'

exec_block:
        dw      0
        dw      cmd_tail
.cmd_seg:
        dw      0
        dw      fcb_blank
.fcb1_seg:
        dw      0
        dw      fcb_blank
.fcb2_seg:
        dw      0

cmd_tail:
        db      0,13

fcb_blank:
        db      0
        db      '           '
        times   25 db 0
