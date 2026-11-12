/* func_800FF320 — PC port stub, Address: 0x800FF320
 * TODO: Implement from disassembly
 */
#include "types.h"

#ifdef PC_PORT
#include <stdio.h>
#include "pc_debug.h"
#endif

s32 func_800FF320(void)
{
#ifdef PC_PORT
    /* PC_DEBUG("STUB: func_800FF320 called\n"); */
#endif
    return 0;
}
