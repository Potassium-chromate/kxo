#include <linux/sched/loadavg.h>  // For EXP_1, EXP_5, EXP_15, FIXED_1

struct Load {
    unsigned long long one_min_load;
    unsigned long long five_min_load;
    unsigned long long fifteen_min_load;
};

static inline void init_load(struct Load *load)
{
    load->one_min_load = 0;
    load->five_min_load = 0;
    load->fifteen_min_load = 0;
}

static inline void renew_load(struct Load *load, unsigned long long active)
{
    load->one_min_load = calc_load(load->one_min_load, EXP_1, active);
    load->five_min_load = calc_load(load->five_min_load, EXP_5, active);
    load->fifteen_min_load = calc_load(load->fifteen_min_load, EXP_15, active);
}
