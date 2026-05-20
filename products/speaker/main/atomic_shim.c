#include <stdbool.h>
#include <stdint.h>

__attribute__((used)) bool __atomic_test_and_set(volatile void *ptr, int memorder)
{
    volatile uint8_t *p = (volatile uint8_t *)ptr;
    bool old = *p;
    *p = 1;
    return old;
}
