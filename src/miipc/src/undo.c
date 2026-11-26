#include "undo.h"
#include <SDL.h>
#include <stdlib.h>
#include <string.h>

#define UNDO_LEVELS 12

typedef struct {
    Pad pads[NUM_PADS];
    Sequence seqs[MAX_SEQS];
    Song song;
    FxParams fx;
    int bpm;
    float swing;
} Snap;

static Snap *ring[UNDO_LEVELS + 1];
static int count; /* snapshots stored before 'cur' */
static int redo_avail;
static int cur; /* ring index where the next snapshot goes */
static const char *last_tag;
static Uint32 last_time;
static int dirty;

static void take(Engine *e, Snap *s)
{
    engine_lock();
    memcpy(s->pads, e->pads, sizeof s->pads);
    memcpy(s->seqs, e->seqs, sizeof s->seqs);
    s->song = e->song;
    s->fx = e->fx;
    s->bpm = e->bpm;
    s->swing = e->swing;
    engine_unlock();
}

static void put(Engine *e, const Snap *s)
{
    engine_lock();
    memcpy(e->pads, s->pads, sizeof s->pads);
    memcpy(e->seqs, s->seqs, sizeof s->seqs);
    e->song = s->song;
    e->fx = s->fx;
    e->bpm = s->bpm;
    e->swing = s->swing;
    /* a sample that was unloaded since cannot come back */
    for (int p = 0; p < NUM_PADS; p++)
        if (e->pads[p].sample >= 0 && !e->pool[e->pads[p].sample].data)
            e->pads[p].sample = -1;
    engine_unlock();
    engine_resync(e);
}

static Snap *slot(int i)
{
    i = (i % (UNDO_LEVELS + 1) + UNDO_LEVELS + 1) % (UNDO_LEVELS + 1);
    if (!ring[i])
        ring[i] = malloc(sizeof(Snap));
    return ring[i];
}

void undo_mark(Engine *e, const char *tag)
{
    Uint32 now = SDL_GetTicks();
    dirty = 1;
    if (tag && last_tag == tag && now - last_time < 1500) {
        last_time = now;
        return;
    }
    last_tag = tag;
    last_time = now;
    Snap *s = slot(cur);
    if (!s)
        return;
    take(e, s);
    cur = (cur + 1) % (UNDO_LEVELS + 1);
    if (count < UNDO_LEVELS)
        count++;
    redo_avail = 0;
}

int undo_undo(Engine *e)
{
    if (!count)
        return 0;
    /* save the present so redo can return to it */
    Snap *here = slot(cur);
    if (!here)
        return 0;
    take(e, here);
    cur = (cur + UNDO_LEVELS) % (UNDO_LEVELS + 1);
    put(e, slot(cur));
    count--;
    redo_avail++;
    last_tag = NULL;
    dirty = 1;
    return 1;
}

int undo_redo(Engine *e)
{
    if (!redo_avail)
        return 0;
    cur = (cur + 1) % (UNDO_LEVELS + 1);
    put(e, slot(cur));
    count++;
    redo_avail--;
    last_tag = NULL;
    dirty = 1;
    return 1;
}

void undo_clear(void)
{
    count = 0;
    redo_avail = 0;
    last_tag = NULL;
}
int undo_pos(void) { return count; }
int undo_count(void) { return count + redo_avail; }
int undo_dirty(void) { return dirty; }
void undo_saved(void) { dirty = 0; }
