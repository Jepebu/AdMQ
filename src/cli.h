#ifndef CLI_H
#define CLI_H

// Spawns the dedicated background thread for the Admin CLI
void cli_init();

// Restores the terminal to its default state
void cli_cleanup();

#endif
