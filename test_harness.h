/*
 * test_harness.h: threaded test harness for kvikdos.
 *
 * Runs a DOS program in a background thread and provides functions to
 * inspect the video buffer and inject keystrokes from the test thread.
 *
 * This file is part of kvikdos. Licensed under GNU GPL >=2.0.
 */

#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H

/* Common key constants: high byte = scancode, low byte = ASCII. */
#define KEY_ESC     0x011b
#define KEY_ENTER   0x1c0d
#define KEY_TAB     0x0f09
#define KEY_F1      0x3b00
#define KEY_F2      0x3c00
#define KEY_F3      0x3d00
#define KEY_F4      0x3e00
#define KEY_F5      0x3f00
#define KEY_F6      0x4000
#define KEY_F7      0x4100
#define KEY_F8      0x4200
#define KEY_F9      0x4300
#define KEY_F10     0x4400
#define KEY_ALT_X   0x2d00
#define KEY_UP      0x4800
#define KEY_DOWN    0x5000
#define KEY_LEFT    0x4b00
#define KEY_RIGHT   0x4d00

/*
 * Start kvikdos running a DOS program in a background thread.
 * prog_path:  path to the .COM or .EXE file.
 * mount_dir:  directory to mount as C: (NULL = current directory).
 * Returns 0 on success, -1 on failure.
 */
int kviktest_start(const char *prog_path, const char *mount_dir);

/*
 * Stop the kvikdos emulator (sends Ctrl+C / terminates thread).
 * Returns the DOS program exit code, or -1 if it didn't exit.
 */
int kviktest_stop(void);

/*
 * Check if the emulator is still running.
 * Returns 1 if running, 0 if exited.
 */
int kviktest_is_running(void);

/*
 * Wait until the emulator thread exits.
 * timeout_ms: max time to wait (0 = no wait, just check).
 * Returns the exit code if exited, -1 on timeout.
 */
int kviktest_wait_exit(unsigned timeout_ms);

/*
 * Send a keystroke to the emulator (pushed into the ring buffer).
 * key: high byte = scancode, low byte = ASCII char.
 *      Use KEY_* constants above.
 */
void kviktest_send_key(unsigned short key);

/*
 * Read a character from the video buffer at (row, col).
 * Returns the ASCII character, or 0 if out of bounds.
 */
char kviktest_read_char(int row, int col);

/*
 * Read a string from the video buffer starting at (row, col).
 * Copies up to max_len characters into buf (NUL-terminated).
 */
void kviktest_read_text(int row, int col, char *buf, int max_len);

/*
 * Wait until the given text appears at (row, col) in the video buffer.
 * Polls with short sleeps.
 * Returns 1 if found, 0 on timeout.
 */
int kviktest_wait_for_text(int row, int col, const char *text, unsigned timeout_ms);

/*
 * Assert that the given text is present at (row, col) right now.
 * Prints a PASS/FAIL message to stdout.
 * Returns 1 if matches, 0 if not.
 */
int kviktest_assert_text(int row, int col, const char *text, const char *label);

/*
 * Search the entire screen for a substring.
 * If found, returns 1 and sets *out_row, *out_col to the position.
 * If not found, returns 0.
 */
int kviktest_find_text(const char *text, int *out_row, int *out_col);

/*
 * Wait until the given text appears anywhere on screen.
 * Returns 1 if found (sets *out_row, *out_col), 0 on timeout.
 */
int kviktest_wait_for_text_anywhere(const char *text, unsigned timeout_ms, int *out_row, int *out_col);

/*
 * Enable instruction coverage tracking. Call before kviktest_start().
 * Tracks which (CS<<4)+IP addresses are executed by the software CPU.
 */
void kviktest_coverage_enable(void);

/*
 * Print coverage report for the loaded program.
 * prog_path: path to the .COM/.EXE file (used to determine total size).
 * code_bytes: if nonzero, use this as the denominator instead of file size
 *             (for programs with large data sections like VC.COM).
 * Returns coverage percentage (0-100).
 */
int kviktest_coverage_report(const char *prog_path, unsigned code_bytes);

/*
 * Dump the raw coverage bitmap to a file. The bitmap is 128 KB (1 bit per
 * byte in the 1 MB address space). Use with tools/coverage_report.py to
 * produce per-procedure coverage reports.
 * Returns 0 on success, -1 on error.
 */
int kviktest_coverage_dump(const char *path);

#endif /* TEST_HARNESS_H */
