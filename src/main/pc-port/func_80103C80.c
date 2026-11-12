/* func_80103C80 — PC port stub, Address: 0x80103C80
 * TODO: Implement from disassembly
 */
#include "types.h"

#ifdef PC_PORT
#include <stdio.h>
#include "pc_debug.h"
#endif

s32 func_80103C80(void)
{
#ifdef PC_PORT
    /* PC_DEBUG("STUB: func_80103C80 called\n"); */
#endif
    return 0;
}
