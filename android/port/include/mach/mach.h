/* Minimal Mach API surface used by the nxnative host, implemented on Android. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

typedef unsigned int mach_port_t;
typedef unsigned int mach_msg_type_number_t;
typedef unsigned int natural_t;
typedef unsigned int integer_t;
typedef int kern_return_t;
typedef unsigned int boolean_t;
typedef uintptr_t vm_address_t;
typedef uintptr_t vm_size_t;
typedef unsigned int vm_prot_t;
typedef mach_port_t thread_act_t;
typedef thread_act_t *thread_act_array_t;
typedef mach_port_t thread_t;
typedef int thread_state_flavor_t;
typedef integer_t *thread_state_t;
typedef integer_t *thread_info_t;
typedef integer_t *vm_region_info_t;
typedef unsigned int vm_region_flavor_t;
typedef unsigned int vm_inherit_t;
typedef unsigned int vm_behavior_t;

#define KERN_SUCCESS 0
#define KERN_INVALID_ADDRESS 1
#define KERN_FAILURE 5
#define KERN_PROTECTION_FAILURE 2

#define TRUE 1
#define FALSE 0

#define VM_PROT_NONE 0x00
#define VM_PROT_READ 0x01
#define VM_PROT_WRITE 0x02
#define VM_PROT_EXECUTE 0x04
#define VM_INHERIT_NONE 2
#define VM_FLAGS_ANYWHERE 0x0001
#define VM_FLAGS_FIXED 0x0000
#define VM_FLAGS_OVERWRITE 0x4000

#define THREAD_BASIC_INFO 3
#define THREAD_BASIC_INFO_COUNT ((mach_msg_type_number_t)(sizeof(thread_basic_info_data_t) / sizeof(natural_t)))
#define TH_STATE_RUNNING 1
#define TH_STATE_STOPPED 2
#define TH_STATE_WAITING 3
#define TH_STATE_UNINTERRUPTIBLE 4
#define TH_STATE_HALTED 5

typedef struct { integer_t seconds; integer_t microseconds; } time_value_t;
typedef struct {
    integer_t cpu_usage;
    integer_t policy;
    integer_t run_state;
    integer_t flags;
    integer_t suspend_count;
    integer_t sleep_time;
    time_value_t user_time;
    time_value_t system_time;
} thread_basic_info_data_t;

/* arm_thread_state64_t layout mirrors Darwin enough for the diagnostic code
 * paths (which are compiled but never receive a state on Android). */
typedef struct {
    uint64_t __x[29];
    uint64_t __fp;
    uint64_t __lr;
    uint64_t __sp;
    uint64_t __pc;
    uint32_t __cpsr;
    uint32_t __pad;
} arm_thread_state64_t;

#define ARM_THREAD_STATE64 6
#define ARM_THREAD_STATE64_COUNT ((mach_msg_type_number_t)(sizeof(arm_thread_state64_t) / sizeof(natural_t)))

/* Darwin's accessors take the state by value. */
#define __darwin_arm_thread_state64_get_pc(s) ((s).__pc)
#define __darwin_arm_thread_state64_get_fp(s) ((s).__fp)
#define __darwin_arm_thread_state64_get_lr(s) ((s).__lr)
#define __darwin_arm_thread_state64_get_sp(s) ((s).__sp)

mach_port_t mach_task_self(void);
mach_port_t mach_thread_self(void);
kern_return_t mach_port_deallocate(mach_port_t task, mach_port_t name);
kern_return_t vm_read_overwrite(mach_port_t task, vm_address_t address, vm_size_t size, vm_address_t data, vm_size_t *out_size);
kern_return_t vm_allocate(mach_port_t task, vm_address_t *address, vm_size_t size, int flags);
kern_return_t vm_deallocate(mach_port_t task, vm_address_t address, vm_size_t size);
kern_return_t vm_remap(mach_port_t target, vm_address_t *address, vm_size_t size, vm_size_t mask, int flags,
                       mach_port_t src, vm_address_t src_addr, boolean_t copy, vm_prot_t *cur, vm_prot_t *max,
                       vm_inherit_t inherit);
kern_return_t vm_region(vm_address_t *address, vm_size_t *size, vm_region_flavor_t flavor,
                        vm_region_info_t info, mach_msg_type_number_t *count, mach_port_t *object);
kern_return_t thread_info(thread_act_t thread, thread_state_flavor_t flavor, thread_info_t info, mach_msg_type_number_t *count);
kern_return_t thread_get_state(thread_act_t thread, thread_state_flavor_t flavor, thread_state_t state, mach_msg_type_number_t *count);
kern_return_t thread_suspend(thread_act_t thread);
kern_return_t thread_resume(thread_act_t thread);
mach_port_t pthread_mach_thread_np(pthread_t thread);
kern_return_t task_threads(mach_port_t task, thread_act_array_t *list, mach_msg_type_number_t *count);
