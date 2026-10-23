/* bionic ucontext accessors for the ported host (Darwin keeps its nested
 * mcontext pointer; the staged Android sources use these macros). */
#pragma once
#include <ucontext.h>

#if defined(__aarch64__)
#define DM_UC_PC(uc) ((uint64_t)(uc)->uc_mcontext.pc)
#define DM_UC_LR(uc) ((uint64_t)(uc)->uc_mcontext.regs[30])
#define DM_UC_SP(uc) ((uint64_t)(uc)->uc_mcontext.sp)
#define DM_UC_FP(uc) ((uint64_t)(uc)->uc_mcontext.regs[29])
#define DM_UC_X(uc, i) ((uint64_t)(uc)->uc_mcontext.regs[i])
#elif defined(__x86_64__)
#include <sys/ucontext.h>
#define DM_UC_PC(uc) ((uint64_t)(uc)->uc_mcontext.gregs[REG_RIP])
#define DM_UC_LR(uc) ((uint64_t)0)
#define DM_UC_SP(uc) ((uint64_t)(uc)->uc_mcontext.gregs[REG_RSP])
#define DM_UC_FP(uc) ((uint64_t)(uc)->uc_mcontext.gregs[REG_RBP])
#define DM_UC_X(uc, i) ((i) >= 0 && (i) < NGREG ? (uint64_t)(uc)->uc_mcontext.gregs[i] : 0)
#else
#define DM_UC_PC(uc) ((uint64_t)0)
#define DM_UC_LR(uc) ((uint64_t)0)
#define DM_UC_SP(uc) ((uint64_t)0)
#define DM_UC_FP(uc) ((uint64_t)0)
#define DM_UC_X(uc, i) ((uint64_t)0)
#endif
