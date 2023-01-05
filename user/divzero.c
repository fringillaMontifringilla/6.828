// buggy program - causes a divide by zero exception

#include <inc/lib.h>

int zero;
int one;

void
umain(int argc, char **argv)
{
	zero = 0;
    one = 1;
	cprintf("1/0 is %08x!\n", one/zero);
}

