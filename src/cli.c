#include "cli.h"
#include "db.h"
#include "pubsub.h"
#include "client_manager.h"
#include "tokenizer.h"

#include <unistd.h>
#include <termios.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define MAX_HISTORY 50
#define MAX_CMD_LEN 1024

char output_header[] = "[AdMQ CLI]";

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


// Adds a char buffer to the global history storage
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

void replace_line(char* buffer, size_t* length, size_t* cursor_idx, const char* new_text, const char* prompt) {
    printf("\r\x1b[K%s%s", prompt, new_text);
    strcpy(buffer, new_text);
    *length = strlen(buffer);
    *cursor_idx = *length;
    fflush(stdout);
}

char* get_input(const char* prompt) {
  printf("%s", prompt);
  fflush(stdout);

  enableRawMode();

  size_t bufsize = 1024;
  size_t length = 0;
  size_t cursor_idx = 0;
  char* buffer = calloc(bufsize, sizeof(char));

  int c;
  while (1) {
    c = read_char();
    if (c == -1) { break; }

      if (c == '\033') { // Escape sequence
        char seq[2];
        if (read(STDIN_FILENO, &seq[0], 1) == 0) return buffer;
        if (read(STDIN_FILENO, &seq[1], 1) == 0) return buffer;

        if (seq[0] == '[') { // Its an arrow key
          switch (seq[1]) {
            case 'A': // Up arrow
              if (history_view_idx > 0) {
                history_view_idx--;
                replace_line(buffer, &length, &cursor_idx, history[history_view_idx], prompt);
              }
              break;
            case 'B': // Down arrow
              if (history_view_idx < history_count) {
                history_view_idx++;

                if (history_view_idx == history_count) {
                  // We moved past the last history item -> go to empty line
                  replace_line(buffer, &length, &cursor_idx, "", prompt);
                }
                else {
                  // Show next item in history
                  replace_line(buffer, &length, &cursor_idx, history[history_view_idx], prompt);
                }
              }
              break;
            case 'D': // Left arrow
              if (cursor_idx > 0) {
                cursor_idx--;
                printf("\033[D");
                fflush(stdout);
              }
              break;
            case 'C': // Right arrow
              if (cursor_idx < length) {
                cursor_idx++;
                printf("\033[C");
                fflush(stdout);
              }
              break;
          }
        }
        continue; // Continue so the \033 doesn't make it to the output buffer
    }

      if ((c == '\n' || c == '\r') && buffer[length] != '\\') { // Enter key
          buffer[length] = '\0';
          printf("\r\n");
          break;
      }
      else if (c == 127) { // Backspace
        if (cursor_idx > 0) {

          if (cursor_idx < length) {
            memmove(&buffer[cursor_idx - 1], &buffer[cursor_idx], length - cursor_idx);
          }

          cursor_idx--;
          length--;
          buffer[length] = '\0';
          printf("\b");
          printf("%s", &buffer[cursor_idx]);
          printf(" ");


          size_t steps_back = (length - cursor_idx) + 1;
          for (size_t i = 0; i < steps_back; i++) {
            printf("\033[D");
          }

          fflush(stdout);
        }
      }
    else if (cursor_idx < length) { // Typing in the middle of the line
      memmove(&buffer[cursor_idx + 1], &buffer[cursor_idx], length - cursor_idx);
      buffer[cursor_idx] = c;
      length++;
      printf("%c", c);
      printf("%s", &buffer[cursor_idx + 1]);

      size_t steps_back = length - (cursor_idx + 1);
      for (size_t i = 0; i < steps_back; i++) {
          printf("\033[D");
      }
      cursor_idx++;
      fflush(stdout);
    }
    else { // Typing at the end of the line
      buffer[cursor_idx] = c;
      length++;
      printf("%c", c);
      cursor_idx++;
      fflush(stdout);
    }
  }

  disableRawMode();
  add_history(buffer);
  return buffer;
}

void* admin_cli_thread(void* arg) {
    // Give the server a second to print its startup logs before showing the prompt
    usleep(500000);

    while (1) {
        char* input = get_input("admq> ");

        if (input == NULL || strlen(input) == 0) {
            free(input);
            continue;
        }

        char **argv = NULL;
        int argc = tokenize_command(input, &argv);

        if (argc > 0) {
            if (strcmp(argv[0], "STATUS") == 0) {
                // Prints a status message
                client_manager_print_status();
                pubsub_print_status();

            } else if (strcmp(argv[0], "PUBLISH") == 0 && argc >= 3) {
                // Publishes to a specific channel
                char topic[64];
                char payload[800] = {0};
                strncpy(topic, argv[1], 63);

                for (int i = 2; i < argc; i++) {
                    strcat(payload, argv[i]);
                    if (i < argc - 1) strcat(payload, " ");
                }

                pubsub_publish(topic, payload);
                printf("%s Message dispatched to topic '%s'\n", output_header, topic);

            } else if (strcmp(argv[0], "SET") == 0 && argc >= 4) {
                // Sets a specific key/value pair in the database
                char target_host[128], key[64], value[256] = {0};
                strncpy(target_host, argv[1], 127);
                strncpy(key, argv[2], 63);

                for (int i = 3; i < argc; i++) {
                    strcat(value, argv[i]);
                    if (i < argc - 1) strcat(value, " ");
                }

                db_set_device_state(target_host, key, value);
                printf("%s State manually updated for %s.\n", output_header, target_host);

            } else if (strcmp(argv[0], "GET") == 0 && argc == 3) {
                // Acquires the value from a specific key in the database
                char target_host[128], key[64], value[256] = {0};
                strncpy(target_host, argv[1], 127);
                strncpy(key, argv[2], 63);

                if (db_get_device_state(target_host, key, value, sizeof(value))) {
                    printf("%s %s -> %s = %s\n", output_header, target_host, key, value);
                } else {
                    printf("%s State key '%s' not found for '%s'.\n",output_header, key, target_host);
                }

            } else if (strcmp(argv[0], "SUBSCRIBE") == 0 && argc == 3) {
                // Subscribes a specific hostname to a topic
                char target_host[128], topic[64] = {0};
                strncpy(target_host, argv[1], 127);
                strncpy(topic, argv[2], 63);

                // Leverage Map to look up hostname queries directly
                Client* c = client_get_and_lock_by_hostname(target_host);
                if (c) {
                    int fd = c->fd;
                    client_unlock(c);
                    pubsub_subscribe(fd, topic);
                } else {
                    printf("%s Error: No active connection found under %s.\n", output_header, target_host);
                }

            } else if (strcmp(argv[0], "UNSUBSCRIBE") == 0 && argc == 3) {
                // Unsubscribes a specific hostname from a topic
                char target_host[128], topic[64] = {0};
                strncpy(target_host, argv[1], 127);
                strncpy(topic, argv[2], 63);

                Client* c = client_get_and_lock_by_hostname(target_host);
                if (c) {
                    int fd = c->fd;
                    client_unlock(c);
                    pubsub_unsubscribe(fd, topic);
                } else {
                    printf("%s Error: No active connection found under %s.\n", output_header, target_host);
                }

            } else if (strcmp(argv[0], "EXIT") == 0) {
                printf("Shutting down CLI...\n");
                free_tokens(argv, argc);
                free(input);
                exit(0);

            } else {
                printf("%s Invalid command or missing arguments.\n", output_header);
                printf("  Usage: PUBLISH <topic> <\"message\">\n");
                printf("  Usage: SUBSCRIBE <hostname> <topic>\n");
                printf("  Usage: UNSUBSCRIBE <hostname> <topic>\n");
                printf("  Usage: SET <hostname> <key> <\"value\">\n");
                printf("  Usage: GET <hostname> <key>\n");
                printf("  Usage: STATUS\n");
                printf("  Usage: EXIT\n");
            }
        }

        free_tokens(argv, argc);
        free(input);
    }
    return NULL;
}

void cli_init() {
    pthread_t cli_tid;
    if (pthread_create(&cli_tid, NULL, admin_cli_thread, NULL) != 0) {
        perror("Failed to start CLI thread");
    }
    pthread_detach(cli_tid);
}

void cli_cleanup() {
    disableRawMode();
    printf("[CLI] Terminal state restored.\n");
}
