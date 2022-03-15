/*
 * kAFL agent for fuzzing #VE returns
 *
 * Dynamically initiate handshake + snapshot upon first use
 */

#include <linux/kernel.h>
#include <linux/random.h>
#include <linux/percpu.h>
#include <linux/smp.h>
#include <linux/slab.h>
#include <linux/memblock.h>
#include <linux/kprobes.h>
#include <asm/tdx.h>
#include <asm/trace/tdx.h>

#include <linux/pci.h>
#include <linux/msi.h>
#include <linux/of_device.h>
#include <linux/of_pci.h>
#include <linux/pci_hotplug.h>

#undef pr_fmt
#define pr_fmt(fmt) "kAFL: " fmt

#include <kafl_user.h>

static bool agent_initialized = false;
static bool agent_enabled = false;
static agent_config_t agent_config = {0};
static host_config_t host_config = {0};

static kafl_dump_file_t dump_file __attribute__((aligned(4096)));
static uint8_t payload_buffer[PAYLOAD_BUFFER_SIZE] __attribute__((aligned(4096)));
static uint8_t observed_payload_buffer[PAYLOAD_BUFFER_SIZE] __attribute__((aligned(4096)));
static uint32_t location_stats[TDX_FUZZ_MAX];

static agent_flags_t *agent_flags;
static u64 *ve_buf;
static u32 ve_num;
static u32 ve_pos;
static u32 ve_mis;

static u64 *ob_buf;
static u32 ob_num;
static u32 ob_pos;

static void tdx_fuzz_filter_init(void);

void kafl_raise_panic(void) {
	kAFL_hypercall(HYPERCALL_KAFL_PANIC, 0);
}

void kafl_raise_kasan(void) {
	kAFL_hypercall(HYPERCALL_KAFL_KASAN, 0);
}

void kafl_agent_setrange(void)
{
	uintptr_t ranges[3];
	ranges[0] = (uintptr_t)&pci_scan_bridge & PAGE_MASK;
	ranges[0] = (uintptr_t)&tdx_handle_virtualization_exception & PAGE_MASK;
	//ranges[0] = (uintptr_t)&fuzzme & PAGE_MASK;
	ranges[1] = ranges[0] + PAGE_SIZE;
	ranges[2] = 0;
	hprintf("Setting range %lu: %lx-%lx\n", ranges[2], ranges[0], ranges[1]);
	kAFL_hypercall(HYPERCALL_KAFL_RANGE_SUBMIT, (uintptr_t)ranges);
}

void kafl_agent_abort(char *msg)
{
	hprintf(msg);
	kAFL_hypercall(HYPERCALL_KAFL_USER_ABORT, 0);
	BUG();
}

static
void kafl_dump_observed_payload(char *filename, int append, uint8_t *buf, uint32_t buflen)
{
	char fname_buf[128];
	strncpy(fname_buf, filename, sizeof(fname_buf));
	dump_file.file_name_str_ptr = (uint64_t)fname_buf;
	dump_file.data_ptr = (uint64_t)buf;
	dump_file.bytes = buflen;
	dump_file.append = append;

	kAFL_hypercall(HYPERCALL_KAFL_DUMP_FILE, (uintptr_t)&dump_file);
}

void kafl_agent_init(void)
{
	kAFL_payload* payload = (kAFL_payload*)payload_buffer;

	if (agent_initialized) {
		kafl_agent_abort("Warning: Agent was already initialized!\n");
	}

	hprintf("[*] Initialize kAFL Agent\n");
	tdx_fuzz_filter_init();

	/* initial fuzzer handshake */
	kAFL_hypercall(HYPERCALL_KAFL_ACQUIRE, 0);
	kAFL_hypercall(HYPERCALL_KAFL_RELEASE, 0);

	/* used for code injection and libxdc disassembly */
#if defined(__i386__)
	kAFL_hypercall(HYPERCALL_KAFL_USER_SUBMIT_MODE, KAFL_MODE_32);
#elif defined(__x86_64__)
	kAFL_hypercall(HYPERCALL_KAFL_USER_SUBMIT_MODE, KAFL_MODE_64);
#endif

#if 0
	/* all error handlers are instrumented directly in source code */

	void* panic_handler = 0;
	void* printk_handler = 0;
	void* kasan_handler = 0;

	printk_handler = (void*)&_printk;
	hprintf("Kernel Print Handler Address:\t%lx\n", (uintptr_t)printk_handler);
	
	panic_handler = (void*)&panic;
	hprintf("Kernel Panic Handler Address:\t%lx\n", (uintptr_t)panic_handler);

	//kasan_handler = (void*)&kasan_report_error;
	if (kasan_handler){
		hprintf("Kernel KASan Handler Address:\t%lx\n", (uintptr_t)kasan_handler);
	}
	
	kAFL_hypercall(HYPERCALL_KAFL_PRINTK_ADDR, (uintptr_t)printk_handler);
	kAFL_hypercall(HYPERCALL_KAFL_SUBMIT_PANIC, (uintptr_t)panic_handler);
	if (kasan_handler){
		kAFL_hypercall(HYPERCALL_KAFL_SUBMIT_KASAN, (uintptr_t)kasan_handler);
	}
#endif

	/* ensure that the virtual memory is *really* present in physical memory... */
	memset(observed_payload_buffer, 0xff, PAYLOAD_BUFFER_SIZE);
	memset(payload, 0xff, PAYLOAD_BUFFER_SIZE);

	hprintf("Submitting payload buffer address to hypervisor (%lx)\n", payload);
	kAFL_hypercall(HYPERCALL_KAFL_GET_PAYLOAD, (uint64_t)payload);

	kAFL_hypercall(HYPERCALL_KAFL_SET_AGENT_CONFIG, (uintptr_t)&agent_config);
	kAFL_hypercall(HYPERCALL_KAFL_GET_HOST_CONFIG, (uintptr_t)&host_config);

	hprintf("[host_config] bitmap sizes = <0x%x,0x%x>\n", host_config.bitmap_size, host_config.ijon_bitmap_size);
	hprintf("[host_config] payload size = %dKB\n", host_config.payload_buffer_size/1024);
	hprintf("[host_config] worker id = %02u\n", host_config.worker_id);

	if (host_config.payload_buffer_size > PAYLOAD_BUFFER_SIZE) {
		kafl_agent_abort("Host agent buffer is larger than agent side allocation!\n");
	}

	//if (agent_config.dump_payloads) {
	//	ob_buf = (u64*)observed_payload_buffer;
	//	ob_num = sizeof(observed_payload_buffer)/sizeof(u64);
	//	ob_pos = 0;
	//	pr_debug("Enabled dump payload (max_entries=%u)\n", ob_num);
	//}

	//hprintf("Submitting current CR3 value to hypervisor...\n");
	//kAFL_hypercall(HYPERCALL_KAFL_SUBMIT_CR3, 0);

	// set IP filter range from agent?
	//kafl_agent_setrange();

	// fetch fuzz input for later #VE injection
	hprintf("Starting kAFL loop...\n");
	kAFL_hypercall(HYPERCALL_KAFL_NEXT_PAYLOAD, 0);

	ve_buf = (u64*)payload->data;
	ve_num = payload->size / sizeof(u64);
	ve_pos = 0;
	ve_mis = 0;

	agent_flags = &payload->flags;
	if (agent_flags->raw_data != 0) {
		pr_debug("Runtime agent flags=%x\n", agent_flags->raw_data);
		pr_debug("\t dump_observed = %u\n", agent_flags->dump_observed);
		pr_debug("\t dump_stats = %u\n", agent_flags->dump_stats);
		pr_debug("\t dump_callers = %u\n", agent_flags->dump_callers);
	}

	if (agent_flags->dump_observed) {
		ob_buf = (u64*)observed_payload_buffer;
		ob_num = sizeof(observed_payload_buffer)/sizeof(u64);
		ob_pos = 0;
	}

	memset(location_stats, 0, sizeof(location_stats));
	agent_initialized = true;

	// start coverage tracing
	kAFL_hypercall(HYPERCALL_KAFL_ACQUIRE, 0); 
}

void kafl_agent_done(void)
{
	if (!agent_initialized)
		return;

	agent_enabled = false;

	//unsigned i;
	//pr_info("[*] Injected %d values, missed %d.\n", ve_pos, ve_mis);
	//for (i=0; i<TDX_FUZZ_MAX; i++) {
	//	if (location_stats[i] != 0) {
	//		pr_debug("\tstat[%u] = %u\n", i, location_stats[i]);
	//	}
	//}

	// Dump observed values
	if (agent_flags->dump_observed) {
		pr_debug("Dumping observed input...\n");
		kafl_dump_observed_payload("", false, (uint8_t*)ob_buf, ob_pos*sizeof(ob_buf[0]));
	}

	if (agent_flags->dump_stats) {

		// flag if payload buffer is >90% and we quit due to missing input
		char maxed_out = ' ';
		if (ve_mis && MAX_PAYLOAD_LEN/10*9 < (ve_pos) * sizeof(ve_buf[0])) {
			maxed_out = '*';
		}

		ob_num = snprintf(observed_payload_buffer,
				          sizeof(observed_payload_buffer),
						  "%05u/%u: %5u, %5u, %5u;\trng=%u; cpuid=<%u,%u,%u,%u>; err=<%u,%u,%u,%u> %c\n",
				          ve_pos, ve_mis,
				          location_stats[TDX_FUZZ_MSR_READ],
				          location_stats[TDX_FUZZ_MMIO_READ],
				          location_stats[TDX_FUZZ_PORT_IN],
				          location_stats[TDX_FUZZ_RANDOM],
				          location_stats[TDX_FUZZ_CPUID1],
				          location_stats[TDX_FUZZ_CPUID2],
				          location_stats[TDX_FUZZ_CPUID3],
				          location_stats[TDX_FUZZ_CPUID4],
						  location_stats[TDX_FUZZ_MSR_READ_ERR],
						  location_stats[TDX_FUZZ_MSR_WRITE_ERR],
						  location_stats[TDX_FUZZ_MAP_ERR],
						  location_stats[TDX_FUZZ_PORT_IN_ERR],
						  maxed_out);
		pr_debug("Dumping fuzzer location stats\n");
		kafl_dump_observed_payload("fuzzer_location_stats.lst", true,
			   observed_payload_buffer, ob_num);
	}

	// Stops tracing and restore the snapshot
	// Non-zero argument triggers stream_expand mutation in kAFL
	kAFL_hypercall(HYPERCALL_KAFL_RELEASE, ve_mis*sizeof(ve_buf[0]));
}

void kafl_agent_stop(void)
{
	if (!agent_enabled) {
		kafl_agent_abort("Attempt to finish kAFL run but not yet enabled\n");
	}

	if (!agent_initialized) {
		kafl_agent_abort("Attempt to finish kAFL run but never initialized\n");
	}

	kafl_agent_done();
}

u64 kafl_fuzz_var(u64 var)
{
	if (ve_pos < ve_num) {
		var = ve_buf[ve_pos];
		ve_pos++;
	}
	else {
		ve_mis++;
		// stop at end of fuzz input, unless in dump mode
		if (!agent_flags->dump_observed)
			kafl_agent_done();
	}

	return var;
}

char *tdx_fuzz_loc_str[] = {
	"MSR",
	"MMIO",
	"ERR_RMSR",
	"ERR_WMSR",
	"ERR_MMAP",
	"PIO",
	"ERR_PIO",
	"CPUID1",
	"CPUID2",
	"CPUID3",
	"CPUID4",
	"PRNG",
};

u64 tdx_fuzz(u64 orig_var, uintptr_t addr, int size, enum tdx_fuzz_loc type)
{
	u64 var;

	if (!agent_enabled) {
		return orig_var;
	}

	// skip any fuzzing blockers
	switch(type) {
		//case TDX_FUZZ_PORT_IN:
		//case TDX_FUZZ_MSR_READ:
#ifdef CONFIG_TDX_FUZZ_KAFL_SKIP_RNG_SEEDING
		case TDX_FUZZ_RANDOM:
			return 42;
#endif
#ifdef CONFIG_TDX_FUZZ_KAFL_SKIP_ACPI_PIO
		case TDX_FUZZ_PORT_IN:
			if (addr == 0xb004) {
				return orig_var;
			}
			break;
#endif
#ifdef CONFIG_TDX_FUZZ_KAFL_SKIP_IOAPIC_READS
		case TDX_FUZZ_MMIO_READ:
			if (addr == 0xfec00000 || addr == 0xfec00010) {
				return orig_var;
			}
			break;
#endif
#ifdef CONFIG_TDX_FUZZ_KAFL_DISABLE_CPUID_FUZZ
		case TDX_FUZZ_CPUID1:
		case TDX_FUZZ_CPUID2:
		case TDX_FUZZ_CPUID3:
		case TDX_FUZZ_CPUID4:
			return orig_var;
#endif
		default:
			; // continue to fuzzing
	}

	if (!agent_initialized) {
		kafl_agent_init();
	}

	location_stats[type]++;
	var = kafl_fuzz_var(orig_var);

	if (agent_flags->dump_callers) {
		printk(KERN_INFO "\nfuzz_var: %s[%d], addr: %16lx, value: %16llx => %16llx\n", tdx_fuzz_loc_str[type], size, addr, orig_var, var);
		dump_stack();
	}

	if (agent_flags->dump_observed) {
		// record input seen so far
		// execution may be (have been) partly driven by fuzzer
		if (ob_pos < ob_num) {
			//hprintf("dump_payload: ob_buf[%u] = %08lx\n", ob_pos, var);
			ob_buf[ob_pos++] = var;
		} else {
			pr_warn("Warning: insufficient space in dump_payload\n");
			kafl_agent_done();
		}
	}

	return var;
}

void tdx_fuzz_enable(void)
{
	agent_enabled = true;
	pr_debug("[*] Agent enabled.\n");
}

struct disallowlist_entry {
        struct list_head next;
        char *buf;
};
static __initdata_or_module LIST_HEAD(disallowed_fuzzing_calls);

static int __init fuzzing_disallow(char *str)
{
        char *str_entry;
        struct disallowlist_entry *entry;

        /* str argument is a comma-separated list of functions */
        do {
                str_entry = strsep(&str, ",");
                if (str_entry) {
                        pr_debug("disabling fuzzing for call %s\n", str_entry);
                        entry = memblock_alloc(sizeof(*entry),
                                               SMP_CACHE_BYTES);
                        if (!entry)
                                panic("%s: Failed to allocate %zu bytes\n",
                                      __func__, sizeof(*entry));
                        entry->buf = memblock_alloc(strlen(str_entry) + 1,
                                                    SMP_CACHE_BYTES);
                        if (!entry->buf)
                                panic("%s: Failed to allocate %zu bytes\n",
                                      __func__, strlen(str_entry) + 1);
                        strcpy(entry->buf, str_entry);
                        list_add(&entry->next, &disallowed_fuzzing_calls);
                }
        } while (str_entry);

        return 0;
}

__setup("fuzzing_disallow=", fuzzing_disallow);


static int kp_handler_pre(struct kprobe *p, struct pt_regs *regs)
{
	pr_debug("disable fuzzing for %s\n", p->symbol_name);
	tdx_fuzz_event(TDX_FUZZ_DISABLE);
	return 0;
}

static void kp_handler_post(struct kprobe *p, struct pt_regs *regs, unsigned long flags)
{
	// TODO: check if it should be enabled
	tdx_fuzz_event(TDX_FUZZ_ENABLE);
}

#define TDX_MAX_NUM_KPROBES 16
static struct kprobe tdx_kprobes[TDX_MAX_NUM_KPROBES] = {0};
static int tdx_kprobes_n = 0;

static void tdx_fuzz_filter_init(void)
{
	int ret;

	struct disallowlist_entry *entry;
	struct kprobe *kp;

	list_for_each_entry(entry, &disallowed_fuzzing_calls, next) {
		pr_info("disable fuzzing mutation for %s\n", entry->buf);
		//struct kprobe *kp = kzalloc(sizeof(struct kprobe), GFP_KERNEL);
		if (tdx_kprobes_n >= TDX_MAX_NUM_KPROBES) {
			pr_info("%s: max number of probes reached (%d)\n", __func__, tdx_kprobes_n);
			return;
		}
		kp = &tdx_kprobes[tdx_kprobes_n++];
		kp->symbol_name = entry->buf;
		//kp->addr = &fork_init;
		kp->pre_handler = kp_handler_pre;
		kp->post_handler = kp_handler_post;
		ret = register_kprobe(kp);
		if (ret < 0) {
			pr_info("register_kprobe failed, returned %d\n", ret);
			continue;
		}
		ret = enable_kprobe(kp);
		if (ret < 0) {
			pr_info("enable_kprobe failed, returned %d\n", ret);
			continue;
		}
		pr_info("Planted kprobe at %lx\n", (uintptr_t)kp->addr);
	}

}

void tdx_fuzz_event(enum tdx_fuzz_event e)
{
	switch (e) {
		case TDX_FUZZ_PANIC:
			return kafl_raise_panic();
		case TDX_FUZZ_KASAN:
		case TDX_FUZZ_UBSAN:
			return kafl_raise_kasan();
		case TDX_FUZZ_DONE:
			//return kafl_agent_stop();
			return kafl_agent_done();
		case TDX_FUZZ_ERROR:
			// raise potential error conditions for review?
			return kafl_raise_panic();
		case TDX_FUZZ_HALT:
		case TDX_FUZZ_REBOOT:
		case TDX_FUZZ_SAFE_HALT:
			// raise potential error conditions for review?
			//return kafl_raise_panic();
		case TDX_FUZZ_TIMEOUT:
			return kafl_agent_done();
		case TDX_FUZZ_DISABLE:
			hprintf("TDX_FUZZ_DISABLE agent_initialized=%d agent_enabled=%d\n", agent_initialized, agent_enabled);
			if (agent_initialized) {
				agent_enabled = false;
			}
			break;
		case TDX_FUZZ_ENABLE:
			hprintf("TDX_FUZZ_ENABLE agent_initialized=%d agent_enabled=%d\n", agent_initialized, agent_enabled);
			if (agent_initialized) {
				agent_enabled = true;
			}
			break;
		default:
			return kafl_agent_abort("Unrecognized fuzz event.\n");

	}
}
