/* func_8010B360 — PC port stub, Address: 0x8010B360
 * TODO: Implement from disassembly
 */
#include "types.h"

#ifdef PC_PORT
#include <stdio.h>
#include "pc_debug.h"
#endif

s32 func_8010B360(void)
{
#ifdef PC_PORT
    /* PC_DEBUG("STUB: func_8010B360 called\n"); */
#endif
    return 0;
}
