/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __KVM_X86_TDX_H
#define __KVM_X86_TDX_H

#include <linux/list.h>
#include <linux/kvm_host.h>

#include "tdx_arch.h"
#include "tdx_errno.h"
#include "tdx_ops.h"
#include "posted_intr.h"

extern bool __read_mostly emulate_seam;

#ifdef CONFIG_KVM_INTEL_TDX

struct kvm_tdx {
	struct kvm kvm;

	hpa_t tdr;
	hpa_t tdcs[TDX1_NR_TDCX_PAGES];

	int hkid;

	u32 max_vcpus;
};

union tdx_exit_reason {
	struct {
		/* 31:0 mirror the VMX Exit Reason format */
		u64 basic		: 16;
		u64 reserved16		: 1;
		u64 reserved17		: 1;
		u64 reserved18		: 1;
		u64 reserved19		: 1;
		u64 reserved20		: 1;
		u64 reserved21		: 1;
		u64 reserved22		: 1;
		u64 reserved23		: 1;
		u64 reserved24		: 1;
		u64 reserved25		: 1;
		u64 reserved26		: 1;
		u64 enclave_mode	: 1;
		u64 smi_pending_mtf	: 1;
		u64 smi_from_vmx_root	: 1;
		u64 reserved30		: 1;
		u64 failed_vmentry	: 1;

		/* 63:32 are TDX specific */
		u64 details_l1		: 8;
		u64 class		: 8;
		u64 reserved61_48	: 14;
		u64 non_recoverable	: 1;
		u64 error		: 1;
	};
	u64 full;
};

struct vcpu_tdx {
	struct kvm_vcpu	vcpu;

	hpa_t tdvpr;
	hpa_t tdvpx[TDX1_NR_TDVPX_PAGES];

	struct list_head cpu_list;
	int cpu;

	/* Posted interrupt descriptor */
	struct pi_desc pi_desc;

	union {
		struct {
			union {
				struct {
					u16 gpr_mask;
					u16 xmm_mask;
				};
				u32 regs_mask;
			};
			u32 reserved;
		};
		u64 rcx;
	} tdvmcall;

	union tdx_exit_reason exit_reason;
};

struct tdx_capabilities {
	u8 tdcs_nr_pages;
	u8 tdvpx_nr_pages;

	u64 attrs_fixed0;
	u64 attrs_fixed1;
	u64 xfam_fixed0;
	u64 xfam_fixed1;

	u32 nr_cpuid_configs;
	struct tdx_cpuid_config cpuid_configs[TDX1_MAX_NR_CPUID_CONFIGS];
};

static inline bool is_td(struct kvm *kvm)
{
	return kvm->arch.vm_type == KVM_X86_TDX_VM;
}

static inline bool is_td_vcpu(struct kvm_vcpu *vcpu)
{
	return is_td(vcpu->kvm);
}

static inline struct kvm_tdx *to_kvm_tdx(struct kvm *kvm)
{
	return container_of(kvm, struct kvm_tdx, kvm);
}

static inline struct vcpu_tdx *to_tdx(struct kvm_vcpu *vcpu)
{
	return container_of(vcpu, struct vcpu_tdx, vcpu);
}

static __always_inline void tdvps_vmcs_check(u32 field)
{
	BUILD_BUG_ON_MSG(__builtin_constant_p(field) && (field) & 0x1,
			 "Read/Write to TD VMCS *_HIGH fields not supported");
}

static __always_inline void tdvps_gpr_check(u64 field)
{
	BUILD_BUG_ON_MSG(__builtin_constant_p(field) && (field) >= NR_VCPU_REGS,
			 "Invalid TD guest GPR index");
}

static __always_inline void tdvps_apic_check(u64 field) {}
static __always_inline void tdvps_dr_check(u64 field) {}
static __always_inline void tdvps_state_check(u64 field) {}
static __always_inline void tdvps_msr_check(u64 field) {}

#define TDX_BUILD_TDVPS_ACCESSORS(bits, uclass, lclass)			      \
static __always_inline u##bits td_##lclass##_read##bits(struct vcpu_tdx *tdx, \
							u32 field)	      \
{									      \
	struct tdx_ex_ret ex_ret;					      \
	long err;							      \
									      \
	tdvps_##lclass##_check(field);					      \
	err = tdrdvps(tdx->tdvpr, TDVPS_##uclass(field), &ex_ret);	      \
	if (unlikely(err)) {						      \
		pr_err("TDRDVPS["#uclass".0x%x] failed: 0x%lx\n", field, err);\
		return 0;						      \
	}								      \
	return (u##bits)ex_ret.r8;					      \
}									      \
static __always_inline void td_##lclass##_write##bits(struct vcpu_tdx *tdx,   \
						      u32 field, u##bits val) \
{									      \
	struct tdx_ex_ret ex_ret;					      \
	long err;							      \
									      \
	tdvps_##lclass##_check(field);					      \
	err = tdwrvps(tdx->tdvpr, TDVPS_##uclass(field), val,		      \
		      GENMASK_ULL(bits - 1, 0), &ex_ret);		      \
	if (unlikely(err))						      \
		pr_err("TDRDVPS["#uclass".0x%x] = 0x%llx failed: 0x%lx\n",    \
		       field, (u64)val, err);				      \
}									      \
static __always_inline void td_##lclass##_setbit##bits(struct vcpu_tdx *tdx,  \
						       u32 field, u64 bit)    \
{									      \
	struct tdx_ex_ret ex_ret;					      \
	long err;							      \
									      \
	tdvps_##lclass##_check(field);					      \
	err = tdwrvps(tdx->tdvpr, TDVPS_##uclass(field), bit, bit, &ex_ret);  \
	if (unlikely(err))						      \
		pr_err("TDRDVPS["#uclass".0x%x] |= 0x%llx failed: 0x%lx\n",   \
		       field, bit, err);				      \
}									      \
static __always_inline void td_##lclass##_clearbit##bits(struct vcpu_tdx *tdx,\
						         u32 field, u64 bit)  \
{									      \
	struct tdx_ex_ret ex_ret;					      \
	long err;							      \
									      \
	tdvps_##lclass##_check(field);					      \
	err = tdwrvps(tdx->tdvpr, TDVPS_##uclass(field), 0, bit, &ex_ret);    \
	if (unlikely(err))						      \
		pr_err("TDRDVPS["#uclass".0x%x] &= ~0x%llx failed: 0x%lx\n",  \
		       field, bit, err);				      \
}

TDX_BUILD_TDVPS_ACCESSORS(16, VMCS, vmcs);
TDX_BUILD_TDVPS_ACCESSORS(32, VMCS, vmcs);
TDX_BUILD_TDVPS_ACCESSORS(64, VMCS, vmcs);

TDX_BUILD_TDVPS_ACCESSORS(64, APIC, apic);
TDX_BUILD_TDVPS_ACCESSORS(64, GPR, gpr);
TDX_BUILD_TDVPS_ACCESSORS(64, DR, dr);
TDX_BUILD_TDVPS_ACCESSORS(64, STATE, state);
TDX_BUILD_TDVPS_ACCESSORS(64, MSR, msr);

#else

struct kvm_tdx;
struct vcpu_tdx;

static inline bool is_td(struct kvm *kvm) { return false; }
static inline bool is_td_vcpu(struct kvm_vcpu *vcpu) { return false; }
static inline struct kvm_tdx *to_kvm_tdx(struct kvm *kvm) { return NULL; }
static inline struct vcpu_tdx *to_tdx(struct kvm_vcpu *vcpu) { return NULL; }

#endif /* CONFIG_KVM_INTEL_TDX */

#endif /* __KVM_X86_TDX_H */
