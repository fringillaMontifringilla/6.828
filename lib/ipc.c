// User-level IPC library routines

#include <inc/lib.h>

// Receive a value via IPC and return it.
// If 'pg' is nonnull, then any page sent by the sender will be mapped at
//	that address.
// If 'from_env_store' is nonnull, then store the IPC sender's envid in
//	*from_env_store.
// If 'perm_store' is nonnull, then store the IPC sender's page permission
//	in *perm_store (this is nonzero iff a page was successfully
//	transferred to 'pg').
// If the system call fails, then store 0 in *fromenv and *perm (if
//	they're nonnull) and return the error.
// Otherwise, return the value sent by the sender
//
// Hint:
//   Use 'thisenv' to discover the value and who sent it.
//   If 'pg' is null, pass sys_ipc_recv a value that it will understand
//   as meaning "no page".  (Zero is not the right value, since that's
//   a perfectly valid place to map a page.)
int32_t
ipc_recv(envid_t *from_env_store, void *pg, int *perm_store)
{
    if(!pg)
        pg = UTOP;
    envid_t envid = 0;
    int perm = 0;
    int value;
    if(value=sys_ipc_recv(pg), !value){
        envid = thisenv -> env_ipc_from;
        perm = thisenv -> env_ipc_perm;
        value = thisenv -> env_ipc_value;
    }
    if(from_env_store)
        *from_env_store = envid;
    if(perm_store)
        *perm_store = perm;
    return value;
}

// Send 'val' (and 'pg' with 'perm', if 'pg' is nonnull) to 'toenv'.
// This function keeps trying until it succeeds.
// It should panic() on any error other than -E_IPC_NOT_RECV.
//
// Hint:
//   Use sys_yield() to be CPU-friendly.
//   If 'pg' is null, pass sys_ipc_try_send a value that it will understand
//   as meaning "no page".  (Zero is not the right value.)
void
ipc_send(envid_t to_env, uint32_t val, void *pg, int perm)
{
    if(!pg)
        pg = UTOP;
    int value;
    do{
        value = sys_ipc_try_send(to_env, val, pg, perm);
        if(!value)
            return;
        if(value != -E_IPC_NOT_RECV)
            panic("ipc send panic, %08x to %08x, err:%d\n, val:%08x, page:%s(%08x), perm:%s(%08x[%c%cr%c%c%c])\n",
                    thisenv -> env_id, to_env,
                    value,
                    val,
                    (pg == UTOP)?"NOT ENABLED":"ENABLED", pg,
                    (pg == UTOP)?"NOT ENABLED":"ENABLED", perm,
                    (perm & PTE_U)?'u':'-',
                    (perm & PTE_W)?'w':'-',
                    (perm & PTE_AVAIL)?'a':'-',
                    (perm & PTE_P)?'p':'-',
                    (perm & (~PTE_U) & (~PTE_W) & (~PTE_AVAIL) & (~PTE_P))?'o':'-');
        sys_yield();
    }while(value);
}

// Find the first environment of the given type.  We'll use this to
// find special environments.
// Returns 0 if no such environment exists.
envid_t
ipc_find_env(enum EnvType type)
{
	int i;
	for (i = 0; i < NENV; i++)
		if (envs[i].env_type == type)
			return envs[i].env_id;
	return 0;
}
