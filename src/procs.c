#include "procs.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/user.h>

int procs_names(char (*names)[20], int cap)
{
	int mib[3] = { CTL_KERN, KERN_PROC, KERN_PROC_PROC };
	static unsigned char buf[512 * 1024];
	size_t size = sizeof(buf);
	/* A short read (ENOMEM) still holds whole records: use what came. */
	int rc = sysctl(mib, 3, buf, &size, NULL, 0);
	if ((rc != 0 && size == 0) || size < sizeof(struct kinfo_proc)) return -1;
	/* The PS5's record does not match the SDK header: the kernel's own record size is the stride, and only fields
	   inside it are read (as ps5-exporter does). */
	size_t stride = (size_t)((const struct kinfo_proc*)buf)->ki_structsize;
	if (stride < offsetof(struct kinfo_proc, ki_comm) + COMMLEN + 1 || stride > size) return -1;
	int count = 0;
	for (size_t at = 0; at + stride <= size && count < cap; at += stride) {
		const char* comm = (const char*)(buf + at + offsetof(struct kinfo_proc, ki_comm));
		snprintf(names[count], sizeof(names[count]), "%.*s", COMMLEN, comm);
		if (names[count][0]) count++;
	}
	return count;
}
