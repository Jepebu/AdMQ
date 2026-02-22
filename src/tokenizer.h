#ifndef TOKENIZER_H
#define TOKENIZER_H

#include <stdbool.h>

#define MAX_CMD_LEN 1024

// Parses a raw string into an array of arguments.
// Returns the number of arguments (argc).
int tokenize_command(char *input_str, char ***argv_ptr);

// Frees the memory allocated by the tokenizer
void free_tokens(char **argv, int argc);

#endif
