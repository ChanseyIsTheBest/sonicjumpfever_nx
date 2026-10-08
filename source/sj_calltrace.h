/* sj_calltrace.h -- log the first call the game makes to each import
 * (sj_calltrace.c). MIT licensed, see LICENSE. */
#ifndef SJ_CALLTRACE_H
#define SJ_CALLTRACE_H

#include "so_util.h"

/* After resolve_imports and before so_finalize: points every PLT slot at a
 * logging stub. Returns the number of imports traced. */
int sj_calltrace_install(so_module *mod);

#endif
