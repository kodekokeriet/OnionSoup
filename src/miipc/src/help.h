/* On-screen help: key names per platform and the help overlay. */
#ifndef HELP_H
#define HELP_H

/* Replaces {L1}, {START}, {DPAD} ... with the button (Miyoo) or key (PC) name. */
void keys_expand(const char *in, char *out, int n);
void help_draw(int view);
/* keys_expand into a small rotating static buffer, for inline use. */
const char *K(const char *in);

#endif
