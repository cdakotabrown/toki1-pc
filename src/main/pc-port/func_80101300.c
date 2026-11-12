/* func_80101300 — PC port stub, Address: 0x80101300
 * TODO: Implement from disassembly
 */
#include "types.h"

#ifdef PC_PORT
#include <stdio.h>
#include "pc_debug.h"
#endif

s32 func_80101300(void)
{
#ifdef PC_PORT
    /* PC_DEBUG("STUB: func_80101300 called\n"); */
#endif
    return 0;
}
