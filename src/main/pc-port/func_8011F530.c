/* func_8011F530 — PC port stub, Address: 0x8011F530
 * TODO: Implement from disassembly
 */
#include "types.h"

#ifdef PC_PORT
#include <stdio.h>
#include "pc_debug.h"
#endif

s32 func_8011F530(void)
{
#ifdef PC_PORT
    /* PC_DEBUG("STUB: func_8011F530 called\n"); */
#endif
    return 0;
}
