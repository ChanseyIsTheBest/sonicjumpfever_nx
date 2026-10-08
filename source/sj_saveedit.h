/* sj_saveedit.h -- edit profile0.dat at boot from save_edit.txt (see sj_saveedit.c).
 * MIT licensed, see LICENSE. */
#ifndef SJ_SAVEEDIT_H
#define SJ_SAVEEDIT_H

/* Portable core (tested on a PC): 1 = save changed, 0 = nothing to do, -1 = refused.
 * contents_path may be NULL: no save_contents.txt listing is written. */
int sj_saveedit_run_paths(const char *save_path, const char *edit_path,
                          const char *contents_path);

/* Switch entry point: <game folder>/save_edit.txt -> <game folder>/profile0.dat,
 * listing in <game folder>/save_contents.txt (SJ_SAVE_EDIT). Call before the
 * game loads its profile. */
void sj_saveedit_run(void);

#endif /* SJ_SAVEEDIT_H */
