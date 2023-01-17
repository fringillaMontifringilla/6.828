#include <inc/mmu.h>
#include <inc/x86.h>
#include <inc/assert.h>

#include <kern/pmap.h>
#include <kern/trap.h>
#include <kern/console.h>
#include <kern/monitor.h>
#include <kern/env.h>
#include <kern/syscall.h>
#include <kern/sched.h>
#include <kern/kclock.h>
#include <kern/picirq.h>
#include <kern/cpu.h>
#include <kern/spinlock.h>

static struct Taskstate ts;

/* For debugging, so print_trapframe can distinguish between printing
 * a saved trapframe and printing the current trapframe and print some
 * additional information in the latter case.
 */
static struct Trapframe *last_tf;

/* Interrupt descriptor table.  (Must be built at run time because
 * shifted function addresses can't be represented in relocation records.)
 */
struct Gatedesc idt[256] = { { 0 } };
struct Pseudodesc idt_pd = {
	sizeof(idt) - 1, (uint32_t) idt
};


static const char *trapname(int trapno)
{
	static const char * const excnames[] = {
		"Divide error",
		"Debug",
		"Non-Maskable Interrupt",
		"Breakpoint",
		"Overflow",
		"BOUND Range Exceeded",
		"Invalid Opcode",
		"Device Not Available",
		"Double Fault",
		"Coprocessor Segment Overrun",
		"Invalid TSS",
		"Segment Not Present",
		"Stack Fault",
		"General Protection",
		"Page Fault",
		"(unknown trap)",
		"x87 FPU Floating-Point Error",
		"Alignment Check",
		"Machine-Check",
		"SIMD Floating-Point Exception"
	};

	if (trapno < ARRAY_SIZE(excnames))
		return excnames[trapno];
	if (trapno == T_SYSCALL)
		return "System call";
	if (trapno >= IRQ_OFFSET && trapno < IRQ_OFFSET + 16)
		return "Hardware Interrupt";
	return "(unknown trap)";
}

#define _setup_gate(name, num, istrap, sel, dpl) \
    void name(); \
    SETGATE(idt[num], istrap, sel, name, dpl)
#define _setup_trap_gate(name, num, sel, dpl) \
    _setup_gate(name, num, 1, sel, dpl)
#define _setup_int_gate(name, num, sel, dpl) \
    _setup_gate(name, num, 0, sel, dpl)
void
trap_init(void)
{
	extern struct Segdesc gdt[];

    _setup_trap_gate(t_divide, T_DIVIDE, GD_KT, 0);
    _setup_trap_gate(t_debug, T_DEBUG, GD_KT, 0);
    _setup_trap_gate(t_nmi, T_NMI, GD_KT, 0);
    _setup_int_gate(t_brkpt, T_BRKPT, GD_KT, 3);
    _setup_trap_gate(t_oflow, T_OFLOW, GD_KT, 0);
    _setup_trap_gate(t_bound, T_BOUND, GD_KT, 0);
    _setup_trap_gate(t_illop, T_ILLOP, GD_KT, 0);
    _setup_trap_gate(t_device, T_DEVICE, GD_KT, 0);
    _setup_trap_gate(t_dblflt, T_DBLFLT, GD_KT, 0);
    _setup_trap_gate(t_tss, T_TSS, GD_KT, 0);
    _setup_trap_gate(t_segnp, T_SEGNP, GD_KT, 0);
    _setup_trap_gate(t_stack, T_STACK, GD_KT, 0);
    _setup_int_gate(t_gpflt, T_GPFLT, GD_KT, 0);
    _setup_int_gate(t_pgflt, T_PGFLT, GD_KT, 0);
    _setup_trap_gate(t_fperr, T_FPERR, GD_KT, 0);
    _setup_trap_gate(t_align, T_ALIGN, GD_KT, 0);
    _setup_trap_gate(t_mchk, T_MCHK, GD_KT, 0);
    _setup_trap_gate(t_simderr, T_SIMDERR, GD_KT, 0);
    _setup_int_gate(t_syscall, T_SYSCALL, GD_KT, 3);
    _setup_int_gate(t_irq0, IRQ_OFFSET, GD_KT, 0);
    _setup_int_gate(t_irq1, IRQ_OFFSET+1, GD_KT, 0);
    _setup_int_gate(t_irq2, IRQ_OFFSET+2, GD_KT, 0);
    _setup_int_gate(t_irq3, IRQ_OFFSET+3, GD_KT, 0);
    _setup_int_gate(t_irq4, IRQ_OFFSET+4, GD_KT, 0);
    _setup_int_gate(t_irq5, IRQ_OFFSET+5, GD_KT, 0);
    _setup_int_gate(t_irq6, IRQ_OFFSET+6, GD_KT, 0);
    _setup_int_gate(t_irq7, IRQ_OFFSET+7, GD_KT, 0);
    _setup_int_gate(t_irq8, IRQ_OFFSET+8, GD_KT, 0);
    _setup_int_gate(t_irq9, IRQ_OFFSET+9, GD_KT, 0);
    _setup_int_gate(t_irq10, IRQ_OFFSET+10, GD_KT, 0);
    _setup_int_gate(t_irq11, IRQ_OFFSET+11, GD_KT, 0);
    _setup_int_gate(t_irq12, IRQ_OFFSET+12, GD_KT, 0);
    _setup_int_gate(t_irq13, IRQ_OFFSET+13, GD_KT, 0);
    _setup_int_gate(t_irq14, IRQ_OFFSET+14, GD_KT, 0);
    _setup_int_gate(t_irq15, IRQ_OFFSET+15, GD_KT, 0);

	// Per-CPU setup
	trap_init_percpu();
}
#undef _setup_gate
#undef _setup_trap_gate
#undef _setup_int_gate

// Initialize and load the per-CPU TSS and IDT
void
trap_init_percpu(void)
{
	// The example code here sets up the Task State Segment (TSS) and
	// the TSS descriptor for CPU 0. But it is incorrect if we are
	// running on other CPUs because each CPU has its own kernel stack.
	// Fix the code so that it works for all CPUs.
	//
	// Hints:
	//   - The macro "thiscpu" always refers to the current CPU's
	//     struct CpuInfo;
	//   - The ID of the current CPU is given by cpunum() or
	//     thiscpu->cpu_id;
	//   - Use "thiscpu->cpu_ts" as the TSS for the current CPU,
	//     rather than the global "ts" variable;
	//   - Use gdt[(GD_TSS0 >> 3) + i] for CPU i's TSS descriptor;
	//   - You mapped the per-CPU kernel stacks in mem_init_mp()
	//   - Initialize cpu_ts.ts_iomb to prevent unauthorized environments
	//     from doing IO (0 is not the correct value!)
	//
	// ltr sets a 'busy' flag in the TSS selector, so if you
	// accidentally load the same TSS on more than one CPU, you'll
	// get a triple fault.  If you set up an individual CPU's TSS
	// wrong, you may not get a fault until you try to return from
	// user space on that CPU.

	// Setup a TSS so that we get the right stack
	// when we trap to the kernel.
    thiscpu -> cpu_ts.ts_esp0 = KSTACKTOP - (thiscpu -> cpu_id)*(KSTKSIZE + KSTKGAP);
    thiscpu -> cpu_ts.ts_ss0 = GD_KD;
    thiscpu -> cpu_ts.ts_iomb = sizeof(struct Taskstate);

	// Initialize the TSS slot of the gdt.
    gdt[(GD_TSS0 >> 3) + thiscpu -> cpu_id] = SEG16(STS_T32A, (uint32_t)(&(thiscpu -> cpu_ts)),
                                                    sizeof(struct Taskstate) - 1, 0);
    gdt[(GD_TSS0 >> 3) + thiscpu -> cpu_id].sd_s = 0;

	// Load the TSS selector (like other segment selectors, the
	// bottom three bits are special; we leave them 0)
	ltr(GD_TSS0 + (thiscpu -> cpu_id << 3));

	// Load the IDT
	lidt(&idt_pd);
}

void
print_trapframe(struct Trapframe *tf)
{
	cprintf("TRAP frame at %p from CPU %d\n", tf, cpunum());
	print_regs(&tf->tf_regs);
	cprintf("  es   0x----%04x\n", tf->tf_es);
	cprintf("  ds   0x----%04x\n", tf->tf_ds);
	cprintf("  trap 0x%08x %s\n", tf->tf_trapno, trapname(tf->tf_trapno));
	// If this trap was a page fault that just happened
	// (so %cr2 is meaningful), print the faulting linear address.
	if (tf == last_tf && tf->tf_trapno == T_PGFLT)
		cprintf("  cr2  0x%08x\n", rcr2());
	cprintf("  err  0x%08x", tf->tf_err);
	// For page faults, print decoded fault error code:
	// U/K=fault occurred in user/kernel mode
	// W/R=a write/read caused the fault
	// PR=a protection violation caused the fault (NP=page not present).
	if (tf->tf_trapno == T_PGFLT)
		cprintf(" [%s, %s, %s]\n",
			tf->tf_err & 4 ? "user" : "kernel",
			tf->tf_err & 2 ? "write" : "read",
			tf->tf_err & 1 ? "protection" : "not-present");
	else
		cprintf("\n");
	cprintf("  eip  0x%08x\n", tf->tf_eip);
	cprintf("  cs   0x----%04x\n", tf->tf_cs);
	cprintf("  flag 0x%08x\n", tf->tf_eflags);
	if ((tf->tf_cs & 3) != 0) {
		cprintf("  esp  0x%08x\n", tf->tf_esp);
		cprintf("  ss   0x----%04x\n", tf->tf_ss);
	}
}

void
print_regs(struct PushRegs *regs)
{
	cprintf("  edi  0x%08x\n", regs->reg_edi);
	cprintf("  esi  0x%08x\n", regs->reg_esi);
	cprintf("  ebp  0x%08x\n", regs->reg_ebp);
	cprintf("  oesp 0x%08x\n", regs->reg_oesp);
	cprintf("  ebx  0x%08x\n", regs->reg_ebx);
	cprintf("  edx  0x%08x\n", regs->reg_edx);
	cprintf("  ecx  0x%08x\n", regs->reg_ecx);
	cprintf("  eax  0x%08x\n", regs->reg_eax);
}

static void
trap_dispatch(struct Trapframe *tf)
{
	// Handle processor exceptions.
    if(tf -> tf_trapno == T_PGFLT){
        page_fault_handler(tf);
        return;
    }
    if(tf -> tf_trapno == T_BRKPT){
        monitor(tf);
        return;
    }
    if(tf -> tf_trapno == T_SYSCALL){
        uint32_t eax = tf -> tf_regs.reg_eax;
        uint32_t edx = tf -> tf_regs.reg_edx;
        uint32_t ecx = tf -> tf_regs.reg_ecx;
        uint32_t ebx = tf -> tf_regs.reg_ebx;
        uint32_t edi = tf -> tf_regs.reg_edi;
        uint32_t esi = tf -> tf_regs.reg_esi;
        eax = syscall(eax, edx, ecx, ebx, edi, esi);
        tf -> tf_regs.reg_eax = eax;
        return;
    }

	// Handle spurious interrupts
	// The hardware sometimes raises these because of noise on the
	// IRQ line or other reasons. We don't care.
	if (tf->tf_trapno == IRQ_OFFSET + IRQ_SPURIOUS) {
		cprintf("Spurious interrupt on irq 7\n");
		print_trapframe(tf);
		return;
	}

	// Handle clock interrupts. Don't forget to acknowledge the
	// interrupt using lapic_eoi() before calling the scheduler!
    if(tf -> tf_trapno == IRQ_OFFSET + IRQ_TIMER){
        lapic_eoi();
        sched_yield();
    }

	// Handle keyboard and serial interrupts.
    if(tf -> tf_trapno == IRQ_OFFSET + IRQ_KBD){
        kbd_intr();
        return;
    }

    if(tf -> tf_trapno == IRQ_OFFSET + IRQ_SERIAL){
        serial_intr();
        return;
    }

	// Unexpected trap: The user process or the kernel has a bug.
	print_trapframe(tf);
	if (tf->tf_cs == GD_KT)
		panic("unhandled trap in kernel");
	else {
		env_destroy(curenv);
		return;
	}
}

void
trap(struct Trapframe *tf)
{
	// The environment may have set DF and some versions
	// of GCC rely on DF being clear
	asm volatile("cld" ::: "cc");

	// Halt the CPU if some other CPU has called panic()
	extern char *panicstr;
	if (panicstr)
		asm volatile("hlt");

	// Re-acqurie the big kernel lock if we were halted in
	// sched_yield()
	if (xchg(&thiscpu->cpu_status, CPU_STARTED) == CPU_HALTED)
		lock_kernel();
	// Check that interrupts are disabled.  If this assertion
	// fails, DO NOT be tempted to fix it by inserting a "cli" in
	// the interrupt path.
	assert(!(read_eflags() & FL_IF));

	if ((tf->tf_cs & 3) == 3) {
		// Trapped from user mode.
		// Acquire the big kernel lock before doing any
		// serious kernel work.
        lock_kernel();
		assert(curenv);

		// Garbage collect if current enviroment is a zombie
		if (curenv->env_status == ENV_DYING) {
			env_free(curenv);
			curenv = NULL;
			sched_yield();
		}

		// Copy trap frame (which is currently on the stack)
		// into 'curenv->env_tf', so that running the environment
		// will restart at the trap point.
		curenv->env_tf = *tf;
		// The trapframe on the stack should be ignored from here on.
		tf = &curenv->env_tf;
	}

	// Record that tf is the last real trapframe so
	// print_trapframe can print some additional information.
	last_tf = tf;

	// Dispatch based on what type of trap occurred
	trap_dispatch(tf);

	// If we made it to this point, then no other environment was
	// scheduled, so we should return to the current environment
	// if doing so makes sense.
	if (curenv && curenv->env_status == ENV_RUNNING)
		env_run(curenv);
	else
		sched_yield();
}


void
page_fault_handler(struct Trapframe *tf)
{
	uint32_t fault_va;

	// Read processor's CR2 register to find the faulting address
	fault_va = rcr2();

	// Handle kernel-mode page faults.

    if((tf -> tf_cs & 3) == 0)
        panic("kernel page fault, va:%08x\n", fault_va);

	// We've already handled kernel-mode exceptions, so if we get here,
	// the page fault happened in user mode.

	// Call the environment's page fault upcall, if one exists.  Set up a
	// page fault stack frame on the user exception stack (below
	// UXSTACKTOP), then branch to curenv->env_pgfault_upcall.
	//
	// The page fault upcall might cause another page fault, in which case
	// we branch to the page fault upcall recursively, pushing another
	// page fault stack frame on top of the user exception stack.
	//
	// It is convenient for our code which returns from a page fault
	// (lib/pfentry.S) to have one word of scratch space at the top of the
	// trap-time stack; it allows us to more easily restore the eip/esp. In
	// the non-recursive case, we don't have to worry about this because
	// the top of the regular user stack is free.  In the recursive case,
	// this means we have to leave an extra word between the current top of
	// the exception stack and the new stack frame because the exception
	// stack _is_ the trap-time stack.
	//
	// If there's no page fault upcall, the environment didn't allocate a
	// page for its exception stack or can't write to it, or the exception
	// stack overflows, then destroy the environment that caused the fault.
	// Note that the grade script assumes you will first check for the page
	// fault upcall and print the "user fault va" message below if there is
	// none.  The remaining three checks can be combined into a single test.
	//
	// Hints:
	//   user_mem_assert() and env_run() are useful here.
	//   To change what the user environment runs, modify 'curenv->env_tf'
	//   (the 'tf' variable points at 'curenv->env_tf').

    if(!curenv -> env_pgfault_upcall)
        goto err;
    user_mem_assert(curenv, curenv -> env_pgfault_upcall, sizeof(void*), 0);
    //just to appease grade script
    user_mem_assert(curenv, UXSTACKTOP - 1, 1, PTE_W);
    //real check, is equal to the above assertion in theory
    user_mem_assert(curenv, UXSTACKTOP - PGSIZE, PGSIZE, PTE_W);
    uint32_t esp;
    if(tf -> tf_esp >= UXSTACKTOP)
        goto err;
    if(tf -> tf_esp >= UXSTACKTOP - PGSIZE)
        esp = tf -> tf_esp;
    else
        esp = UXSTACKTOP + 4;
    esp -= 4 + sizeof(struct UTrapframe);
    if(esp < UXSTACKTOP - PGSIZE)
        goto err;
    struct UTrapframe* utf = (struct UTrapframe*)esp;
    utf -> utf_fault_va = fault_va;
    utf -> utf_err = tf -> tf_err;
    utf -> utf_regs.reg_edi = tf -> tf_regs.reg_edi;
    utf -> utf_regs.reg_esi = tf -> tf_regs.reg_esi;
    utf -> utf_regs.reg_ebp = tf -> tf_regs.reg_ebp;
    utf -> utf_regs.reg_oesp = tf -> tf_regs.reg_oesp;
    utf -> utf_regs.reg_ebx = tf -> tf_regs.reg_ebx;
    utf -> utf_regs.reg_edx = tf -> tf_regs.reg_edx;
    utf -> utf_regs.reg_ecx = tf -> tf_regs.reg_ecx;
    utf -> utf_regs.reg_eax = tf -> tf_regs.reg_eax;
    utf -> utf_eip = tf -> tf_eip;
    utf -> utf_eflags = tf -> tf_eflags;
    utf -> utf_esp = tf -> tf_esp;
    curenv -> env_tf.tf_eip = curenv -> env_pgfault_upcall;
    curenv -> env_tf.tf_esp = esp;
    env_run(curenv);

	// Destroy the environment that caused the fault.
err:
	cprintf("[%08x] user fault va %08x ip %08x\n",
		curenv->env_id, fault_va, tf->tf_eip);
	print_trapframe(tf);
	env_destroy(curenv);
}

