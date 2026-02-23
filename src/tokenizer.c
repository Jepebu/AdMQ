#include "tokenizer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Helper to add an argument to the dynamic argv list
static void add_arg(char ***argv, int *argc, const char *buffer) {
    *argv = realloc(*argv, sizeof(char*) * (*argc + 2));
    (*argv)[*argc] = strdup(buffer);
    (*argc)++;
    (*argv)[*argc] = NULL;
}

int tokenize_command(char *input_str, char ***argv_ptr) {
    *argv_ptr = NULL;
    int argc = 0;

    const size_t input_size = strlen(input_str);
    size_t buff_position = 0;
    int subshell_depth = 0;
    char arg_buffer[MAX_CMD_LEN];
    char current_char;

    bool single_quote = false;
    bool double_quote = false;
    bool char_escaped = false;

    for (size_t i = 0; i < input_size; i++) {
        current_char = input_str[i];

        if (buff_position >= MAX_CMD_LEN - 1) {
            fprintf(stderr, "Error: Argument exceeds maximum buffer size.\n");
            return -1;
        }

        if (char_escaped) {
            arg_buffer[buff_position++] = current_char;
            char_escaped = false;
        }
        else if (!single_quote && !double_quote && !char_escaped &&
             (current_char == '|' || current_char == '<' ||
              current_char == '>' || current_char == '(' || current_char == ')' ||
              current_char == '&')) {

            if (current_char == '(' && buff_position > 0 && arg_buffer[buff_position-1] == '$') {
                subshell_depth++;
                arg_buffer[buff_position++] = current_char;
                continue;
            }

            if (current_char == ')' && subshell_depth > 0) {
                subshell_depth--;
                arg_buffer[buff_position++] = current_char;
                continue;
            }

            if (subshell_depth > 0) {
                arg_buffer[buff_position++] = current_char;
                continue;
            }

            if (buff_position > 0) {
                arg_buffer[buff_position] = '\0';
                add_arg(argv_ptr, &argc, arg_buffer);
                buff_position = 0;
            }

            if (current_char == '&' && input_str[i+1] == '&') {
                add_arg(argv_ptr, &argc, "&&");
                i++;
            } else if (current_char == '|' && input_str[i+1] == '|') {
                add_arg(argv_ptr, &argc, "||");
                i++;
            } else if (current_char == '>' && input_str[i+1] == '>') {
                add_arg(argv_ptr, &argc, ">>");
                i++;
            } else {
                char op_str[2] = {current_char, '\0'};
                add_arg(argv_ptr, &argc, op_str);
            }
        }
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
        else if (current_char == '\'' && !double_quote) {
            single_quote = !single_quote;
        }
        else if (current_char == '\"' && !single_quote) {
            double_quote = !double_quote;
        }
        else if (current_char == '\\' && !double_quote && !single_quote) {
            char_escaped = true;
        }
        else {
            arg_buffer[buff_position++] = current_char;
        }
    }

    if (buff_position > 0) {
        arg_buffer[buff_position] = '\0';
        add_arg(argv_ptr, &argc, arg_buffer);
    }

    return argc;
}

void free_tokens(char **argv, int argc) {
    if (!argv) return;
    for (int i = 0; i < argc; i++) {
        free(argv[i]);
    }
    free(argv);
}
