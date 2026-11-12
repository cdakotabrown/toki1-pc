/* func_80106E30 — PC port stub, Address: 0x80106E30
 * TODO: Implement from disassembly
 */
#include "types.h"

#ifdef PC_PORT
#include <stdio.h>
#include "pc_debug.h"
#endif

s32 func_80106E30(void)
{
#ifdef PC_PORT
    /* PC_DEBUG("STUB: func_80106E30 called\n"); */
#endif
    return 0;
}
