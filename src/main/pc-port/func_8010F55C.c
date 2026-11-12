/* func_8010F55C — PC port stub, Address: 0x8010F55C
 * TODO: Implement from disassembly
 */
#include "types.h"

#ifdef PC_PORT
#include <stdio.h>
#include "pc_debug.h"
#endif

s32 func_8010F55C(void)
{
#ifdef PC_PORT
    /* PC_DEBUG("STUB: func_8010F55C called\n"); */
#endif
    return 0;
}
