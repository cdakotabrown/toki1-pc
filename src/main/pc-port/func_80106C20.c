/* func_80106C20 — PC port stub, Address: 0x80106C20
 * TODO: Implement from disassembly
 */
#include "types.h"

#ifdef PC_PORT
#include <stdio.h>
#include "pc_debug.h"
#endif

s32 func_80106C20(void)
{
#ifdef PC_PORT
    /* PC_DEBUG("STUB: func_80106C20 called\n"); */
#endif
    return 0;
}
