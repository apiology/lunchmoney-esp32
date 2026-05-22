#pragma once

#include <lvgl.h>

void boardInitDisplay();
lv_display_t* boardInitLvgl();
void boardInitTouch();
void boardSetScrollTarget(lv_obj_t* scroll_obj);
void boardPollTouchScroll();
void boardFlushPendingScroll();
void boardPollScrollInertia();
bool boardScrollActive();
void boardLogScrollMetrics(lv_obj_t* scroll_obj);
void boardSetTouchDebug(bool enable);
void boardLogDisplayInfo();
