/* Apple QoS scheduling classes: Android applications cannot raise their own
 * scheduling priority, so the host's Horizon-priority emulation degrades to a
 * no-op here (see docs/android-port-feasibility.md, risk R3). */
#pragma once

typedef enum {
    QOS_CLASS_USER_INTERACTIVE = 0x21,
    QOS_CLASS_USER_INITIATED = 0x19,
    QOS_CLASS_DEFAULT = 0x15,
    QOS_CLASS_UTILITY = 0x11,
    QOS_CLASS_BACKGROUND = 0x09,
    QOS_CLASS_UNSPECIFIED = 0x00
} qos_class_t;

int pthread_set_qos_class_self_np(qos_class_t cls, int relative_priority);
int pthread_get_qos_class_np(pthread_t thread, qos_class_t *cls, int *relative_priority);
typedef void *pthread_override_t;
pthread_override_t pthread_override_qos_class_start_np(pthread_t thread, qos_class_t cls, int relative_priority);
int pthread_override_qos_class_end_np(pthread_override_t override);
