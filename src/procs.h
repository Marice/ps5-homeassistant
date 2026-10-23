/* The names of the running processes (p_comm, at most 19 characters), read from the kernel. On the console only. */
#ifndef PROCS_H
#define PROCS_H

#define PROCS_MAX 512

/* Fills names; returns the count, or -1 when the process table cannot be read. */
int procs_names(char (*names)[20], int cap);

#endif
