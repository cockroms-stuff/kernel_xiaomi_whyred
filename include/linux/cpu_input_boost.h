/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2018-2019 Sultan Alsawaf <sultan@kerneltoast.com>.
 */
#ifndef _CPU_INPUT_BOOST_H_
#define _CPU_INPUT_BOOST_H_

#ifdef CONFIG_CPU_INPUT_BOOST
void powerhal_boost_kick(void);
void powerhal_boost_kick_max(unsigned int duration_ms);
#else
static inline void powerhal_boost_kick(void) { }
static inline void powerhal_boost_kick_max(unsigned int duration_ms) { }
#endif

/* CPUBW management */
#ifdef CONFIG_DEVFREQ_GOV_QCOM_BW_HWMON
void set_hyst_trigger_count_val(int val);
void set_hist_memory_val(int val);
void set_hyst_length_val(int val);
#else
static inline void set_hyst_trigger_count_val(int val) { }
static inline void set_hist_memory_val(int val) { }
static inline void set_hyst_length_val(int val) { }
#endif

#endif /* _CPU_INPUT_BOOST_H_ */
