/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2019 idkwhoiam322 <idkwhoiam322@raphielgang.org>
 */

/* In-kernel powerHAL to replicate some behaviours of the pixel powerHAL */

#ifndef _POWER_HAL_H
#define _POWER_HAL_H

#ifdef CONFIG_IN_KERNEL_POWERHAL
/* powerHAL main */
void powerhal_boost_kick(void);
void powerhal_boost_kick_max(unsigned int duration_ms);

/* CPUBW management */
void set_hyst_trigger_count_val(int val);
void set_hist_memory_val(int val);
void set_hyst_length_val(int val);

/* EAS */
extern bool disable_boost;

#if defined(CONFIG_SCHED_TUNE) && defined(CONFIG_CGROUP_SCHEDTUNE)
int disable_schedtune_boost(char *st_name, bool disable);
#else
static inline int disable_schedtune_boost(char *st_name, bool disable) { }
#endif

#else
static inline void powerhal_boost_kick(void) { }
static inline void powerhal_boost_kick_max(unsigned int duration_ms) { }
static inline void set_hyst_trigger_count_val(int val) { }
static inline void set_hist_memory_val(int val) { }
static inline void set_hyst_length_val(int val) { }
#endif /* IN_KERNEL_POWERHAL */

#endif /* _POWER_HAL_H */
