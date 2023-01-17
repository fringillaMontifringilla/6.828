// implement fork from user space

#include <inc/string.h>
#include <inc/lib.h>

// PTE_COW marks copy-on-write page table entries.
// It is one of the bits explicitly allocated to user processes (PTE_AVAIL).
#define PTE_COW		0x800

//
// Custom page fault handler - if faulting page is copy-on-write,
// map in our own private writable copy.
//
static void
pgfault(struct UTrapframe *utf)
{
	void *addr = (void *) utf->utf_fault_va;
	uint32_t err = utf->utf_err;
	int r;

	// Check that the faulting access was (1) a write, and (2) to a
	// copy-on-write page.  If not, panic.
	// Hint:
	//   Use the read-only page table mappings at uvpt
	//   (see <inc/memlayout.h>).
    addr = ROUNDDOWN(addr, PGSIZE);
    pte_t pte = uvpt[PGNUM(addr)];
    if(!(err & FEC_WR) || !(pte & PTE_COW))
        panic("pgfault at not cow-page, va:%08x\n", addr);

	// Allocate a new page, map it at a temporary location (PFTEMP),
	// copy the data from the old page to the new page, then move the new
	// page to the old page's address.
	// Hint:
	//   You should make three system calls.
    r = sys_page_alloc(0, PFTEMP, PTE_U|PTE_W|PTE_P);
    if(r < 0)
        panic("cow failed, va:%08x\n", addr);
    memmove(PFTEMP, addr, PGSIZE);
    r = sys_page_map(0, PFTEMP, 0, addr, PTE_U|PTE_W|PTE_P);
    if(r < 0)
        panic("cow failed, va:%08x\n", addr);
    r = sys_page_unmap(0, PFTEMP);
    if(r < 0)
        panic("cow failed, va:%08x\n", addr);
}

//
// Map our virtual page pn (address pn*PGSIZE) into the target envid
// at the same virtual address.  If the page is writable or copy-on-write,
// the new mapping must be created copy-on-write, and then our mapping must be
// marked copy-on-write as well.  (Exercise: Why do we need to mark ours
// copy-on-write again if it was already copy-on-write at the beginning of
// this function?)
//
// Returns: 0 on success, < 0 on error.
// It is also OK to panic on error.
//
static int
duppage(envid_t envid, unsigned pn)
{
	int r;
    pte_t pte = uvpt[pn];
    int perm = PTE_P|PTE_U;
    if(pte & PTE_SHARE)
        return sys_page_map(0, pn*PGSIZE, envid, pn*PGSIZE, pte & PTE_SYSCALL);
    if((pte & PTE_W) || (pte & PTE_COW))
        perm |= PTE_COW;
    r = sys_page_map(0, pn*PGSIZE, envid, pn*PGSIZE, perm);
    if(r < 0)
        return r;
    if(perm & PTE_COW)
        if(sys_page_map(0, pn*PGSIZE, 0, pn*PGSIZE, perm))
            sys_page_unmap(envid, pn*PGSIZE);
	return r;
}

//
// User-level fork with copy-on-write.
// Set up our page fault handler appropriately.
// Create a child.
// Copy our address space and page fault handler setup to the child.
// Then mark the child as runnable and return.
//
// Returns: child's envid to the parent, 0 to the child, < 0 on error.
// It is also OK to panic on error.
//
// Hint:
//   Use uvpd, uvpt, and duppage.
//   Remember to fix "thisenv" in the child process.
//   Neither user exception stack should ever be marked copy-on-write,
//   so you must allocate a new page for the child's user exception stack.
//
envid_t
fork(void)
{
    set_pgfault_handler(pgfault);
    envid_t child = sys_exofork();
    if(child < 0)
        return child;
    if(!child){
        thisenv = &envs[ENVX(sys_getenvid())];
        return 0;
    }
    uint32_t i = 0;
    for(;i < USTACKTOP;i+=PGSIZE){
        if(uvpd[PDX(i)] & PTE_P){
            if(uvpt[PGNUM(i)] & PTE_P){
                if(duppage(child, i/PGSIZE) < 0)
                    break;
            }
        }
    }
    if(i != USTACKTOP){
        //error occurs, need to rollback
        for(uint32_t j = 0;j < i;j+=PGSIZE)
            sys_page_unmap(child, j);
        sys_env_destroy(child);
        return -1;
    }
    extern void _pgfault_upcall(void);
    //now exception stack and final status
    if(sys_page_alloc(child, UXSTACKTOP - PGSIZE, PTE_U|PTE_W|PTE_P)
        || sys_env_set_pgfault_upcall(child, _pgfault_upcall)
        || sys_env_set_status(child, ENV_RUNNABLE)){
        for(i = 0;i < USTACKTOP;i+=PGSIZE)
            sys_page_unmap(child, i);
        sys_env_destroy(child);
        return -1;
    }
    return child;
}

// Challenge!
int
sfork(void)
{
	panic("sfork not implemented");
	return -E_INVAL;
}
