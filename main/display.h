#ifndef DISPLAY_H
#define DISPLAY_H

#ifdef __cplusplus
extern "C" {
#endif

void display_init(void);
void display_deinit(void);
void display_clear(void);
void display_text(int row, int col, const char *str);
void display_show(void);

#ifdef __cplusplus
}
#endif

#endif
