/* Undo / redo: snapshots of the editable project state (pads, sequences,
 * song, mixer/FX, tempo). Sample audio itself is not part of a snapshot. */
#ifndef UNDO_H
#define UNDO_H
#include "engine.h"

/* Call before an edit. Edits with the same tag within ~1.5 s share one step
 * (e.g. holding Right on a fader). */
void undo_mark(Engine *e, const char *tag);
int undo_undo(Engine *e); /* 0 = nothing to undo */
int undo_redo(Engine *e);
void undo_clear(void);
int undo_pos(void); /* for "Undo 3/7" */
int undo_count(void);
int undo_dirty(void); /* edits since last undo_saved() */
void undo_saved(void);

#endif
