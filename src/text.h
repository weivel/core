#ifndef WEIVEL_TEXT_H
#define WEIVEL_TEXT_H

/** Formats into an allocation of exactly the size needed, so a long id is never cut; NULL when memory runs out. */
char *weivel_text_format(const char *format, ...);

/** A copy of `text` the caller frees; NULL when memory runs out. */
char *weivel_text_copy(const char *text);

#endif
