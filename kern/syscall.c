/* See COPYRIGHT for copyright information. */

#include "env.h"
#include <inc/x86.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/assert.h>

#include <kern/env.h>
#include <kern/pmap.h>
#include <kern/trap.h>
#include <kern/syscall.h>
#include <kern/console.h>
#include <kern/sched.h>
#include <kern/time.h>
#include <kern/e1000.h>

// Print a string to the system console.
// The string is exactly 'len' characters long.
// Destroys the environment on memory errors.
static void
sys_cputs(const char *s, size_t len)
{
	// Check that the user has permission to read memory [s, s+len).
	// Destroy the environment if not.

    user_mem_assert(curenv, (const void*)s, len, PTE_U);

	// Print the string supplied by the user.
	cprintf("%.*s", len, s);
}

// Read a character from the system console without blocking.
// Returns the character, or 0 if there is no input waiting.
static int
sys_cgetc(void)
{
	return cons_getc();
}

// Returns the current environment's envid.
static envid_t
sys_getenvid(void)
{
	return curenv->env_id;
}

// Destroy a given environment (possibly the currently running environment).
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
static int
sys_env_destroy(envid_t envid)
{
	int r;
	struct Env *e;

	if ((r = envid2env(envid, &e, 1)) < 0)
		return r;
	env_destroy(e);
	return 0;
}

// Deschedule current environment and pick a different one to run.
static void
sys_yield(void)
{
	sched_yield();
}

// Allocate a new environment.
// Returns envid of new environment, or < 0 on error.  Errors are:
//	-E_NO_FREE_ENV if no free environment is available.
//	-E_NO_MEM on memory exhaustion.
static envid_t
sys_exofork(void)
{
	// Create the new environment with env_alloc(), from kern/env.c.
	// It should be left as env_alloc created it, except that
	// status is set to ENV_NOT_RUNNABLE, and the register set is copied
	// from the current environment -- but tweaked so sys_exofork
	// will appear to return 0.

    struct Env* env;
    int result = env_alloc(&env, curenv -> env_id);
    if(result < 0)
        return result;
    env -> env_status = ENV_NOT_RUNNABLE;
    memcpy(&env -> env_tf, &curenv -> env_tf, sizeof(struct Trapframe));
    env -> env_tf.tf_regs.reg_eax = 0;
    return env -> env_id;
}

// Set envid's env_status to status, which must be ENV_RUNNABLE
// or ENV_NOT_RUNNABLE.
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
//	-E_INVAL if status is not a valid status for an environment.
static int
sys_env_set_status(envid_t envid, int status)
{
	// Hint: Use the 'envid2env' function from kern/env.c to translate an
	// envid to a struct Env.
	// You should set envid2env's third argument to 1, which will
	// check whether the current environment has permission to set
	// envid's status.

    if(status < 0 || status > ENV_NOT_RUNNABLE)
        return -E_INVAL;
    struct Env* env;
    if(envid2env(envid, &env, 1) < 0)
        return -E_BAD_ENV;
    env -> env_status = status;
    return 0;
}

// Set envid's trap frame to 'tf'.
// tf is modified to make sure that user environments always run at code
// protection level 3 (CPL 3), interrupts enabled, and IOPL of 0.
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
static int
sys_env_set_trapframe(envid_t envid, struct Trapframe *tf)
{
	// Remember to check whether the user has supplied us with a good
	// address!
    struct Env* e;
    if(envid2env(envid, &e, 1) < 0)
        return -E_BAD_ENV;
    user_mem_assert(e, tf, sizeof(struct Trapframe), PTE_P|PTE_U|PTE_W);
    tf -> tf_eflags |= FL_IF;
    tf -> tf_eflags &= ~FL_IOPL_MASK;
    tf -> tf_cs = GD_UT | 3;
    memmove(&(e -> env_tf), tf, sizeof(struct Trapframe));
    return 0;
}

// Set the page fault upcall for 'envid' by modifying the corresponding struct
// Env's 'env_pgfault_upcall' field.  When 'envid' causes a page fault, the
// kernel will push a fault record onto the exception stack, then branch to
// 'func'.
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
static int
sys_env_set_pgfault_upcall(envid_t envid, void *func)
{
    struct Env* env;
    if(envid2env(envid, &env, 1) < 0)
        return -E_BAD_ENV;
    env -> env_pgfault_upcall = func;
    return 0;
}

// Allocate a page of memory and map it at 'va' with permission
// 'perm' in the address space of 'envid'.
// The page's contents are set to 0.
// If a page is already mapped at 'va', that page is unmapped as a
// side effect.
//
// perm -- PTE_U | PTE_P must be set, PTE_AVAIL | PTE_W may or may not be set,
//         but no other bits may be set.  See PTE_SYSCALL in inc/mmu.h.
//
// Return 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
//	-E_INVAL if va >= UTOP, or va is not page-aligned.
//	-E_INVAL if perm is inappropriate (see above).
//	-E_NO_MEM if there's no memory to allocate the new page,
//		or to allocate any necessary page tables.
static int
sys_page_alloc(envid_t envid, void *va, int perm)
{
	// Hint: This function is a wrapper around page_alloc() and
	//   page_insert() from kern/pmap.c.
	//   Most of the new code you write should be to check the
	//   parameters for correctness.
	//   If page_insert() fails, remember to free the page you
	//   allocated!
    if((uint32_t)va >= UTOP || (uint32_t)va % PGSIZE != 0)
        return -E_INVAL;
    if((perm & (PTE_U|PTE_P)) != (PTE_U|PTE_P))
        return -E_INVAL;
    if((perm & (~PTE_U) & (~PTE_P) & (~PTE_AVAIL) & (~PTE_W)))
        return -E_INVAL;
    struct Env* env;
    if(envid2env(envid, &env, 1) < 0)
        return -E_BAD_ENV;
    struct PageInfo* page = page_alloc(ALLOC_ZERO);
    if(!page)
        return -E_NO_MEM;
    if(page_insert(env -> env_pgdir, page, va, perm) < 0){
        page_decref(page);
        return -E_NO_MEM;
    }
    return 0;

}

// Map the page of memory at 'srcva' in srcenvid's address space
// at 'dstva' in dstenvid's address space with permission 'perm'.
// Perm has the same restrictions as in sys_page_alloc, except
// that it also must not grant write access to a read-only
// page.
//
// Return 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if srcenvid and/or dstenvid doesn't currently exist,
//		or the caller doesn't have permission to change one of them.
//	-E_INVAL if srcva >= UTOP or srcva is not page-aligned,
//		or dstva >= UTOP or dstva is not page-aligned.
//	-E_INVAL is srcva is not mapped in srcenvid's address space.
//	-E_INVAL if perm is inappropriate (see sys_page_alloc).
//	-E_INVAL if (perm & PTE_W), but srcva is read-only in srcenvid's
//		address space.
//	-E_NO_MEM if there's no memory to allocate any necessary page tables.
static int
sys_page_map(envid_t srcenvid, void *srcva,
	     envid_t dstenvid, void *dstva, int perm)
{
	// Hint: This function is a wrapper around page_lookup() and
	//   page_insert() from kern/pmap.c.
	//   Again, most of the new code you write should be to check the
	//   parameters for correctness.
	//   Use the third argument to page_lookup() to
	//   check the current permissions on the page.

    if((uint32_t)srcva >= UTOP || (uint32_t)dstva >= UTOP || (uint32_t)srcva % PGSIZE != 0 || (uint32_t)dstva % PGSIZE != 0)
        return -E_INVAL;
    if((perm & (PTE_U|PTE_P)) != (PTE_U|PTE_P))
        return -E_INVAL;
    if((perm & (~PTE_U) & (~PTE_P) & (~PTE_AVAIL) & (~PTE_W)))
        return -E_INVAL;
    struct Env* srcenv;
    struct Env* dstenv;
    if(envid2env(srcenvid, &srcenv, 1)
        || envid2env(dstenvid, &dstenv, 1))
        return -E_BAD_ENV;
    pte_t* srcpte;
    struct PageInfo* page = page_lookup(srcenv -> env_pgdir, srcva, &srcpte);
    if(!page)
        return -E_INVAL;
    if((perm & PTE_W) && (!(*srcpte & PTE_W)))
        return -E_INVAL;
    return page_insert(dstenv -> env_pgdir, page, dstva, perm);
}

// Unmap the page of memory at 'va' in the address space of 'envid'.
// If no page is mapped, the function silently succeeds.
//
// Return 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
//	-E_INVAL if va >= UTOP, or va is not page-aligned.
static int
sys_page_unmap(envid_t envid, void *va)
{
	// Hint: This function is a wrapper around page_remove().

    if((uint32_t)va >= UTOP || (uint32_t)va % PGSIZE != 0)
        return -E_INVAL;
    struct Env* env;
    if(envid2env(envid, &env, 1) < 0)
        return -E_BAD_ENV;
    page_remove(env -> env_pgdir, va);
    return 0;
}

// Try to send 'value' to the target env 'envid'.
// If srcva < UTOP, then also send page currently mapped at 'srcva',
// so that receiver gets a duplicate mapping of the same page.
//
// The send fails with a return value of -E_IPC_NOT_RECV if the
// target is not blocked, waiting for an IPC.
//
// The send also can fail for the other reasons listed below.
//
// Otherwise, the send succeeds, and the target's ipc fields are
// updated as follows:
//    env_ipc_recving is set to 0 to block future sends;
//    env_ipc_from is set to the sending envid;
//    env_ipc_value is set to the 'value' parameter;
//    env_ipc_perm is set to 'perm' if a page was transferred, 0 otherwise.
// The target environment is marked runnable again, returning 0
// from the paused sys_ipc_recv system call.  (Hint: does the
// sys_ipc_recv function ever actually return?)
//
// If the sender wants to send a page but the receiver isn't asking for one,
// then no page mapping is transferred, but no error occurs.
// The ipc only happens when no errors occur.
//
// Returns 0 on success, < 0 on error.
// Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist.
//		(No need to check permissions.)
//	-E_IPC_NOT_RECV if envid is not currently blocked in sys_ipc_recv,
//		or another environment managed to send first.
//	-E_INVAL if srcva < UTOP but srcva is not page-aligned.
//	-E_INVAL if srcva < UTOP and perm is inappropriate
//		(see sys_page_alloc).
//	-E_INVAL if srcva < UTOP but srcva is not mapped in the caller's
//		address space.
//	-E_INVAL if (perm & PTE_W), but srcva is read-only in the
//		current environment's address space.
//	-E_NO_MEM if there's not enough memory to map srcva in envid's
//		address space.
static int
sys_ipc_try_send(envid_t envid, uint32_t value, void *srcva, unsigned perm)
{
    //first check target env
    struct Env* dstenv;
    if(envid2env(envid, &dstenv, 0) < 0)
        return -E_BAD_ENV;

    //check dstenv's status
    if(!dstenv -> env_ipc_recving)
        return -E_IPC_NOT_RECV;

    //check srcva itself
    int send_page = 0;
    if((uint32_t)srcva < UTOP){
        //ready to send
        if((uint32_t)srcva % PGSIZE)
            //not page-aligned
            return -E_INVAL;
        //check src page-mapping
        pte_t* pte;
        struct PageInfo* page = page_lookup(curenv -> env_pgdir, srcva, &pte);
        if(!page)
            //not mapped
            return -E_INVAL;
        if((perm & PTE_W) && (!(*pte & PTE_W)))
            //map RO page
            return -E_INVAL;
        //check perm itself
        if((perm & (PTE_U|PTE_P)) != (PTE_U|PTE_P))
            //perm1 failed
            return -E_INVAL;
        if(perm & (~PTE_W) & (~PTE_U) & (~PTE_P) & (~PTE_AVAIL))
            //perm2 failed
            return -E_INVAL;
        //srcva itself passed, check for dstva
        if((uint32_t)(dstenv -> env_ipc_dstva) < UTOP)
            //dst request page mapping
            send_page = 1;
    }

    //install data to dst
    if(send_page){
        struct PageInfo* page = page_lookup(curenv -> env_pgdir, srcva, 0);
        assert(page);
        if(page_insert(dstenv -> env_pgdir, page, dstenv -> env_ipc_dstva, perm) < 0)
            return -E_NO_MEM;
    }
    //page mapped, continue installing
    dstenv -> env_ipc_value = value;
    dstenv -> env_ipc_from = curenv -> env_id;
    dstenv -> env_ipc_perm = perm;
    //responsible for receiver's return value
    dstenv -> env_tf.tf_regs.reg_eax = 0;
    dstenv -> env_ipc_recving = 0;

    //enable scheduling
    dstenv -> env_status = ENV_RUNNABLE;
    return 0;
}

// Block until a value is ready.  Record that you want to receive
// using the env_ipc_recving and env_ipc_dstva fields of struct Env,
// mark yourself not runnable, and then give up the CPU.
//
// If 'dstva' is < UTOP, then you are willing to receive a page of data.
// 'dstva' is the virtual address at which the sent page should be mapped.
//
// This function only returns on error, but the system call will eventually
// return 0 on success.
// Return < 0 on error.  Errors are:
//	-E_INVAL if dstva < UTOP but dstva is not page-aligned.
static int
sys_ipc_recv(void *dstva)
{
    //process dstva
    if((uint32_t)dstva < UTOP && (uint32_t)dstva % PGSIZE)
        return -E_INVAL;

    //install data, as syscall is marked as interrupt, it cam never be interrupted again
    curenv -> env_ipc_dstva = dstva;
    curenv -> env_ipc_recving = 1;
    curenv -> env_status = ENV_NOT_RUNNABLE;
    curenv -> env_ipc_perm = 0;
    curenv -> env_ipc_from = curenv -> env_id;

    //waiting for message, yield the CPU
    sched_yield();

    //Now message received, but it is impossible to reach here.
    //sched_yield() will call env_run(), finally execute 'iret'.
    //but the stack's is when it calls syscall, so the program will
    //return to the library call-point directly.
    //the return value need to be set by sender
    return 0;
}

// Return the current time.
static int
sys_time_msec(void)
{
    return time_msec();
}

static int
sys_nic_transmit(const void* packet, int size){
    user_mem_assert(curenv, packet, size, PTE_U);
    return e1000_82540em_send(packet, size);
}

static int
sys_nic_recv(void* buf, int limit){
    user_mem_assert(curenv, buf, limit, PTE_U);
    return e1000_82540em_recv(buf, limit);
}

// Dispatches to the correct kernel function, passing the arguments.
int32_t
syscall(uint32_t syscallno, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
	// Call the function corresponding to the 'syscallno' parameter.
	// Return any appropriate return value.


	switch (syscallno) {
    case SYS_cputs:
        sys_cputs((char*)a1, (size_t)a2);
        return 0;
    case SYS_cgetc:
        return sys_cgetc();
    case SYS_getenvid:
        return sys_getenvid();
    case SYS_env_destroy:
        return sys_env_destroy((int)a1);
    case SYS_exofork:
        return sys_exofork();
    case SYS_env_set_status:
        return sys_env_set_status((envid_t)a1, (int)a2);
    case SYS_page_alloc:
        return sys_page_alloc((envid_t)a1, (void*)a2, (int)a3);
    case SYS_page_map:
        return sys_page_map((int)a1, (void*)a2, (int)a3, (void*)a4, (int)a5);
    case SYS_page_unmap:
        return sys_page_unmap((int)a1, (void*)a2);
    case SYS_env_set_pgfault_upcall:
        return sys_env_set_pgfault_upcall((envid_t)a1, (void*)a2);
    case SYS_yield:
        sched_yield();
        return 0;
    case SYS_ipc_try_send:
        return sys_ipc_try_send((int)a1, (int)a2, (void*)a3, (unsigned int)a4);
    case SYS_ipc_recv:
        return sys_ipc_recv((void*)a1);
    case SYS_env_set_trapframe:
        return sys_env_set_trapframe((int)a1, (struct Trapframe*)a2);
    case SYS_time_msec:
        return sys_time_msec();
    case SYS_nic_transmit:
        return sys_nic_transmit((const void*)a1, (int)a2);
    case SYS_nic_recv:
        return sys_nic_recv((void*)a1, (int)a2);
	default:
		return -E_INVAL;
	}
}

