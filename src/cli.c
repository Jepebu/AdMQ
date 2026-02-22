#include "cli.h"
#include "pubsub.h"
#include "client_manager.h"
#include <unistd.h>
#include <termios.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define MAX_HISTORY 50
#define MAX_CMD_LEN 1024

static char history[MAX_HISTORY][MAX_CMD_LEN];
static int history_count = 0;
static int history_view_idx = 0;
struct termios orig_termios;

void enableRawMode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

void disableRawMode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

int read_char() {
    char c;
    if (read(STDIN_FILENO, &c, 1) == 1) return c;
    return -1; 
}

void add_history(const char* cmd) {
    if (strlen(cmd) == 0) return;
    if (history_count == MAX_HISTORY) {
        for (int i = 0; i < MAX_HISTORY - 1; i++) {
            strcpy(history[i], history[i+1]);
        }
        history_count--;
    }
    strncpy(history[history_count], cmd, MAX_CMD_LEN - 1);
    history[history_count][MAX_CMD_LEN - 1] = '\0';
    history_count++;
    history_view_idx = history_count;
}

void replace_line(char* buffer, size_t* length, size_t* cursor_idx, const char* new_text, const char* term_prompt) {
    printf("\r\x1b[K%s%s", term_prompt, new_text); 
    strcpy(buffer, new_text);
    *length = strlen(buffer);
    *cursor_idx = *length; 
    fflush(stdout);
}


// Interactive command line handler
char* get_input(const char* prompt) {
  printf("%s", prompt);
  fflush(stdout);

  enableRawMode(); // Turn off buffering/echo

  // Allocate a buffer for the user's input
  size_t bufsize = 1024;
  size_t length = 0;
  size_t cursor_idx = 0;
  char* buffer = calloc(bufsize, sizeof(char));

  int c;
  while (1) {
    c = read_char();
    if (c == -1) { break; }

    // ---------------------------------------------------------
    // LINUX / MACOS LOGIC
    // ---------------------------------------------------------
      if (c == '\033') { // Escape sequence
        char seq[2];
        // Read the next two bytes immediately
        if (read(STDIN_FILENO, &seq[0], 1) == 0) return buffer;
        if (read(STDIN_FILENO, &seq[1], 1) == 0) return buffer;

        if (seq[0] == '[') { // Its an arrow key
          switch (seq[1]) {

            case 'A': // UP ARROW
              if (history_view_idx > 0) {
                history_view_idx--; // Move backwards
                replace_line(buffer, &length, &cursor_idx, history[history_view_idx], prompt);
              }
              break;

            case 'B': // DOWN ARROW
              if (history_view_idx < history_count) {
                history_view_idx++; // Move fowards

                if (history_view_idx == history_count) {
                  // We moved past the last history item, go to empty line.
                  replace_line(buffer, &length, &cursor_idx, "", prompt);
                }
                else {
                  // Show next history item
                  replace_line(buffer, &length, &cursor_idx, history[history_view_idx], prompt);
                }
              }
              break;

            case 'D': // LEFT ARROW
              if (cursor_idx > 0) {
                cursor_idx--;
                printf("\033[D"); // ANSI code to move cursor left visually
                fflush(stdout);
              }
              break;
            case 'C': // RIGHT ARROW
              if (cursor_idx < length) {
                cursor_idx++;
                printf("\033[C"); // ANSI code to move cursor right visually
                fflush(stdout);
              }
              break;
          }
        }
        continue; // Continue so the \033 doesn't get added to the output buffer.
    }


      /* [UNIX ENTER]
         Reset the cursor_idx and length, move to
         the next line, and break the loop.
      */
      if ((c == '\n' || c == '\r') && buffer[length] != '\\') { // User hit Enter
          buffer[length] = '\0';
          printf("\r\n"); // Move to next line visually
          break;
      }

      /* [UNIX BACKSPACE]
         We do some left buffer shifting, then reprint.
      */
      else if (c == 127) {
        if (cursor_idx > 0) { // Make sure we aren't at the start of the line.

          if (cursor_idx < length) {
            memmove(&buffer[cursor_idx - 1], &buffer[cursor_idx], length - cursor_idx); // Shift buffer left one
          }


          cursor_idx--;
          length--;
          buffer[length] = '\0';
          printf("\b"); // Visual backspace
          printf("%s", &buffer[cursor_idx]); // Print the tail
          printf(" "); // Erase ghost char


          size_t steps_back = (length - cursor_idx) + 1;  //
          for (size_t i = 0; i < steps_back; i++) {       //
            printf("\033[D");                             //  Fix cursor position
          }                                               //

          fflush(stdout);
        }
      }

    /* [NORMAL TYPING - MIDDLE OF STRING]
       We do some right buffer shifting and reprinting.
    */
    else if (cursor_idx < length) {
      // [#1] Shift the memory.
      memmove(&buffer[cursor_idx + 1], &buffer[cursor_idx], length - cursor_idx);

      // Insert the new char
      buffer[cursor_idx] = c;
      length++; // Total string got longer


      // [#2] Redraw the visuals.
      printf("%c", c);
      printf("%s", &buffer[cursor_idx + 1]);


      // [#3] Fix the cursor position.
      size_t steps_back = length - (cursor_idx + 1);
      for (size_t i = 0; i < steps_back; i++) {
          printf("\033[D");
      }

      // Increment our logical cursor position
      cursor_idx++;

      // Flush stdout
      fflush(stdout);
    }

    // [NORMAL TYPING - END OF STRING]
    // Nothing fancy here, just append it and print it.
    else {
      buffer[cursor_idx] = c;
      length++;
      printf("%c", c); // Manual echo
      cursor_idx++;
      fflush(stdout);
    }

    // TODO: Add buffer resizing logic here if position >= bufsize
  }

  disableRawMode(); // Restore terminal
  add_history(buffer);
  return buffer; // Return the pointer
}

// --- NEW: THE THREAD LOOP ---
void* admin_cli_thread(void* arg) {
    // Give the server a second to print its startup logs before showing the prompt
    usleep(500000);

    while (1) {
        char* input = get_input("JepMQ> ");

        if (input == NULL || strlen(input) == 0) {
            free(input);
            continue;
        }

        char cmd[32] = {0};
        char topic[64] = {0};
        char payload[800] = {0};

        int parsed = sscanf(input, "%31s %63s %799[^\n]", cmd, topic, payload);

        if (strcmp(cmd, "STATUS") == 0) {
            client_manager_print_status();
            pubsub_print_status();
        }
        else if (parsed == 3 && strcmp(cmd, "PUBLISH") == 0) {
            pubsub_publish(topic, payload);
            printf("[Admin] Message dispatched to topic '%s'\n", topic);
        }
        else if (strcmp(cmd, "EXIT") == 0) {
            printf("Shutting down CLI...\n");
            free(input);
            exit(0); // Forcibly shutdown the broker
        }
        else {
            printf("[Admin Error] Invalid command.\n");
            printf("  Usage: PUBLISH <topic> <message>\n");
            printf("  Usage: STATUS\n");
            printf("  Usage: EXIT\n");
        }

        free(input);
    }
    return NULL;
}

void cli_init() {
    pthread_t cli_tid;
    if (pthread_create(&cli_tid, NULL, admin_cli_thread, NULL) != 0) {
        perror("Failed to start CLI thread");
    }
    // We detach it so we don't have to join it later
    pthread_detach(cli_tid);
}


void cli_cleanup() {
    disableRawMode(); // Restores the terminal's ECHO and ICANON flags
    printf("[CLI] Terminal state restored.\n");
}
