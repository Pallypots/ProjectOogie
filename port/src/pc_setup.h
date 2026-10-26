/* Project Oogie: first-run setup from the player's disc image, see pc_setup.c. */
#ifndef PC_SETUP_H
#define PC_SETUP_H

/* Makes game\default.xbe reachable from the working directory: finds it next
 * to the program (or next to bin\), or copies the game from a disc image the
 * player picks. Returns 0 if the player cancelled or setup failed. */
int pc_setup_ensure_game(void);

/* Test mode (--extract-iso <image> <folder>): check and copy, no window. */
int pc_setup_extract_cli(const char *iso, const char *out);

/* Started without play.bat (RECOMP_PB_EXEC unset): the runtime switches,
 * saves in saves\ (backed up to saves_backup\), the log in logs\play.log. */
void pc_setup_standalone(void);

/* 1 when pc_setup_standalone set things up (no play.bat or test script). */
int pc_setup_is_standalone(void);

#endif
