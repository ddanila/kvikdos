# kvikdos improvements plan — VC.COM support

Goal: run VC.COM (Volkov Commander) under kvikdos as a test harness,
replacing QEMU for e2e tests. Enables native screen buffer access,
instruction-level code coverage, faster startup.

## Critical gaps (required for VC.COM to run)

### Phase 1: Video buffer core

1. **B800:0000 video memory** — Map a 4 KB buffer at B800:0000 for direct
   text-mode writes (char+attr pairs). Expose for read-back so tests can
   assert screen content.
2. **INT 10h AH=00 (set video mode)** — Stub: set mode vars, clear video buffer.
3. **INT 10h AH=06/07 (scroll up/down)** — Implement as memmove on video buffer.

### Phase 2: Display stubs

4. **INT 10h AH=12 (EGA info)** — Stub: return 80x25.
5. **INT 10h AH=11/30 (font info)** — Stub: return 25 rows.
6. **Port 3DAh (VGA status)** — Stub: toggle bit 0 on each IN read (vsync).

### Phase 3: System stubs

7. **BDA timer 0040:006Ch** — Increment at ~18.2 Hz.
8. **INT 21h AH=58/02-03 (UMB link)** — Stub: return "not available".

### Phase 4: Command execution

9. **INT 2Eh (execute command)** — Medium effort, needs command.com emulation.

## Code coverage instrumentation (software CPU only)

10. Record `(CS<<4)+IP` bitmap during `cpu_exec()`, dump on exit.
    Post-process: map physical addresses back to source lines via VC.LST.
    Only works with software CPU path (macOS, or Linux with `--no-kvm`).

## Smoke test

Use VC.COM binary: run under kvikdos, feed keystrokes, read video buffer
at B800:0000, assert expected screen content. Each gap fixed = more of
VC.COM works.
