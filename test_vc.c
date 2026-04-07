/*
 * test_vc.c: smoke test for Volkov Commander under kvikdos.
 *
 * Starts VC.COM in a background thread, waits for the TUI to render,
 * checks key screen elements, then stops the emulator.
 *
 * Usage: ./test_vc <path-to-VC.COM> [mount-dir]
 *
 * This file is part of kvikdos. Licensed under GNU GPL >=2.0.
 */

#include "test_harness.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  const char *vc_path;
  const char *mount_dir;
  int pass = 0, fail = 0;
  int r, c;

  if (argc < 2) {
    fprintf(stderr, "usage: %s <path-to-VC.COM> [mount-dir]\n", argv[0]);
    return 1;
  }
  vc_path = argv[1];
  mount_dir = argc > 2 ? argv[2] : NULL;

  printf("=== test_vc: starting VC.COM ===\n");
  if (kviktest_start(vc_path, mount_dir) != 0) {
    fprintf(stderr, "FAIL: could not start kvikdos\n");
    return 1;
  }

  /* Wait for the command line prompt at row 23 (bottom of screen, above F-key bar). */
  printf("Waiting for TUI to render...\n");
  if (!kviktest_wait_for_text(23, 0, "C:\\>", 5000)) {
    printf("  FAIL: command line prompt not found at row 23 within 5s\n");
    ++fail;
  } else {
    printf("  PASS: command line prompt at row 23\n");
    ++pass;
  }

  /* Check function key bar at the bottom row. */
  if (kviktest_wait_for_text_anywhere("Help", 1000, &r, &c)) {
    printf("  PASS: function key bar ('Help') at row=%d col=%d\n", r, c);
    ++pass;
  } else if (kviktest_wait_for_text_anywhere("1", 1000, &r, &c) && r >= 23) {
    printf("  PASS: function key bar (numbers) at row=%d\n", r);
    ++pass;
  } else {
    printf("  FAIL: function key bar not found\n");
    ++fail;
  }

  /* Check for 'Name' column header in panel. */
  if (kviktest_wait_for_text_anywhere("Name", 1000, &r, &c)) {
    printf("  PASS: panel header 'Name' at row=%d col=%d\n", r, c);
    ++pass;
  } else {
    printf("  FAIL: panel header 'Name' not found\n");
    ++fail;
  }

  /* Check that the emulator is still running (not crashed). */
  if (kviktest_is_running()) {
    printf("  PASS: emulator still running\n");
    ++pass;
  } else {
    printf("  FAIL: emulator exited unexpectedly\n");
    ++fail;
  }

  printf("Stopping emulator...\n");
  kviktest_stop();

  printf("\n=== test_vc: %d passed, %d failed ===\n", pass, fail);
  return fail > 0 ? 1 : 0;
}
