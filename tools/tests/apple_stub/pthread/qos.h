#pragma once
typedef unsigned int qos_class_t;
#define QOS_CLASS_USER_INTERACTIVE 0x21
#define QOS_CLASS_USER_INITIATED 0x19
int pthread_set_qos_class_self_np(qos_class_t, int);
