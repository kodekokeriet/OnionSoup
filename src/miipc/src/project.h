/* Project files (.mpj): plain text, key=value, easy to read and diff. */
#ifndef PROJECT_H
#define PROJECT_H
#include "engine.h"

#define PROJECT_DIR "projects"
#define AUTOSAVE PROJECT_DIR "/_autosave.mpj"

int project_save(const Engine *e, const char *path, const char *name);
/* name_out (may be NULL) receives the name stored in the file. */
int project_load(Engine *e, const char *path, char *name_out, int name_len);
/* Next free "projects/project_NNN.mpj". */
void project_next_path(char *out, int len);

#endif
