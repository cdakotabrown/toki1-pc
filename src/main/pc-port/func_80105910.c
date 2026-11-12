/* func_80105910 — PC port stub, Address: 0x80105910
 * TODO: Implement from disassembly
 */
#include "types.h"

#ifdef PC_PORT
#include <stdio.h>
#include "pc_debug.h"
#endif

s32 func_80105910(void)
{
#ifdef PC_PORT
    /* PC_DEBUG("STUB: func_80105910 called\n"); */
#endif
    return 0;
}
