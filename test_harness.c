/*
 * test_harness.c: threaded test harness for kvikdos.
 *
 * Runs a DOS program in a background pthread and provides functions to
 * inspect the video buffer and inject keystrokes from the test thread.
 *
 * Build: this file #includes kvikdos.c with KVIKDOS_TEST defined,
 * so it gets all internal types and functions directly.
 *
 * This file is part of kvikdos. Licensed under GNU GPL >=2.0.
 */

#define KVIKDOS_TEST 1
#include "kvikdos.c"

#include "test_harness.h"

#include <pthread.h>

/* --- Harness state --- */

static pthread_t emu_thread;
static volatile int emu_running;
static volatile int emu_exit_code;

struct emu_thread_args {
  char prog_path[1024];
  char mount_dir[1024];
};
static struct emu_thread_args thread_args;

static void *emu_thread_func(void *arg) {
  struct emu_thread_args *a = (struct emu_thread_args *)arg;
  EmuState emu;
  DirState dir_state;
  TtyState tty_state;
  EmuParams emu_params;
  const char *args[] = { NULL };
  const char *envp[] = { "PATH=C:\\", "COMSPEC=C:\\COMMAND.COM", NULL };

  memset(&dir_state, 0, sizeof(dir_state));
  memset(&emu_params, 0, sizeof(emu_params));

  /* Set up drive C:. */
  dir_state.drive = 'C';
  dir_state.linux_mount_dir[2] = a->mount_dir[0] ? a->mount_dir : "";
  dir_state.case_mode[2] = CASE_MODE_UPPERCASE;

  /* Emulator params. */
  emu_params.mem_mb = 1;
  emu_params.dos_version = 5;
  emu_params.video_mode = VIDEO_VGA_COLOR;

  /* Use test key mode (ring buffer). */
  init_tty_state(&tty_state, -3);
  init_emu(&emu);

  emu_exit_code = run_dos_prog(&emu, a->prog_path, NULL, args, &dir_state,
                                &tty_state, &emu_params, envp);
  emu_running = 0;
  return NULL;
}

int kviktest_start(const char *prog_path, const char *mount_dir) {
  if (emu_running) return -1;

  strncpy(thread_args.prog_path, prog_path, sizeof(thread_args.prog_path) - 1);
  thread_args.prog_path[sizeof(thread_args.prog_path) - 1] = '\0';
  if (mount_dir) {
    strncpy(thread_args.mount_dir, mount_dir, sizeof(thread_args.mount_dir) - 1);
    thread_args.mount_dir[sizeof(thread_args.mount_dir) - 1] = '\0';
  } else {
    thread_args.mount_dir[0] = '\0';
  }

  emu_running = 1;
  emu_exit_code = -1;
  if (pthread_create(&emu_thread, NULL, emu_thread_func, &thread_args) != 0) {
    emu_running = 0;
    return -1;
  }
  /* Wait until the emulator has initialized the video buffer pointer. */
  { unsigned wait = 0;
    while (!g_dump_video_mem && emu_running && wait < 5000) { usleep(1000); wait++; }
  }
  return 0;
}

extern volatile int g_key_ring_abort;

int kviktest_stop(void) {
  if (!emu_running) return emu_exit_code;
  /* Signal the emulator to stop: set abort flag so blocking key reads return. */
  g_key_ring_abort = 1;
  /* Also push some ESC keys to unblock any non-ring-buffer blocking reads. */
  key_ring_push(KEY_ESC);
  key_ring_push(KEY_ESC);
  key_ring_push(KEY_ESC);
  { unsigned wait = 0;
    while (emu_running && wait < 2000) { usleep(10000); wait += 10; }
  }
  if (emu_running) {
    pthread_cancel(emu_thread);
  }
  pthread_join(emu_thread, NULL);
  emu_running = 0;
  g_key_ring_abort = 0;
  return emu_exit_code;
}

int kviktest_is_running(void) {
  return emu_running;
}

int kviktest_wait_exit(unsigned timeout_ms) {
  unsigned elapsed = 0;
  while (emu_running && elapsed < timeout_ms) {
    usleep(10000);
    elapsed += 10;
  }
  if (!emu_running) {
    pthread_join(emu_thread, NULL);
    return emu_exit_code;
  }
  return -1;
}

void kviktest_send_key(unsigned short key) {
  key_ring_push(key);
}

char kviktest_read_char(int row, int col) {
  unsigned ofs;
  if (row < 0 || row >= 25 || col < 0 || col >= 80) return 0;
  if (!g_dump_video_mem || !g_dump_video_size) return 0;
  ofs = (unsigned)((row * 80 + col) * 2);
  if (ofs >= g_dump_video_size) return 0;
  return (char)g_dump_video_mem[ofs];
}

void kviktest_read_text(int row, int col, char *buf, int max_len) {
  int i;
  for (i = 0; i < max_len - 1 && col + i < 80; ++i) {
    char c = kviktest_read_char(row, col + i);
    buf[i] = c ? c : ' ';
  }
  buf[i] = '\0';
}

int kviktest_wait_for_text(int row, int col, const char *text, unsigned timeout_ms) {
  unsigned elapsed = 0;
  int len = (int)strlen(text);
  char buf[81];
  if (len > 80) len = 80;
  while (elapsed < timeout_ms) {
    kviktest_read_text(row, col, buf, len + 1);
    if (memcmp(buf, text, len) == 0) return 1;
    usleep(10000);
    elapsed += 10;
  }
  return 0;
}

int kviktest_assert_text(int row, int col, const char *text, const char *label) {
  int len = (int)strlen(text);
  char buf[81];
  if (len > 80) len = 80;
  kviktest_read_text(row, col, buf, len + 1);
  if (memcmp(buf, text, len) == 0) {
    printf("  PASS: %s\n", label);
    return 1;
  } else {
    printf("  FAIL: %s (expected \"%s\", got \"%s\")\n", label, text, buf);
    return 0;
  }
}

int kviktest_find_text(const char *text, int *out_row, int *out_col) {
  int len = (int)strlen(text);
  int r, c;
  char buf[81];
  for (r = 0; r < 25; ++r) {
    kviktest_read_text(r, 0, buf, 81);
    for (c = 0; c <= 80 - len; ++c) {
      if (memcmp(buf + c, text, len) == 0) {
        if (out_row) *out_row = r;
        if (out_col) *out_col = c;
        return 1;
      }
    }
  }
  return 0;
}

int kviktest_wait_for_text_anywhere(const char *text, unsigned timeout_ms, int *out_row, int *out_col) {
  unsigned elapsed = 0;
  while (elapsed < timeout_ms) {
    if (kviktest_find_text(text, out_row, out_col)) return 1;
    usleep(10000);
    elapsed += 10;
  }
  return 0;
}
