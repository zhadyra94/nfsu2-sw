#pragma once
/* Apple libc extras that SDL headers call inline under __APPLE__. */
#include <stddef.h>
void memset_pattern4(void *b, const void *pattern4, size_t len);
