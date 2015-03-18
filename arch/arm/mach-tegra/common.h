extern struct smp_operations tegra_smp_ops;

extern unsigned long tegra_tsec_start;
extern unsigned long tegra_tsec_size;

#ifdef CONFIG_CACHE_L2X0
void tegra_init_cache(bool init);
#else
static inline void tegra_init_cache(bool init) {}
#endif

extern void tegra_cpu_die(unsigned int cpu);
extern int tegra_cpu_kill(unsigned int cpu);
extern unsigned long tegra_avp_kernel_start;
extern unsigned long tegra_avp_kernel_size;
