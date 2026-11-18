// preview stub
#pragma once
#define GPIO_IN false
inline void gpio_init(unsigned) {}
inline void gpio_set_dir(unsigned, bool) {}
inline void gpio_pull_up(unsigned) {}
inline bool gpio_get(unsigned) { return true; }
