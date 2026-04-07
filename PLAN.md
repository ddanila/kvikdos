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

## Current status

Phases 1-3 are done. VC.COM 4.05 boots and renders its TUI (directory
panels, command line, F1-F10 bar) in the video buffer. Additional fixes
needed during bring-up: memory management (free Z-type MCBs, relaxed
MCB validation), permissive interrupt vector get/set, INT 10h AH=FE,
INT 33h mouse stub, IOCTL 0x0E stub, --video-mode flag.

## Threaded test harness (next major task)

Goal: in-process e2e tests for DOS programs running under kvikdos.

Architecture:
- **Thread 1 (emulator)**: runs `run_dos_prog()` — the kvikdos main loop
- **Thread 2 (test driver)**: polls `vga_mem` and injects keystrokes

API sketch:
- `wait_for_text(row, col, "expected", timeout_ms)` — scan vga_mem
  in a loop until the text appears or timeout
- `send_key(scancode, ascii)` — push into a shared keystroke queue
  that INT 16h reads from instead of stdin
- `assert_screen(row, col, "expected")` — immediate check

Implementation notes:
- Expose `vga_mem` pointer and a keystroke ring buffer as globals
- kvikdos is a monolithic .c file; no refactoring needed — just add
  the globals and a pthread-based test runner (test_vc.c)
- INT 16h AH=00/10 (wait for key) reads from the shared queue;
  AH=01/11 (check buffer) peeks without blocking
- Test program links kvikdos.o + cpu8086.o + test_vc.o

Example test scenario:
```
start_kvikdos("VC.COM");
wait_for_text(24, 0, "C:\\>", 5000);       // command line appeared
wait_for_text(0, 0, "C:\\", 5000);          // directory path
send_key(KEY_F10, 0);                       // quit
wait_for_text(12, 30, "Quit", 2000);        // quit dialog
send_key(KEY_ENTER, '\r');                   // confirm
wait_for_exit(2000);                         // VC.COM exits
```

## Phase 4: Command execution

9. **INT 2Eh (execute command)** — Medium effort. Consider using real
   COMMAND.COM from ddanila/msdos repo via INT 21h/4Bh spawn instead
   of reimplementing.

## Code coverage instrumentation (software CPU only)

10. Record `(CS<<4)+IP` bitmap during `cpu_exec()`, dump on exit.
    Post-process: map physical addresses back to source lines via VC.LST.
    Only works with software CPU path (macOS, or Linux with `--no-kvm`).
