# In-place child spawn for AH=4B AL=00

## Why

Volkov Commander 4.99.09 does not boot under kvikdos. Tracing the
INT 21h calls during startup:

```
ah:4a (resize)  → VC shrinks to 0x66 paragraphs (1.6 KB resident stub)
ah:58 strategy=128  → "last fit, high memory" malloc strategy
ah:4b al:00 → spawn  program=(C:\KVIKPROG.OVL)
ah:4c al:ff cs:0110 ip:006c    ← child exits almost immediately, code 255
"Program too big to fit in memory"  ← message printed by the resident stub
```

`VC.COM` 4.99.09 is a **resident stub + swap-spawned overlay**:

* The resident `VC.COM` is a 9 KB stub. Its first bytes are the literal
  ASCII signature `RESIDENT` (file offset 0).
* The stub shrinks its DOS allocation to ~1.6 KB, sets the high-memory
  malloc strategy, and spawns `VC.OVL` (a 117 KB MZ EXE) as a child via
  `INT 21h AH=4Bh AL=00`.
* `VC.OVL` reads the parent stub's resident memory back at the original
  load segment to find the `RESIDENT` signature plus shared data, runs
  the UI, then `INT 21h AH=4Ch` returns to the stub, which terminates
  cleanly.

This pattern (resident stub + swap-spawned heavy child) is also used by
some compilers (Borland C++ 2.0 — `bcc.exe` driver / `tlink.exe`),
overlay loaders, and several DOS games. The kvikdos comment at
`kvikdos.c` (the AH=4B AL==00 branch) already calls out the gap:

> Even with `al == 0`, the correct behavior would be resuming execution
> of the parent process … after the child process has finished. However,
> kvikdos is not smart enough for that.

## What kvikdos does today

`kvikdos.c:do_exec` (around line 2642):

1. On `AH=4B AL=00`, save a snapshot of all 640 KB of guest memory plus
   regs/sregs into a `SpawnFrame` (host-side `malloc`).
2. Call `reset_emu(emu)` — wipes guest memory, refills the magic IVT,
   BIOS data area, etc.
3. Load the child program at `PSP_PARA` (0x100), exactly where the
   parent had been loaded.
4. Run the child.
5. On `AH=4C` exit, `memcpy` the snapshot back over guest memory, restore
   regs/sregs, set `AX=0`, `CF=0`, resume.

That works for "spawn-and-discard-parent-state" use cases (e.g. one-shot
compiler driver wrappers). It does **not** work for the resident-stub
pattern, because the child reads back from the parent's load segment
expecting to find the parent's still-running image. After step 2 those
bytes are zeroed, so the child fails its sanity check and bails out.

## What real DOS does

Real DOS keeps the parent in place. On `AH=4B AL=00`:

1. The parent's MCB chain remains untouched; `AH=4A` has already
   shrunk it to free space above.
2. DOS allocates a new MCB out of the **free** memory above the
   resident allocation (or below, depending on the malloc strategy
   set by `AH=58`).
3. The child is loaded *into that new MCB* — i.e. at a higher segment
   than the parent. The parent's bytes stay where they are.
4. The child's `PSP[0x16]` records the **parent PSP segment**, so the
   child can read back parent memory using ordinary `mov` instructions.
5. On the child's `AH=4C`, DOS frees the child's MCB chain (and any
   other MCBs the child allocated) and resumes the parent at its saved
   `int 21h` return address.

Importantly the parent's memory is **never copied or wiped**. The child
runs above it and only frees its own allocations on exit.

## Proposed kvikdos design

Goal: emulate real DOS spawn closely enough for resident-stub overlays
without rewriting the loader.

### Memory model

Today the loader assumes the program lives at `PSP_PARA = 0x100`. We
need a child to live at a higher para (above the parent's resident
allocation). Two options:

* **A. Parameterise the load segment.** Add a `load_psp_para` argument
  to `load_dos_executable_program`. For a top-level program it stays at
  `PSP_PARA`. For a child spawn, compute `child_psp_para` from the MCB
  chain after the parent's `AH=4A` shrink, respecting the malloc
  strategy.
* **B. Emulate via mock MCB allocation.** Walk the MCB chain to find a
  suitable hole, mark it as the child's PSP MCB, then load the child
  there.

(B) reuses the existing MCB plumbing in kvikdos, which already supports
`AH=48`/`AH=49`/`AH=4A` correctly (pts has tested this with `malloct.com`).
The cleanest approach is to drive the load through the existing MCB code:

1. Compute child memory size (`memsize_min_para` + image size).
2. Call the same internal function `AH=48` uses to allocate that many
   paragraphs honouring `malloc_strategy`. Use the returned segment as
   the child PSP segment.
3. Build the child PSP at that segment, copying parent PSP fields the
   spec requires (parent PSP segment at `+0x16`, env seg, `+0x2C`, etc.).
4. Load the child image at `child_psp + 0x10`.
5. Set CS:IP, SS:SP from the loaded image.

### Spawn frame: keep regs, drop the memory snapshot

Today `SpawnFrame` includes a `mem_snapshot` (640 KB malloc'd) so we can
restore parent memory on child exit. With in-place loading the parent
memory is *already* preserved — we only need to:

* Save parent regs/sregs/CS:IP at the int-21h return point (already done).
* Save parent's small per-process state: `mapped_handles`, `tick_count`,
  `dta_seg_ofs`, `cleanup_fn`, `last_dos_error_code`, `malloc_strategy`,
  `had_get_*`, `sphinx_cmm_flags`, `ctrl_break_checking`, `port_0x40_tick`.
* Drop `mem_snapshot` entirely — saves 640 KB per spawn level.

On child exit (`AH=4C`):

* Free the child's MCB chain (any MCBs whose owner-PID matches the
  child's PSP segment). DOS does this; we should too.
* Restore parent regs/sregs and per-process state.
* Set parent `AX=0`, `CF=0`. Done.

### Things that "stay the same"

* `had_get_first_mcb`, INVARS synthesis, magic IVT, BIOS data area —
  these are all global to the emulator session, not per-program. The
  child can read them just like the parent.
* `mapped_handles` carries open files. DOS's behaviour is: child
  inherits parent handles unless the parent set them no-inherit. For a
  first cut, inherit everything (matches what kvikdos already does
  effectively today).

### Things that need care

* **Stack location.** For a `.COM` child, real DOS sets `SP = 0xFFFE` at
  end of the child's segment. With the child loaded at a high segment,
  the child's SS:SP address must not collide with the parent's data.
  This is naturally true if the child's MCB ends well above its `org
  100h` data area.
* **Environment segment.** AH=4B AL=00 takes the env seg from the
  parameter block. If 0, inherit parent's. We need to allocate a new env
  block in the MCB chain (DOS does), copy contents, and stash that seg
  in the child's PSP at `+0x2C`. The existing env-copy code in
  `do_exec` (line 2708 onwards) can be reused; it just needs to write
  into the new env MCB instead of `ENV_PARA`.
* **`load_para` for AL=03 (overlay load).** The al=3 path
  *already* loads at a caller-specified segment. The al=0 path can
  share most of that machinery once we have a target segment.

## Test fixtures (what's in this branch)

`tests/inplace_spawn/parent.nasm` — minimal resident stub. Shrinks via
AH=4A, sets high-memory strategy, spawns `child.com` via AH=4B AL=00,
then prints whether the child completed cleanly.

`tests/inplace_spawn/child.nasm` — minimal child. Reads its
`PSP[0x16]` (parent PSP segment), reads back parent's first 8 bytes of
the load image (the literal sentinel `KVKDPRT1`), and exits with
`AL=0` on match or `AL=0xFF` on mismatch. Mirrors what `VC.OVL` does
when it looks for `RESIDENT` at the parent's image origin.

`tests/inplace_spawn/run.sh` — runs `parent.com` under kvikdos and
checks the output for the success message.

Currently this test FAILS (it reproduces the bug). After the fix lands
it should print:

```
parent: in-place spawn OK
```

and exit 0.

## Next steps

1. Get the test repro committed and confirmed-failing on this branch
   (this PR / commit).
2. Refactor `load_dos_executable_program` to accept `load_psp_para`.
3. Add an internal `dos_alloc_for_child(memsize_para, strategy)` that
   reuses the AH=48 allocator path.
4. Reroute the AH=4B AL=00 spawn path to:
   - allocate env block,
   - allocate child program block (PSP+image),
   - load child into new block,
   - run child.
5. On AH=4C from child, free child's MCBs, restore parent state,
   resume.
6. Run the `inplace_spawn` test — should pass.
7. Run the wider kvikdos test suite — should still pass.
8. Run beta_kappa's test suite — should still pass.
9. Run ddanila/vc's `test-kvikdos-4.99.09` against `build/4.99.09/VC.COM`
   — should boot to the panel.

## Open questions

* Should `MCB_PID` for the child's MCB be the child's PSP para, or the
  parent's PSP para? DOS stores the child's PSP para. We should match.
* Do we need the spawn-stack at all once memory is in-place? Yes — for
  regs/sregs and per-process state across nested spawns.
* What happens if the child itself spawns? It should work recursively
  with the same machinery. The spawn stack handles depth.
