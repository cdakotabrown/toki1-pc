/* func_8010AFE4 — PC port stub, Address: 0x8010AFE4
 * TODO: Implement from disassembly
 */
#include "types.h"

#ifdef PC_PORT
#include <stdio.h>
#include "pc_debug.h"
#endif

s32 func_8010AFE4(void)
{
#ifdef PC_PORT
    /* PC_DEBUG("STUB: func_8010AFE4 called\n"); */
#endif
    return 0;
}
