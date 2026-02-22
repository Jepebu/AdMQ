#ifndef ENROLL_H
#define ENROLL_H

// Processes an enrollment request, validates identity, signs the CSR,
// and writes the generated certificate back to the client socket.
void process_enrollment(int client_fd, const char* request_buffer);

#endif
