# Development guidelines for kvikdos

## KVM vs Software CPU

- Backend selection uses `USE_KVM` define, not platform detection (`__linux__`).
- `__linux__` is only for genuinely OS-specific behavior (madvise semantics, xattr API).
- Linux default build: KVM. macOS: soft CPU. Test harness: always soft CPU.
- New code that differs between KVM and soft CPU must use `#ifdef USE_KVM`, never `#ifdef __linux__`.

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
