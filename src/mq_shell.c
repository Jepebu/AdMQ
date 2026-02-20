// Linux / MacOS
#include <sys/wait.h>
#include <unistd.h>
#include <termios.h>
#define FILE_MODE 0644

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <fcntl.h>
#include <sys/stat.h>

#define MAX_HISTORY 50
#define MAX_ARGS 64
#define MAX_CMD_LEN 1024

// -- GLOBAL HISTORY STORAGE --
static char history[MAX_HISTORY][MAX_CMD_LEN];
static int history_count = 0;
static int history_view_idx = 0; // Where the user is currently looking


// Process spawner
pid_t spawn_command(char **argv, int input_fd, int output_fd) {
    pid_t pid = fork();
    if (pid == 0) {
        if (input_fd != STDIN_FILENO) dup2(input_fd, STDIN_FILENO);
        if (output_fd != STDOUT_FILENO) dup2(output_fd, STDOUT_FILENO);
        execvp(argv[0], argv);
        perror("execvp failed");
        exit(1);
    }
    return pid;
}


// --- C HELPER FUNCTIONS ---


// WINDOWS IMPLEMENTATION FOR KEYBOARD INPUT
#ifdef _WIN32
  // Windows doesn't need "enableRawMode" the same way,
  // but we might need to disable line buffering.
  void enableRawMode() {
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    GetConsoleMode(hStdin, &mode);
    // Disable Line Input and Echo
    SetConsoleMode(hStdin, mode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT));
  }

  void disableRawMode() {
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    GetConsoleMode(hStdin, &mode);
    // Re-enable Line Input and Echo
    SetConsoleMode(hStdin, mode | (ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT));
  }

  // Windows has _getch() which reads a char without echo (Raw by default)
  int read_char() {
    return _getch();
  }

// LINUX / MACOS IMPLEMENTATION FOR KEYBOARD INPUT
#else
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
    return -1; // Error
  }
#endif

// Helper to add an argument to the dynamic argv list
void add_arg(char ***argv, int *argc, const char *buffer) {
  *argv = realloc(*argv, sizeof(char*) * (*argc + 2)); // Resize array
  (*argv)[*argc] = strdup(buffer); // Copy string
  (*argc)++;
  (*argv)[*argc] = NULL; // Null terminate list
}

// Save a command line to the shell history buffer.
void add_history(const char* cmd) {
  if (strlen(cmd) == 0) return;

  // Shift everything down if full (Simple implementation)
  if (history_count == MAX_HISTORY) {
    for (int i = 0; i < MAX_HISTORY - 1; i++) {
      strcpy(history[i], history[i+1]);
    }
    history_count--;
  }

  // Add to end
  strncpy(history[history_count], cmd, MAX_CMD_LEN - 1);
  history[history_count][MAX_CMD_LEN - 1] = '\0';
  history_count++;

  // Reset view index to the NEW end (so pressing UP goes to the latest)
  history_view_idx = history_count;
}


void replace_line(char* buffer, size_t* length, size_t* cursor_idx, const char* new_text, const char* term_prompt) {
  // Fix visuals
  printf("\r");               // Move to line start
  printf("\x1b[K");           // Print ANSI code to clear the line
  printf("%s", term_prompt);  // Print prompt
  printf("%s", new_text);     // Print buffer content

  // Update Buffer Memory
  strcpy(buffer, new_text);
  *length = strlen(buffer);
  *cursor_idx = *length; // Set cursor to end of line

  fflush(stdout);
}

// --- EXECUTION ENGINE ---


int tokenize_command(char *input_str, char ***argv_ptr) {
  // Initialize output
  *argv_ptr = NULL;
  int argc = 0;

  const size_t input_size = strlen(input_str);
  size_t buff_position = 0;
  int subshell_depth = 0;
  char arg_buffer[MAX_CMD_LEN];
  char current_char;

  // Boolean values for tracking state.
  bool single_quote = false;
  bool double_quote = false;
  bool char_escaped = false;

  for (size_t i = 0; i < input_size; i++) {
    current_char = input_str[i];

    // [#0] Buffer Overflow Check
    if (buff_position >= MAX_CMD_LEN - 1) {
      fprintf(stderr, "Error: Argument exceeds maximum buffer size.\n");
      return -1;
    }

    // [#1] Escaped Character
    if (char_escaped) {
      arg_buffer[buff_position++] = current_char;
      char_escaped = false;
    }

    // [#2] Unquoted Operators (|, <, >, (, ), &)
    else if (!single_quote && !double_quote && !char_escaped &&
         (current_char == '|' || current_char == '<' ||
          current_char == '>' || current_char == '(' || current_char == ')' ||
          current_char == '&')) {

      // A. Substitution Start "$("
      if (current_char == '(' && buff_position > 0 && arg_buffer[buff_position-1] == '$') {
        subshell_depth++;
        arg_buffer[buff_position++] = current_char;
        continue;
      }

      // B. Substitution End ")"
      if (current_char == ')' && subshell_depth > 0) {
        subshell_depth--;
        arg_buffer[buff_position++] = current_char;
        continue;
      }

      // C. Inside Substitution -> Literal
      if (subshell_depth > 0) {
        arg_buffer[buff_position++] = current_char;
        continue;
      }

      // --- Delimiter Logic ---

      // 1. Flush current word if exists
      if (buff_position > 0) {
        arg_buffer[buff_position] = '\0';
        add_arg(argv_ptr, &argc, arg_buffer);
        buff_position = 0;
      }

      // 2. Handle Operators as separate args
      if (current_char == '&' && input_str[i+1] == '&') {
        add_arg(argv_ptr, &argc, "&&");
        i++; // Skip the second '&'
      } else if (current_char == '|' && input_str[i+1] == '|') {
        add_arg(argv_ptr, &argc, "||");
        i++; // Skip the second '|'
      } else if (current_char == '>' && input_str[i+1] == '>') {
        add_arg(argv_ptr, &argc, ">>");
        i++; // Skip the second '>'
      } else {
        // Catch-all for single operators: >, <, |
        char op_str[2] = {current_char, '\0'};
        add_arg(argv_ptr, &argc, op_str);
      }
    }
    // [#3] Whitespace
    else if (current_char == ' ' || current_char == '\n') {
      if (single_quote || double_quote || subshell_depth > 0) {
        arg_buffer[buff_position++] = current_char;
      } else {
        if (buff_position > 0) {
          arg_buffer[buff_position] = '\0';
          add_arg(argv_ptr, &argc, arg_buffer);
          buff_position = 0;
        }
      }
    }

    // [#4] Quotes
    // Note: We generally DO NOT want to include the quote characters themselves
    // in the final arg, unless you want the python script to see them.
    else if (current_char == '\'' && !double_quote) {
      single_quote = !single_quote;
    }
    else if (current_char == '\"' && !single_quote) {
      double_quote = !double_quote;
    }

    // [#5] Backslash Start
    else if (current_char == '\\' && !double_quote && !single_quote) {
      char_escaped = true;
    }

    // [Final] Regular Char
    else {
      arg_buffer[buff_position++] = current_char;
    }
  }

  // [CLEAN UP] Flush remaining buffer
  if (buff_position > 0) {
    arg_buffer[buff_position] = '\0';
    add_arg(argv_ptr, &argc, arg_buffer);
  }

  return argc;
}


// Helper: Executes a slice of tokens that only contains commands and pipes (|)
int execute_simple_pipeline(char **tokens, int count, int default_in, int default_out) {
  int i = 0;
  int prev_fd = default_in;
  int pipe_fds[2];
  pid_t pids[16];
  int pid_count = 0;
  int last_exit_code = 0;

  while (i < count) {
    char *cmd_argv[64];
    int cmd_argc = 0;

    int redirect_in_fd = -1;
    int redirect_out_fd = -1;

    // Extract arguments and handle redirection until we hit a pipe or the end
    while (i < count && strcmp(tokens[i], "|") != 0) {

      if (strcmp(tokens[i], "<") == 0) {
        if (i + 1 < count) {
          redirect_in_fd = open(tokens[i+1], O_RDONLY);
          if (redirect_in_fd < 0) perror(tokens[i+1]);
          i += 2; // Skip operator and filename
        } else {
          fprintf(stderr, "syntax error near unexpected token `<'\n");
          i++;
        }
      }
      else if (strcmp(tokens[i], ">") == 0) {
        if (i + 1 < count) {
          redirect_out_fd = open(tokens[i+1], O_WRONLY | O_CREAT | O_TRUNC, FILE_MODE);
          if (redirect_out_fd < 0) perror(tokens[i+1]);
          i += 2;
        } else {
          fprintf(stderr, "syntax error near unexpected token `>'\n");
          i++;
        }
      }
      else if (strcmp(tokens[i], ">>") == 0) {
        if (i + 1 < count) {
          redirect_out_fd = open(tokens[i+1], O_WRONLY | O_CREAT | O_APPEND, FILE_MODE);
          if (redirect_out_fd < 0) perror(tokens[i+1]);
          i += 2;
        } else {
          fprintf(stderr, "syntax error near unexpected token `>>'\n");
          i++;
        }
      }
      else {
        // Normal argument
        cmd_argv[cmd_argc++] = tokens[i++];
      }
    }
    cmd_argv[cmd_argc] = NULL;

    // Skip execution if no command was given (e.g., just "> file.txt")
    if (cmd_argc == 0) {
      if (redirect_in_fd != -1) close(redirect_in_fd);
      if (redirect_out_fd != -1) close(redirect_out_fd);
      if (i < count && strcmp(tokens[i], "|") == 0) i++;
      continue;
    }

    // --- PIPELINE SETUP ---
    int input_fd = prev_fd;
    int output_fd = default_out;
    int has_next = (i < count && strcmp(tokens[i], "|") == 0);

    if (has_next) {
      if (pipe(pipe_fds) == -1) { perror("pipe"); exit(1); }
      output_fd = pipe_fds[1]; // By default, write to the pipe
    }

    // --- REDIRECTION OVERRIDES ---
    // If the user specified a file, it overrides the pipe or default streams
    if (redirect_in_fd != -1) {
      input_fd = redirect_in_fd;
    }
    if (redirect_out_fd != -1) {
      // If there's a pipe AND file output (cmd > file | cmd2),
      // we still open the pipe for the next command, but THIS command writes to the file.
      output_fd = redirect_out_fd;
    }

    // --- EXECUTION ---
    pid_t pid = spawn_command(cmd_argv, input_fd, output_fd);
    if (pid > 0) pids[pid_count++] = pid;

    // --- CLEANUP FDs IN PARENT ---
    // Close pipe ends handed to the child
    if (input_fd != default_in && input_fd != redirect_in_fd) close(input_fd);
    if (output_fd != default_out && output_fd != redirect_out_fd) close(output_fd);

    // Close the opened files
    if (redirect_in_fd != -1) close(redirect_in_fd);
    if (redirect_out_fd != -1) close(redirect_out_fd);

    // --- ADVANCE TO NEXT PIPELINE STAGE ---
    if (has_next) {
      prev_fd = pipe_fds[0]; // Next command reads from the pipe
      i++; // Skip the "|" token
    }
  }

  // Wait for all children and capture the exit code of the last command
  for (int j = 0; j < pid_count; j++) {
    if (pids[j] > 0) {
#ifdef _WIN32
      int status;
      _cwait(&status, pids[j], 0);
      last_exit_code = status;
#else
      int status;
      waitpid(pids[j], &status, 0);
      if (WIFEXITED(status)) last_exit_code = WEXITSTATUS(status);
#endif
    }
  }
  return last_exit_code;
}


// Top-Level
int execute_logic_line(char *input, int default_in, int default_out) {
  char **tokens = NULL;
  int total_tokens = tokenize_command(input, &tokens);

  if (total_tokens <= 0 || tokens == NULL) return 0;

  int i = 0;
  int last_exit_code = 0;
  int skip_next = 0; // 0 = execute, 1 = skip

  while (i < total_tokens) {
    int start = i;

    // Fast-forward until we hit a logical operator
    while (i < total_tokens && strcmp(tokens[i], "&&") != 0 && strcmp(tokens[i], "||") != 0) {
      i++;
    }

    // Execute this chunk if we aren't skipping it
    if (!skip_next && (i > start)) {
      last_exit_code = execute_simple_pipeline(&tokens[start], i - start, default_in, default_out);
    }

    // Evaluate the logical operator to decide what to do with the NEXT chunk
    if (i < total_tokens) {
      char *op = tokens[i];
      if (strcmp(op, "&&") == 0) {
        // AND: Skip next if the current command failed (non-zero)
        skip_next = (last_exit_code != 0); 
      } else if (strcmp(op, "||") == 0) {
        // OR: Skip next if the current command succeeded (zero)
        skip_next = (last_exit_code == 0); 
      }
      i++; // Skip the operator token itself
    }
  }

  // Clean up memory allocated by the tokenizer
  for (int k = 0; k < total_tokens; k++) free(tokens[k]);
  free(tokens);

  return last_exit_code;
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
    // WINDOWS ARROW KEY LOGIC
    // ---------------------------------------------------------
    #ifdef _WIN32
      // --- 1. HANDLE SPECIAL KEYS (Arrows, F-Keys, etc.) ---
      // Windows sends 0 or 0xE0 (224) first for special keys
      if (c == 0 || c == 0xE0) {
        // Read the SECOND code (the actual scan code)
        int special = read_char();

        // Handle the logic, but DO NOT print 'special' or 'c'
        switch (special) {
          case 72: // UP ARROW
            if (history_view_idx > 0) {
              history_view_idx--; // Move backwards
              replace_line(buffer, &length, &cursor_idx, history[history_view_idx], prompt);
            }
            break;

          case 80: // DOWN ARROW
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

          case 75: // LEFT ARROW (K)
            if (cursor_idx > 0) {
              cursor_idx--;
              printf("\b"); // Visual backspace
              fflush(stdout);
            }
            break;
          case 77: // RIGHT ARROW (M)
            if (cursor_idx < length) {
              cursor_idx++;
              printf("%c", buffer[cursor_idx-1]);
              fflush(stdout);
              // Note: Windows console might need specific API calls for non-destructive move,
              // but printing the existing char is a cheap hack that works.
            }
            break;
          }
          // Skip the rest of the loop
          // Do not let 'c' (0xE0) or 'special' (75) get added to buffer.
          continue;
      }

      // --- 2. HANDLE ENTER ---
      // Windows sends \r (13) for Enter
      if (c == '\r' && buffer[length] != '\\') {
        buffer[length] = '\0';
        printf("\n"); // Visual newline
        break;        // Stop reading
      }

      // --- 3. HANDLE BACKSPACE ---
      if (c == 8) { // Windows Backspace is 8
        if (cursor_idx > 0) {
          // 1. Shift memory left
          memmove(&buffer[cursor_idx - 1], &buffer[cursor_idx], length - cursor_idx);

          // 2. Update stats
          cursor_idx--;
          length--;
          buffer[length] = '\0'; // Null terminate

          // 3. Visual Update
          printf("\b"); // Move back
          printf("%s", &buffer[cursor_idx]); // Print tail
          printf(" "); // Erase ghost char

          // Move cursor back to correct spot
          int steps = length - cursor_idx + 1;
          for(int i=0; i<steps; i++) printf("\b");
          fflush(stdout);
        }
        continue;
      }

      if (c >= 32 && c <= 126) {
        if (cursor_idx < length) {
        // INSERT MODE LOGIC

        // 1. Shift Right (Include +1 for Null Terminator!)
        memmove(&buffer[cursor_idx + 1], &buffer[cursor_idx], length - cursor_idx + 1);

        // 2. Insert Char
        buffer[cursor_idx] = c;
        length++;
        buffer[length] = '\0';

        // 3. Print New Char
        printf("%c", c);

        // 4. Print the shifted tail
        printf("%s", &buffer[cursor_idx + 1]);

        // 5. Move Cursor Back
        int steps = length - (cursor_idx + 1);
        for(int i=0; i<steps; i++) printf("\b");
        fflush(stdout);

        cursor_idx++;
      } else {
        // APPEND MODE
        buffer[cursor_idx] = c;
        length++;
        buffer[length] = '\0';
        cursor_idx++;
        printf("%c", c);
        fflush(stdout);
      }
    }
  }

  buffer[length] = '\0'; // Ensure final string is safe
  disableRawMode();
  add_history(buffer);
  return buffer;

    #else
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
  #endif
}

// --- EXPANSION HELPERS ---

// VARIABLE EXPANSION ($VAR -> Value)
char* expand_variables(const char* input) {
  char *result = malloc(MAX_CMD_LEN);
  const char *p = input;
  char *d = result;

  while (*p) {
    if (*p == '$' && *(p+1) != '(') { // Found $, but not $(
      p++; // Skip $

      // Extract Var Name
      char var_name[128];
      int i = 0;
      while (isalnum(*p) || *p == '_') {
        var_name[i++] = *p++;
      }
      var_name[i] = '\0';

      // Get Value
      char *val = getenv(var_name);
      if (val) {
        strcpy(d, val);
        d += strlen(val);
      }
    } else {
      *d++ = *p++;
    }
  }
  *d = '\0';
  return result;
}

// CAPTURE OUTPUT (Runs a command and returns its stdout)
// Forward declarations for mutual recursion
char* expand_variables(const char* input);
char* expand_subshells(const char* input);

char* capture_command_output(char *cmd) {
  FILE *tmp = tmpfile();
  if (!tmp) return strdup("");

  int tmp_fd = fileno(tmp);

  char *expanded_vars = expand_variables(cmd);
  char *final_cmd = expand_subshells(expanded_vars);

  // Pass the temporary file descriptor directly into the pipeline
  execute_logic_line(final_cmd, STDIN_FILENO, tmp_fd);

  // Clean up allocated strings
  free(expanded_vars);
  free(final_cmd);

  fflush(stdout);
  rewind(tmp);

  char buffer[4096];
  memset(buffer, 0, sizeof(buffer));
  size_t n = fread(buffer, 1, sizeof(buffer) - 1, tmp);
  fclose(tmp);

  if (n > 0 && buffer[n-1] == '\n') {
    buffer[n-1] = '\0';
  } else {
    buffer[n] = '\0';
  }

  return strdup(buffer);
}


// SUBSHELL EXPANSION ($(cmd) -> Output)
char* expand_subshells(const char* input) {
  char *result = malloc(MAX_CMD_LEN);
  const char *p = input;
  char *d = result;

  while (*p) {
    if (*p == '$' && *(p+1) == '(') {
      // Found start of $()
      p += 2; // Skip $(

      // Extract Inner Command
      char sub_cmd[MAX_CMD_LEN];
      int i = 0;
      int paren_depth = 1;

      while (*p && paren_depth > 0) {
        if (*p == '(') paren_depth++;
        if (*p == ')') paren_depth--;

        if (paren_depth > 0) sub_cmd[i++] = *p++;
      }
      p++; // Skip closing )
      sub_cmd[i] = '\0';

      // Execute and Substitute
      char *output = capture_command_output(sub_cmd);
      if (output) {
        strcpy(d, output);
        d += strlen(output);
        free(output);
      }
    } else {
      *d++ = *p++;
    }
  }
  *d = '\0';
  return result;
}

// ASSIGNMENT HANDLING (VAR=VAL)
int handle_assignment(char *input) {
  // Check for '=' before any space
  char *equals = strchr(input, '=');
  char *space = strchr(input, ' ');

  // If '=' exists and is before any space (e.g. "A=B" not "ls --option=B")
  if (equals && (!space || equals < space)) {
    *equals = '\0'; // Split Key and Value
    char *key = input;
    char *val = equals + 1;

    // Using setenv (overwrite=1)
    if (setenv(key, val, 1) != 0) {
      perror("setenv");
    }
    return 1; // Handled
  }
  return 0; // Not an assignment
}

// --- THE ENTRY POINT ---
char* shell_start() {
  char* prompt = "shell> ";
  while (1) {
    char *raw_input = get_input(prompt);
    if (raw_input == NULL) break;
    if (strcmp(raw_input, "exit") == 0) {
      free(raw_input);
      break;
    }
    if (strlen(raw_input) == 0) {
      free(raw_input);
      continue;
    }

    // Expand Variables ($VAR -> VAL)
    char *expanded_vars = expand_variables(raw_input);
    free(raw_input);

    // Expand Subshells ($(cmd) -> output)
    char *final_cmd = expand_subshells(expanded_vars);
    free(expanded_vars);

    // Handle Assignment (VAR=VAL)
    // We do this on raw input so expansions don't mess up the assignment syntax
    if (handle_assignment(final_cmd)) {
      free(final_cmd);
      continue; // Skip execution
    }

    // Execute Pipeline
    execute_logic_line(final_cmd, STDIN_FILENO, STDOUT_FILENO);

    free(final_cmd);
  }
  return 0;
}
