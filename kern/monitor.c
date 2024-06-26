// Simple command-line kernel monitor useful for
// controlling the kernel and exploring the system interactively.

#include <inc/stdio.h>
#include <inc/string.h>
#include <inc/memlayout.h>
#include <inc/assert.h>
#include <inc/x86.h>

#include <kern/console.h>
#include <kern/monitor.h>
#include <kern/kdebug.h>
#include <kern/pmap.h>
#include <kern/trap.h>

#define CMDBUF_SIZE	80	// enough for one VGA text line


struct Command {
	const char *name;
	const char *desc;
	// return -1 to force monitor to exit
	int (*func)(int argc, char** argv, struct Trapframe* tf);
};

static struct Command commands[] = {
	{ "help", "Display this list of commands", mon_help },
	{ "kerninfo", "Display information about the kernel", mon_kerninfo },
    { "backtrace", "calling backtrace", mon_backtrace },
    { "memmap", "show memory mapping", mon_memmap },
    { "continue", "continue(debug mode) the execution or exit(normal mode) the monitor", mon_continue }
};

/***** Implementations of basic kernel monitor commands *****/

int
mon_help(int argc, char **argv, struct Trapframe *tf)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(commands); i++)
		cprintf("%s - %s\n", commands[i].name, commands[i].desc);
	return 0;
}

int
mon_kerninfo(int argc, char **argv, struct Trapframe *tf)
{
	extern char _start[], entry[], etext[], edata[], end[];

	cprintf("Special kernel symbols:\n");
	cprintf("  _start                  %08x (phys)\n", _start);
	cprintf("  entry  %08x (virt)  %08x (phys)\n", entry, entry - KERNBASE);
	cprintf("  etext  %08x (virt)  %08x (phys)\n", etext, etext - KERNBASE);
	cprintf("  edata  %08x (virt)  %08x (phys)\n", edata, edata - KERNBASE);
	cprintf("  end    %08x (virt)  %08x (phys)\n", end, end - KERNBASE);
	cprintf("Kernel executable memory footprint: %dKB\n",
		ROUNDUP(end - entry, 1024) / 1024);
	return 0;
}

int
mon_backtrace(int argc, char **argv, struct Trapframe *tf)
{
    cprintf("Stack backtrace:\n");
    uint32_t ebp = read_ebp();
    while(ebp){
        uint32_t eip = *(uint32_t*)(ebp+4);
        uint32_t arg1 = *(uint32_t*)(ebp+8);
        uint32_t arg2 = *(uint32_t*)(ebp+12);
        uint32_t arg3 = *(uint32_t*)(ebp+16);
        uint32_t arg4 = *(uint32_t*)(ebp+20);
        uint32_t arg5 = *(uint32_t*)(ebp+24);
        struct Eipdebuginfo info;
        debuginfo_eip(eip, &info);
        cprintf("  ebp %08x  eip %08x  args %08x %08x %08x %08x %08x\n", ebp, eip, arg1, arg2, arg3, arg4, arg5);
        cprintf("         %s:%d: %.*s+%d\n",
                info.eip_file,
                info.eip_line,
                info.eip_fn_namelen,
                info.eip_fn_name,
                eip - info.eip_fn_addr);
        ebp = *(uint32_t*)ebp;
    }
	return 0;
}

int mon_memmap(int argc, char** argv, struct Trapframe *tf){
    cprintf("Memory Mapping:\n");
    cprintf("Virt\tPhys\tPerm\n");
    for(size_t i = 0;i <= 0xffffffff-PGSIZE+1;i+=PGSIZE){
        pte_t* pte = pgdir_walk(kern_pgdir, (void*)i, 0);
        if(!pte || !(*pte & PTE_P))
            continue;
        cprintf("0x%08x-0x%08x\t0x%08x-0x%08x\t%c%c%c\n",
                i, i + 0x1000,
                PTE_ADDR(*pte), PTE_ADDR(*pte) + 0x1000,
                (*pte & PTE_U) ? 'U' : '-',
                'R',
                (*pte & PTE_W) ? 'W' : '-');
    }
}

int mon_continue(int argc, char** argv, struct Trapframe *tf){
    return -1;
}

/***** Kernel monitor command interpreter *****/

#define WHITESPACE "\t\r\n "
#define MAXARGS 16

static int
runcmd(char *buf, struct Trapframe *tf)
{
	int argc;
	char *argv[MAXARGS];
	int i;

	// Parse the command buffer into whitespace-separated arguments
	argc = 0;
	argv[argc] = 0;
	while (1) {
		// gobble whitespace
		while (*buf && strchr(WHITESPACE, *buf))
			*buf++ = 0;
		if (*buf == 0)
			break;

		// save and scan past next arg
		if (argc == MAXARGS-1) {
			cprintf("Too many arguments (max %d)\n", MAXARGS);
			return 0;
		}
		argv[argc++] = buf;
		while (*buf && !strchr(WHITESPACE, *buf))
			buf++;
	}
	argv[argc] = 0;

	// Lookup and invoke the command
	if (argc == 0)
		return 0;
	for (i = 0; i < ARRAY_SIZE(commands); i++) {
		if (strcmp(argv[0], commands[i].name) == 0)
			return commands[i].func(argc, argv, tf);
	}
	cprintf("Unknown command '%s'\n", argv[0]);
	return 0;
}

void
monitor(struct Trapframe *tf)
{
	char *buf;

	cprintf("Welcome to the JOS kernel monitor!\n");
	cprintf("Type 'help' for a list of commands.\n");

	if (tf != NULL)
		print_trapframe(tf);

	while (1) {
		buf = readline("K> ");
		if (buf != NULL)
			if (runcmd(buf, tf) < 0)
				break;
	}
}
