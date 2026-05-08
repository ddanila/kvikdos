;
; child_exe.nasm: minimal MZ EXE child for the in-place-spawn repro,
; specifically exercising the AH=4B AL=00 fallback path when the EXE
; header asks for "give me all available memory" (maxalloc=0xFFFF).
;
; The child does two checks:
;
;   1) PSP[0x16] is the parent PSP segment. parent.nasm stamps the
;      sentinel "KVKDPRT1" at parent_psp:0103h. We read it back to
;      prove parent memory survived the spawn (same check as
;      child.nasm).
;
;   2) PSP[0x02] is the segment one past the end of the child's
;      allocated block. (PSP[0x02] - PSP_segment) paragraphs were
;      allocated to us. With strategy=128 (last-fit, high) and ~640 KB
;      free at parent-shrink time, the child should get most of that
;      — at least 0x4000 paragraphs (256 KB) here.
;
;      Pre-fix kvikdos: when memsize_max_para (~64 KiB cap) didn't fit,
;      the fallback used memsize_min_para — i.e. only ~16 paragraphs
;      after the image. Child exits AL=0xFD.
;
;      Post-fix: fallback uses largest_para clamped to
;      [memsize_min_para, memsize_max_para]. Child exits AL=0.
;
; NASM `-f bin` doesn't emit MZ headers, so we lay one out manually
; using two sections: `header` at file offset 0, `text` at file
; offset 32 with vstart=0 so labels inside .text resolve to CS-relative
; addresses (otherwise they'd include the 32-byte header offset).

bits 16
cpu 8086

header_paragraphs equ 2
header_bytes      equ header_paragraphs * 16
; Image size (bytes after the header) — image lives in the second
; section, computed via end_of_image - mz_start which both resolve in
; the same section (no cross-section division).
image_bytes       equ end_of_image - mz_start
total_bytes       equ header_bytes + image_bytes
total_pages       equ (total_bytes + 511) / 512
last_page_bytes   equ total_bytes - (total_pages - 1) * 512

section header start=0
        db 'MZ'
        dw last_page_bytes
        dw total_pages
        dw 0                        ; relocations
        dw header_paragraphs
        dw 0                        ; minalloc — paired with maxalloc=FFFF below
        dw 0xFFFF                   ; maxalloc; kvikdos's loader auto-grows
                                    ; memsize_min if SS:SP exceeds it only
                                    ; when minalloc=0 AND maxalloc=0xFFFF.
        dw 0                        ; SS (image-relative)
        dw 0xFFFE                   ; SP
        dw 0                        ; checksum
        dw 0                        ; IP
        dw 0                        ; CS (image-relative)
        dw 0                        ; relocations table offset
        dw 0                        ; overlay number
        times (header_paragraphs * 16) - ($-$$) db 0

; .text starts right after the header in the file, but its labels are
; CS-relative (vstart=0).
section .text follows=header vstart=0

mz_start:
        ; Save PSP segment (= initial DS) in BP for later.
        push    ds
        pop     bp

        ; PSP[0x16]: parent PSP segment.
        mov     ax, [0x16]
        cmp     ax, 0
        je      .no_parent

        ; ES = our CS so we can read sentinel_expected from ES:DI.
        push    cs
        pop     es
        ; DS = parent PSP for the parent_psp:[0x103] read.
        mov     dx, ax
        mov     ds, dx
        mov     si, 0x103
        mov     di, sentinel_expected
        mov     cx, 8
        cld
.cmp_loop:
        mov     al, [ds:si]
        cmp     al, [es:di]
        jne     .mismatch
        inc     si
        inc     di
        loop    .cmp_loop

        ; Restore DS = our PSP for the PSP[0x02] read.
        push    bp
        pop     ds

        ; PSP[0x02]: top-of-allocation segment.
        mov     ax, [0x02]
        sub     ax, bp                      ; paragraphs allocated to us
        cmp     ax, 0x4000                  ; ≥ 256 KB?
        jb      .too_small

        ; Pass: switch DS to CS (where messages live) and exit AL=0.
        push    cs
        pop     ds
        mov     dx, msg_ok
        mov     ah, 9
        int     21h
        mov     ax, 4C00h
        int     21h

.mismatch:
        push    cs
        pop     ds
        mov     dx, msg_mismatch
        mov     ah, 9
        int     21h
        mov     ax, 4CFFh
        int     21h

.no_parent:
        push    cs
        pop     ds
        mov     dx, msg_no_parent
        mov     ah, 9
        int     21h
        mov     ax, 4CFFh
        int     21h

.too_small:
        push    cs
        pop     ds
        mov     dx, msg_too_small
        mov     ah, 9
        int     21h
        mov     ax, 4CFDh
        int     21h

sentinel_expected:
        db      'KVKDPRT1'
msg_ok:
        db      'child_exe: parent OK and got >=256 KB',13,10,'$'
msg_mismatch:
        db      'child_exe: parent sentinel MISSING',13,10,'$'
msg_no_parent:
        db      'child_exe: PSP[0x16] is zero',13,10,'$'
msg_too_small:
        db      'child_exe: allocation too small (<256 KB)',13,10,'$'

end_of_image:
