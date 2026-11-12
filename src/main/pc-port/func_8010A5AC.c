/* func_8010A5AC — PC port stub, Address: 0x8010A5AC
 * TODO: Implement from disassembly
 */
#include "types.h"

#ifdef PC_PORT
#include <stdio.h>
#include "pc_debug.h"
#endif

s32 func_8010A5AC(void)
{
#ifdef PC_PORT
    /* PC_DEBUG("STUB: func_8010A5AC called\n"); */
#endif
    return 0;
}
