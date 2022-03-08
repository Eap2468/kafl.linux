/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_X86_KVM_BOOT_H
#define _ASM_X86_KVM_BOOT_H

#include <linux/cpumask.h>
#include <linux/types.h>
#include <asm/processor.h>

#ifdef CONFIG_KVM_INTEL_TDX
void __init seam_map_seamrr(unsigned long (*map) (unsigned long start,
						  unsigned long end,
						  unsigned long ps_mask));
int seam_load_module(const char *name, void *data, u64 size);
int seam_load_module_from_path(const char *seam_module);

void __init tdx_seam_init(void);
void tdx_init_cpu(struct cpuinfo_x86 *c);
/* TDX CPU mask for TDSYSCONFIGKEY/TDCONFIGKEY -- one cpu per package */
extern const struct cpumask *tdx_package_leadcpus;
#else
static inline void __init tdx_seam_init(void) {}
static inline void tdx_init_cpu(struct cpuinfo_x86 *c) {}
#endif

#endif /* _ASM_X86_KVM_BOOT_H */
