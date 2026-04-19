# Development guidelines for kvikdos

## Workflow

- All work happens on the `improvements` branch. No feature branches or PRs.
- Commit directly to `improvements` and push to `origin/improvements`.
- When used as a submodule (e.g. in beta_kappa), the parent repo records the submodule commit separately.

## KVM vs Software CPU

- Backend selection uses `USE_KVM` define, not platform detection (`__linux__`).
- `__linux__` is only for genuinely OS-specific behavior (madvise semantics, xattr API).
- Linux default build: KVM. macOS: soft CPU. Test harness: always soft CPU.
- New code that differs between KVM and soft CPU must use `#ifdef USE_KVM`, never `#ifdef __linux__`.
- Synthetic high-memory regions like INVARS (DOS List of Lists at
  0xFFF7E..0xFFFB3) are served in two places: the KVM exit path
  handles them in `kvikdos.c` via the MMIO handler, and `cpu8086.c`'s
  `cpu_read()` answers the same bytes inline before it would fall
  through to the MMIO exit. The soft-CPU version MUST stay in sync
  with the KVM stub — an instruction like `mov ax, es:[bx-2]`
  completes within one `cpu_exec()` call, so setting exit_pending
  isn't enough; we have to return real data for the second byte too.

## Validation and --azzy

- Default mode is strict: MCB signature/PID/psize checks and filename validation (no dot-prefix, no trailing slash) are enforced.
- `--azzy` relaxes these for programs that manage their own MCBs or use non-standard filenames (e.g. VC.COM).
- When adding new DOS compatibility features, ask: does this break existing strict-mode programs? If yes, gate it behind `--azzy`.
- Exception: if kvikdos itself creates state that fails a check (like free trailing MCBs after shrink), fix the check unconditionally.

## Build artifacts

- Binaries (`kvikdos`, `kvikdos-soft`, `*.o`, `*.com`) must not be committed. They are in `.gitignore`.

## Test harness

- `test_harness.c` includes `kvikdos.c` via `#define KVIKDOS_TEST` -- always builds with soft CPU (no `USE_KVM`).
- Test harness defaults to `is_azzy=1` since it's designed for programs like VC.COM.
- `make test` validates both strict and `--azzy` modes.
