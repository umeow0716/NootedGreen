	
//  Copyright © 2026 Stezza @ inc. Licensed under the Thou Shalt Not Profit License version 1.0. See LICENSE for
//  details.

#ifndef kern_gen11_hpp
#define kern_gen11_hpp
#include "kern_green.hpp"
#include "kern_patcherplus.hpp"
#include <Headers/kern_util.hpp>
#include <IOKit/IOBufferMemoryDescriptor.h>

// AppleIntelTGLGraphics' two-qword virtual-address range.  The SR-IOV VF does
// not expose the stolen-memory sizing field used by the native TGL path, so
// IGMemoryManager can hand the global page-table constructor a zero length.
struct NGIGAddressRange {
	uint64_t start;
	uint64_t length;
};

// Framebuffer flags (fInfoFlags / boot flags) used in platform info patching
// ─── Workaround registers (WA) ──────────────────────────────────────────────
// Hardware workaround register offsets and bits, mostly from i915 Linux driver.
// PSR = Panel Self Refresh interrupt mask/status
#define _PSR_IMR_A				0x60814
#define _PSR_IIR_A				0x60818
// DG1 tile-level master interrupt (reused for GT interrupt routing on Gen12+)
#define DG1_MSTR_TILE_INTR		(0x190008)
#define   DG1_MSTR_IRQ			REG_BIT(31)
#define   DG1_MSTR_TILE(t)		REG_BIT(t)
#define  DBUF_POWER_REQUEST			REG_BIT(31)
// South display clock gating + display chicken register workarounds
#define SOUTH_DSPCLK_GATE_D	(0xc2020)
#define GEN11_CHICKEN_DCPR_2	(0x46434)
#define  PCH_DPMGUNIT_CLOCK_GATE_DISABLE (1 << 15)
#define   DCPR_MASK_MAXLATENCY_MEMUP_CLR	(1 << 27)
#define   DCPR_MASK_LPMODE			(1 << 26)
#define   DCPR_SEND_RESP_IMM			(1 << 25)
#define   DCPR_CLEAR_MEMSTAT_DIS		(1 << 24)
#define GEN9_CLKGATE_DIS_0		(0x46530)
#define   DARBF_GATING_DIS		(1 << 27)
#define DC_STATE_EN			(0x45504)
#define CLKREQ_POLICY			(0x101038)
#define  CLKREQ_POLICY_MEM_UP_OVRD	(1 << 1)
#define GEN11_COMMON_SLICE_CHICKEN3		(0x7304)
#define   GEN12_DISABLE_CPS_AWARE_COLOR_PIPE	(1 << 9)
// Command Streamer chicken bits — GPU preemption granularity control
#define GEN8_CS_CHICKEN1			(0x2580)
#define   GEN9_PREEMPT_3D_OBJECT_LEVEL		(1 << 0)
#define   GEN9_PREEMPT_GPGPU_LEVEL(hi, lo)	(((hi) << 2) | ((lo) << 1))
#define   GEN9_PREEMPT_GPGPU_MID_THREAD_LEVEL	GEN9_PREEMPT_GPGPU_LEVEL(0, 0)
#define   GEN9_PREEMPT_GPGPU_THREAD_GROUP_LEVEL	GEN9_PREEMPT_GPGPU_LEVEL(0, 1)
#define   GEN9_PREEMPT_GPGPU_COMMAND_LEVEL	GEN9_PREEMPT_GPGPU_LEVEL(1, 0)
#define   GEN9_PREEMPT_GPGPU_LEVEL_MASK		GEN9_PREEMPT_GPGPU_LEVEL(1, 1)
#define   GEN8_MCR_SLICE(slice)			(((slice) & 3) << 26)
#define   GEN8_MCR_SLICE_MASK			GEN8_MCR_SLICE(3)
#define   GEN8_MCR_SUBSLICE(subslice)		(((subslice) & 3) << 24)
#define   GEN8_MCR_SUBSLICE_MASK		GEN8_MCR_SUBSLICE(3)
// MCR = Multicast/Replicated register — selects which slice/subslice to target
#define GEN8_MCR_SELECTOR			(0xfdc)
#define PS_INVOCATION_COUNT			(0x2348)
// Force-to-non-privileged access — allows userspace to access certain MMIO regs
#define   RING_FORCE_TO_NONPRIV_ACCESS_RD	(1 << 28)
#define   RING_FORCE_TO_NONPRIV_ACCESS_WR	(2 << 28)
#define   RING_FORCE_TO_NONPRIV_ACCESS_INVALID	(3 << 28)
#define   RING_FORCE_TO_NONPRIV_ACCESS_MASK	(3 << 28)
#define   RING_FORCE_TO_NONPRIV_RANGE_1		(0 << 0)     /* CFL+ & Gen11+ */
#define   RING_FORCE_TO_NONPRIV_RANGE_4		(1 << 0)
#define GEN7_COMMON_SLICE_CHICKEN1		(0x7010)
#define HIZ_CHICKEN				(0x7018)
#define GEN7_FF_SLICE_CS_CHICKEN1		(0x20e0)
#define   GEN9_FFSC_PERCTX_PREEMPT_CTRL		(1 << 14)
#define _PICK_EVEN(__index, __a, __b) ((__a) + (__index) * ((__b) - (__a)))
// BW Buddy — TLB bandwidth allocation for memory access patterns
#define _BW_BUDDY0_CTL			0x45130
#define _BW_BUDDY1_CTL			0x45140
#define BW_BUDDY_CTL(x)			(_PICK_EVEN(x, \
							 _BW_BUDDY0_CTL, \
							 _BW_BUDDY1_CTL))
#define _BW_BUDDY0_PAGE_MASK		0x45134
#define _BW_BUDDY1_PAGE_MASK		0x45144
#define BW_BUDDY_PAGE_MASK(x)		(_PICK_EVEN(x, \
							 _BW_BUDDY0_PAGE_MASK, \
							 _BW_BUDDY1_PAGE_MASK))
#define   BW_BUDDY_DISABLE		( 1 << 31)
#define   BW_BUDDY_TLB_REQ_TIMER_MASK	REG_GENMASK(21, 16)
#define   BW_BUDDY_TLB_REQ_TIMER(x)	REG_FIELD_PREP(BW_BUDDY_TLB_REQ_TIMER_MASK, x)
// FF_MODE2 — Fixed Function mode control (geometry/tessellation timers)
#define GEN12_FF_MODE2				(0x6604)
#define XEHP_FF_MODE2				(0x6604)
#define   FF_MODE2_GS_TIMER_MASK		REG_GENMASK(31, 24)
#define   FF_MODE2_GS_TIMER_224			REG_FIELD_PREP(FF_MODE2_GS_TIMER_MASK, 224)
#define   FF_MODE2_TDS_TIMER_MASK		REG_GENMASK(23, 16)
#define   FF_MODE2_TDS_TIMER_128		REG_FIELD_PREP(FF_MODE2_TDS_TIMER_MASK, 4)
// Miscellaneous clock/power gating controls
#define GEN7_MISCCPCTL				(0x9424)
#define   GEN12_DOP_CLOCK_GATE_RENDER_ENABLE	(1 << 1)
#define GEN9_CS_DEBUG_MODE1			(0x20ec)
#define   FF_DOP_CLOCK_GATE_DISABLE		(1 << 1)
#define GEN8_ROW_CHICKEN2			(0xe4f4)
#define   GEN12_DISABLE_READ_SUPPRESSION	(1 << 15)
#define   GEN12_DISABLE_EARLY_READ		(1 << 14)
#define GEN7_FF_THREAD_MODE		(0x20a0)
#define   GEN7_FF_SCHED_MASK		0x0077070
#define   GEN8_FF_DS_REF_CNT_FFME	(1 << 19)
#define   GEN12_FF_TESSELATION_DOP_GATE_DISABLE BIT(19)
#define GEN10_SAMPLER_MODE			(0xe18c)
#define   ENABLE_SMALLPL			(1 << 15)
#define   GEN12_PUSH_CONST_DEREF_HOLD_DIS	(1 << 8)
#define GEN9_ROW_CHICKEN4			(0xe48c)
#define   GEN12_DISABLE_TDL_PUSH		(1 << 9)
#define RING_PSMI_CTL(base)			((base) + 0x50)
#define   GEN12_WAIT_FOR_EVENT_POWER_DOWN_DISABLE (1 << 7)
#define   GEN8_RC_SEMA_IDLE_MSG_DISABLE		(1 << 12)
#define _PICK_EVEN_2RANGES(__index, __c_index, __a, __b, __c, __d)		\
				   _PICK_EVEN((__index) - (__c_index), __c, __d)
// MBUS Arbiter Box — memory bus bandwidth credit allocation
#define _MBUS_ABOX0_CTL			0x45038
#define _MBUS_ABOX1_CTL			0x45048
#define _MBUS_ABOX2_CTL			0x4504C
#define MBUS_ABOX_CTL(x)							\
	(_PICK_EVEN_2RANGES(x, 2,						\
				 _MBUS_ABOX0_CTL, _MBUS_ABOX1_CTL,		\
				 _MBUS_ABOX2_CTL, _MBUS_ABOX2_CTL))
#define MBUS_ABOX_BW_CREDIT_MASK	(3 << 20)
#define MBUS_ABOX_BW_CREDIT(x)		((x) << 20)
#define MBUS_ABOX_B_CREDIT_MASK		(0xF << 16)
#define MBUS_ABOX_B_CREDIT(x)		((x) << 16)
#define MBUS_ABOX_BT_CREDIT_POOL2_MASK	(0x1F << 8)
#define MBUS_ABOX_BT_CREDIT_POOL2(x)	((x) << 8)
#define MBUS_ABOX_BT_CREDIT_POOL1_MASK	(0x1F << 0)
#define MBUS_ABOX_BT_CREDIT_POOL1(x)	((x) << 0)
// MOCS (Memory Object Control State) — cache policy overrides per ring
#define RING_CMD_CCTL(base)			((base) + 0xc4)
#define CMD_CCTL_WRITE_OVERRIDE_MASK REG_GENMASK(13, 7)
#define CMD_CCTL_READ_OVERRIDE_MASK REG_GENMASK(6, 0)
#define CMD_CCTL_MOCS_MASK (CMD_CCTL_WRITE_OVERRIDE_MASK | \
				CMD_CCTL_READ_OVERRIDE_MASK)
#define CMD_CCTL_MOCS_OVERRIDE(write, read)				      \
		(REG_FIELD_PREP(CMD_CCTL_WRITE_OVERRIDE_MASK, (write) << 1) | \
		 REG_FIELD_PREP(CMD_CCTL_READ_OVERRIDE_MASK, (read) << 1))
#define BLIT_CCTL(base)				((base) + 0x204)
#define   BLIT_CCTL_DST_MOCS_MASK		REG_GENMASK(14, 8)
#define   BLIT_CCTL_SRC_MOCS_MASK		REG_GENMASK(6, 0)
#define   BLIT_CCTL_MASK (BLIT_CCTL_DST_MOCS_MASK | \
			  BLIT_CCTL_SRC_MOCS_MASK)
#define   BLIT_CCTL_MOCS(dst, src)				       \
		(REG_FIELD_PREP(BLIT_CCTL_DST_MOCS_MASK, (dst) << 1) | \
		 REG_FIELD_PREP(BLIT_CCTL_SRC_MOCS_MASK, (src) << 1))
// DBUF — Display Buffer control, one per slice (manages display BW)
#define _DBUF_CTL_S0				0x45008
#define _DBUF_CTL_S1				0x44FE8
#define _DBUF_CTL_S2				0x44300
#define _DBUF_CTL_S3				0x44304
#define  DBUF_TRACKER_STATE_SERVICE_MASK	REG_GENMASK(23, 19)
#define  DBUF_TRACKER_STATE_SERVICE(x)		REG_FIELD_PREP(DBUF_TRACKER_STATE_SERVICE_MASK, x)
//end workar

// ─── GT fuse/topology registers ─────────────────────────────────────────────
// Read at boot to determine which EUs/slices/subslices are enabled
#define   IECPUNIT_CLKGATE_DIS			REG_BIT(22)
#define VDBOX_CGCTL3F10(base)			((base) + 0x3f10)
#define GEN11_GT_VEBOX_VDBOX_DISABLE		(0x9140)
#define GEN11_EU_DISABLE			(0x9134)
#define GEN11_GT_SLICE_ENABLE			(0x9138)
#define GEN11_GT_SUBSLICE_DISABLE		(0x913c)
#define RPM_CONFIG0				(0xd00)
#define   GEN11_GT_VDBOX_DISABLE_MASK		0xff
#define   GEN11_GT_VEBOX_DISABLE_SHIFT		0x10
#define   GEN11_GT_VEBOX_DISABLE_MASK		(0x0f << GEN11_GT_VEBOX_DISABLE_SHIFT)

#define   I915_ERROR_INSTRUCTION			(1 << 0)
// ─── Ring buffer MMIO (per-engine, offset from ring base) ───────────────────
#define RING_START(base)			((base) + 0x38)
#define RING_CTL(base)				((base) + 0x3c)
#define RING_EIR(base)				((base) + 0xb0)
#define RING_EMR(base)				((base) + 0xb4)
#define RING_TAIL(base)				((base) + 0x30)
#define RING_HEAD(base)				((base) + 0x34)
#define RING_HWS_PGA(base)			((base) + 0x80)
#define RING_HWSTAM(base)			((base) + 0x98)
#define RING_MI_MODE(base)			((base) + 0x9c)
#define RING_MODE_GEN7(base)		((base) + 0x29c)
#define RING_ACTHD(base)			((base) + 0x74)
#define RING_ACTHD_UDW(base)		((base) + 0x5c)
#define RING_IPEHR(base)			((base) + 0x68)
#define RING_IPEIR(base)			((base) + 0x64)
#define RING_INSTDONE(base)			((base) + 0x6c)
#define RING_ESR(base)				((base) + 0xb8)
#define RING_CTX_SIZE(base)			((base) + 0x1a0) /* context size */
#define RING_CCID(base)				((base) + 0x180) /* context control ID */
#define RING_CTX_CTRL(base)			((base) + 0x244) /* context control */
/* RING_CTX_CTRL bit definitions (GEN8+/GEN12) */
#define CTX_CTRL_ENGINE_CTX_RESTORE_INHIBIT	(1 << 0)  /* bit 0: inhibit context restore on context switch */
#define CTX_CTRL_ENGINE_CTX_SAVE_INHIBIT	(1 << 2)  /* bit 2: inhibit context save on preemption */
#define CTX_CTRL_INHIBIT_SYN_CTX_SWITCH	(1 << 3)  /* bit 3: force immediate (async) context switch */
/* Writing bits 0+2+3 = 0x0D forces context abandon: no restore, no save, immediate switch.
 * This is the i915 engine-quiesce sequence used before __intel_gt_disable(). */
#define CTX_CTRL_FORCE_ABANDON \
	(CTX_CTRL_ENGINE_CTX_RESTORE_INHIBIT | CTX_CTRL_ENGINE_CTX_SAVE_INHIBIT | CTX_CTRL_INHIBIT_SYN_CTX_SWITCH)
#define RING_ELSP(base)				((base) + 0x230) /* ExecList Submission Port */
#define RING_EXECLIST_STATUS(base)	((base) + 0x234)
#define RING_CONTEXT_STATUS_PTR(base) ((base) + 0x3a0)
#define RING_MODE(base)				((base) + 0x29c)
#define RING_FAULT_REG(base)		((base) + 0x150)
#define RING_DMA_FADD(base)			((base) + 0x78)
#define RING_DMA_FADD_UDW(base)		((base) + 0x60)
#define RING_INSTPM(base)			((base) + 0xC0)
#define RING_CONTEXT_STATUS_BUF(base, idx)    ((base) + 0x370 + (idx) * 8)
#define RING_CONTEXT_STATUS_BUF_HI(base, idx) ((base) + 0x374 + (idx) * 8)

// Global error/fault registers
#define ERROR_GEN6				0x40A0
#define GEN12_RING_FAULT_REG	0xCEC4
#define GEN8_FAULT_TLB_DATA0	0x4B10
#define GEN8_FAULT_TLB_DATA1	0x4B14

// Engine reset registers (Gen6+)
#define GEN6_GDRST				0x941c
#define   GEN6_GRDOM_FULL		(1 << 0)
#define   GEN6_GRDOM_RENDER		(1 << 1)
#define   GEN6_GRDOM_MEDIA		(1 << 2)
#define   GEN6_GRDOM_BLT		(1 << 3)

// Per-engine reset control (Gen12+)
#define RING_RESET_CTL(base)		((base) + 0xd0)
#define   RESET_CTL_REQUEST_RESET	(1 << 0)
#define   RESET_CTL_READY_TO_RESET	(1 << 1)
#define   RESET_CTL_CAT_ERROR		(1 << 2)

// GGTT PTE base within BAR0 (Gen8+: 8MB into MMIO BAR, each PTE is 8 bytes)
#define GEN8_GGTT_PTE_BASE		0x800000
#define GGTT_PTE_LO(page)		(GEN8_GGTT_PTE_BASE + (page) * 8)
#define GGTT_PTE_HI(page)		(GEN8_GGTT_PTE_BASE + (page) * 8 + 4)
#define   GEN11_GFX_DISABLE_LEGACY_MODE		(1 << 3)
#define   STOP_RING				REG_BIT(8)
// Engine ring base addresses (RCS=render, BCS=blitter, VCS/BSD=video, VECS=video enhance)
#define RENDER_RING_BASE	0x02000
#define GEN12_COMPUTE0_RING_BASE	0x1a000
#define BLT_RING_BASE		0x22000
#define GEN11_BSD_RING_BASE	0x1c0000
#define GEN11_BSD3_RING_BASE	0x1d0000
#define GEN11_VEBOX_RING_BASE	0x1c8000

// ─── CDCLK (Core Display Clock) ─────────────────────────────────────────────
static constexpr uint32_t ICL_REG_CDCLK_CTL = 0x46000;
// DSSM = Display Subsystem Status/Mode — contains PLL reference clock info
static constexpr uint32_t ICL_REG_DSSM = 0x51004;


enum ICLReferenceClockFrequency {
	
	// 24 MHz
	ICL_REF_CLOCK_FREQ_24_0 = 0x0,
	
	// 19.2 MHz
	ICL_REF_CLOCK_FREQ_19_2 = 0x1,
	
	// 38.4 MHz
	ICL_REF_CLOCK_FREQ_38_4 = 0x2
};


enum ICLCoreDisplayClockDecimalFrequency {
	
	// 172.8 MHz
	ICL_CDCLK_FREQ_172_8 = 0x158,
	
	// 180 MHz
	ICL_CDCLK_FREQ_180_0 = 0x166,
	
	// 192 MHz
	ICL_CDCLK_FREQ_192_0 = 0x17E,
	
	// 307.2 MHz
	ICL_CDCLK_FREQ_307_2 = 0x264,
	
	// 312 MHz
	ICL_CDCLK_FREQ_312_0 = 0x26E,
	
	// 552 MHz
	ICL_CDCLK_FREQ_552_0 = 0x44E,
	
	// 556.8 MHz
	ICL_CDCLK_FREQ_556_8 = 0x458,
	
	// 648 MHz
	ICL_CDCLK_FREQ_648_0 = 0x50E,
	
	// 652.8 MHz
	ICL_CDCLK_FREQ_652_8 = 0x518
};

static constexpr uint32_t ICL_CDCLK_DEC_FREQ_THRESHOLD = ICL_CDCLK_FREQ_648_0;


// PLL output = refclk × ratio (all produce ~1296 MHz)
static constexpr uint32_t ICL_CDCLK_PLL_FREQ_REF_24_0 = 24000000 * 54;
static constexpr uint32_t ICL_CDCLK_PLL_FREQ_REF_19_2 = 19200000 * 68;
static constexpr uint32_t ICL_CDCLK_PLL_FREQ_REF_38_4 = 38400000 * 34;

//#define  BXT_CDCLK_CD2X_DIV_SEL_MASK	REG_GENMASK(23, 22)
#define  BXT_CDCLK_CD2X_DIV_SEL_MASK	(3 << 22)
#define  BXT_CDCLK_CD2X_DIV_SEL_1	(0 << 22)
#define  BXT_CDCLK_CD2X_DIV_SEL_1_5	(1 << 22)
#define  BXT_CDCLK_CD2X_DIV_SEL_2	(2 << 22)
#define  BXT_CDCLK_CD2X_DIV_SEL_4	(3 << 22)
#define  BXT_CDCLK_CD2X_PIPE(pipe)	((pipe) << 20)
#define  CDCLK_DIVMUX_CD_OVERRIDE	(1 << 19)
#define  BXT_CDCLK_CD2X_PIPE_NONE	BXT_CDCLK_CD2X_PIPE(3)
#define  ICL_CDCLK_CD2X_PIPE_NONE	(7 << 19)
#define  BXT_CDCLK_SSA_PRECHARGE_ENABLE	(1 << 16)
#define  CDCLK_FREQ_DECIMAL_MASK	(0x7ff)
#define SKL_DSSM				(0x51004)
#define ICL_DSSM_CDCLK_PLL_REFCLK_MASK		(7 << 29)
#define ICL_DSSM_CDCLK_PLL_REFCLK_24MHz		(0 << 29)
#define ICL_DSSM_CDCLK_PLL_REFCLK_19_2MHz	(1 << 29)
#define ICL_DSSM_CDCLK_PLL_REFCLK_38_4MHz	(2 << 29)
#define BXT_DE_PLL_ENABLE		(0x46070)
#define   BXT_DE_PLL_PLL_ENABLE		(1 << 31)
#define   BXT_DE_PLL_LOCK		(1 << 30)
#define   BXT_DE_PLL_FREQ_REQ		(1 << 23)
#define   BXT_DE_PLL_FREQ_REQ_ACK	(1 << 22)
#define   ICL_CDCLK_PLL_RATIO(x)	(x)
#define   ICL_CDCLK_PLL_RATIO_MASK	0xff
#define CDCLK_CTL			(0x46000)

#define DIV_ROUND_CLOSEST(x, divisor)(			\
{							\
__typeof(x) __x = x;				\
__typeof(divisor) __d = divisor;			\
	(((__typeof(x))-1) > 0 ||				\
	 ((__typeof(divisor))-1) > 0 || (__x) > 0) ?	\
		(((__x) + ((__d) / 2)) / (__d)) :	\
		(((__x) - ((__d) / 2)) / (__d));	\
}							\
)

// ─── Interrupt registers ────────────────────────────────────────────────────
#define SOUTH_CHICKEN1		(0xc2000)
#define GEN8_MASTER_IRQ		(0x44200)  // same offset as GEN11_DISPLAY_INT_CTL

// Hotplug detection registers (PCH south bridge side)
#define PCH_PORT_HOTPLUG		(0xc4030)
#define SHOTPLUG_CTL_DDI		(0xc4030)  // DDI = Digital Display Interface
#define SHOTPLUG_CTL_TC			(0xc4034)  // TC  = Type-C
#define SHPD_FILTER_CNT			(0xc4038)

// SDE = South Display Engine interrupt mask/identity/enable
#define SDEIMR (0xc4004)
#define SDEIIR (0xc4008)
#define SDEIER (0xc400c)

// DE = Display Engine; HPD = Hot Plug Detect; ISR/IMR/IIR/IER = status/mask/identity/enable
#define GEN11_DE_HPD_ISR		(0x44470)
#define GEN11_DE_HPD_IMR		(0x44474)
#define GEN11_DE_HPD_IIR		(0x44478)
#define GEN11_DE_HPD_IER		(0x4447c)

#define GEN8_DE_MISC_ISR (0x44460)
#define GEN8_DE_MISC_IMR  (0x44464)
#define GEN8_DE_MISC_IIR  (0x44468)
#define GEN8_DE_MISC_IER  (0x4446c)

#define GEN8_DE_PIPE_ISR_A  (0x44400)
#define GEN8_DE_PIPE_IMR_A  (0x44404)
#define GEN8_DE_PIPE_IIR_A  (0x44408)
#define GEN8_DE_PIPE_IER_A  (0x4440c)

#define GEN8_DE_PIPE_ISR_B  (0x44410)
#define GEN8_DE_PIPE_IMR_B  (0x44414)
#define GEN8_DE_PIPE_IIR_B  (0x44418)
#define GEN8_DE_PIPE_IER_B  (0x4441c)

#define GEN8_DE_PIPE_ISR_C  (0x44420)
#define GEN8_DE_PIPE_IMR_C  (0x44424)
#define GEN8_DE_PIPE_IIR_C  (0x44428)
#define GEN8_DE_PIPE_IER_C  (0x4442c)



// ─── GPU frequency / power management ───────────────────────────────────────
// MCHBAR mirror: memory controller hub BAR mapped into GT MMIO space
constexpr uint32_t MCHBAR_MIRROR_BASE_SNB = 0x140000;
// RP_STATE_CAP: contains RP0 (max), RP1 (efficient), RPn (min) frequency caps
constexpr uint32_t GEN6_RP_STATE_CAP = MCHBAR_MIRROR_BASE_SNB + 0x5998;

constexpr uint32_t GEN9_FREQUENCY_SHIFT = 23;
constexpr uint32_t GEN9_FREQ_SCALER  = 3;

static inline unsigned long find_first_bit(const unsigned long *addr, unsigned long size) {
	unsigned long val = *addr;
	if (!val || size == 0) return size;
	return __builtin_ctzl(val);
}

static inline unsigned long find_next_bit(const unsigned long *addr, unsigned long size, unsigned long offset) {
	if (offset >= size) return size;
	unsigned long val = *addr & (~0UL << offset);
	if (!val) return size;
	return __builtin_ctzl(val);
}

#define for_each_set_bit(bit, addr, size) \
	for ((bit) = find_first_bit((addr), (size));		\
		 (bit) < (size);					\
		 (bit) = find_next_bit((addr), (size), (bit) + 1))

#define __bf_shf(x) (__builtin_ffsll(x) - 1)
#define REG_FIELD_PREP(__mask, __val) \
((uint32_t)((__typeof(__mask))(__val) << __bf_shf(__mask)) & (__mask))

// Apple's command streamer type enum (matches IGAccel internal enum)
enum IGHwCsType
{
	kIGHwCsTypeRCS,     //  0 — Render Command Streamer
	kIGHwCsTypeCCS,     //  1 — Compute Command Streamer (Gen12.5+)
	kIGHwCsTypeBCS,     //  2 — Blitter Command Streamer
	kIGHwCsTypeVCS0,    //  3 — Video Command Streamer 0
	kIGHwCsTypeVCS2,    //  4 — Video Command Streamer 2
	kIGHwCsTypeVECS0,   //  5 — Video Enhancement CS 0
};

// Apple's per-engine descriptor — MMIO offsets for ExecList, context, status, forcewake
struct IGHwCsDesc {
	IGHwCsType   type;
	uint32_t     csMask;
	const char * name;
	const char * label;
	uint32_t     mmioExecListSubmitPort;
	uint32_t     mmioExecListSubmitQueue;
	uint32_t     mmioExecListControl;
	uint32_t     mmioExecListStatus;
	uint32_t     mmioContextStatusPointer;
	uint32_t     mmioContextStatusBuffer;
	uint32_t     mmioContextStatusPort;
	uint32_t     mmioContextStatusFifoStatus;
	uint32_t     mmioGlobalStatusPage;
	uint32_t     mmioGfxMode;
	uint32_t     mmioResetCtrl;
	uint32_t     mmioErrorIdentity;
	uint32_t     mmioErrorMask;
	uint32_t     mmioTimeStamp;
	uint32_t     mmioGpr0;
	uint32_t     fuseMask;
	uint32_t     contextSizeBytes;
	int32_t      stampIndexRangeMin;
	int32_t      stampIndexRangeMax;
	int32_t      stampIndexRangeSize;
	IOSelect     contextSwitchInterruptType;
	IOSelect     flushNotifyInterruptType;
	IOSelect     errorInterruptType;
	uint32_t     mmioForcewakeReq;
	uint32_t     mmioForcewakeAck;
};


// BW Buddy page masks — TLB request size depends on DRAM type & channel count
struct buddy_page_mask {
	uint32_t page_mask;
	uint8_t type;
	uint8_t num_channels;
};
enum intel_dram_type {
	INTEL_DRAM_UNKNOWN,
	INTEL_DRAM_DDR3,
	INTEL_DRAM_DDR4,
	INTEL_DRAM_LPDDR3,
	INTEL_DRAM_LPDDR4,
	INTEL_DRAM_DDR5,
	INTEL_DRAM_LPDDR5,
	INTEL_DRAM_GDDR,
} ;

static const struct buddy_page_mask tgl_buddy_page_masks[] = {
	{0xF,  INTEL_DRAM_DDR4,   1},
	{0xF,  INTEL_DRAM_DDR5,   1},
	{0x1C, INTEL_DRAM_LPDDR4, 2},
	{0x1C, INTEL_DRAM_LPDDR5, 2},
	{0x1F, INTEL_DRAM_DDR4,   2},
	{0x1E, INTEL_DRAM_DDR5,   2},
	{0x38, INTEL_DRAM_LPDDR4, 4},
	{0x38, INTEL_DRAM_LPDDR5, 4},
	{}
};

#define GENMASK(high, low) \
	(((0xFFFFFFFF) << (low)) & (0xFFFFFFFF >> (32 - 1 - (high))))

#define REG_GENMASK(__high, __low)	GENMASK(__high, __low)


#define BIT(n) (1U << (n))

// Engine instance IDs (i915 convention)
enum intel_engine_id {
	RCS0 = 0,  // Render
	BCS0,
	BCS1,
	BCS2,
	BCS3,
	BCS4,
	BCS5,
	BCS6,
	BCS7,
	BCS8,
#define _BCS(n) (BCS0 + (n))
	VCS0,
	VCS1,
	VCS2,
	VCS3,
	VCS4,
	VCS5,
	VCS6,
	VCS7,
#define _VCS(n) (VCS0 + (n))
	VECS0,
	VECS1,
	VECS2,
	VECS3,
#define _VECS(n) (VECS0 + (n))
	CCS0,
	CCS1,
	CCS2,
	CCS3,
#define _CCS(n) (CCS0 + (n))
	GSC0,
	I915_NUM_ENGINES
#define INVALID_ENGINE ((enum intel_engine_id)-1)
};
#define __HAS_ENGINE(engine_mask, id) ((engine_mask) & BIT(id))

// ─── GT workaround registers ────────────────────────────────────────────────
#define GEN11_GACB_PERF_CTRL			(0x4b80)
#define   GEN11_HASH_CTRL_MASK			(0x3 << 12 | 0xf << 0)
#define   GEN11_HASH_CTRL_BIT0			(1 << 0)
#define   GEN11_HASH_CTRL_BIT4			(1 << 12)
#define GEN11_LSN_UNSLCVC			(0xb43c)
#define   GEN11_LSN_UNSLCVC_GAFS_HALF_CL2_MAXALLOC	(1 << 9)
#define   GEN11_LSN_UNSLCVC_GAFS_HALF_SF_MAXALLOC	(1 << 7)
#define GEN8_GAMW_ECO_DEV_RW_IA			(0x4080)
#define   GAMW_ECO_ENABLE_64K_IPS_FIELD		0xF
#define   GAMW_ECO_DEV_CTX_RELOAD_DISABLE	(1 << 7)
#define GAMT_CHKN_BIT_REG			(0x4ab8)
#define   GAMT_CHKN_DISABLE_L3_COH_PIPE		(1 << 31)
#define   GAMT_CHKN_DISABLE_DYNAMIC_CREDIT_SHARING	(1 << 28)
#define   GAMT_CHKN_DISABLE_I2M_CYCLE_ON_WR_PORT	(1 << 24)
#define UNSLICE_UNIT_LEVEL_CLKGATE2		(0x94e4)
#define   VSUNIT_CLKGATE_DIS_TGL		BIT(19)
#define   PSDUNIT_CLKGATE_DIS			BIT(5)
#define UNSLICE_UNIT_LEVEL_CLKGATE		(0x9434)
#define   VFUNIT_CLKGATE_DIS			BIT(20)
#define   CG3DDISCFEG_CLKGATE_DIS		BIT(17) /* DG2 */
#define   GAMEDIA_CLKGATE_DIS			BIT(11)
#define   HSUNIT_CLKGATE_DIS			BIT(8)
#define   VSUNIT_CLKGATE_DIS			BIT(3)
#define GEN10_DFR_RATIO_EN_AND_CHICKEN		(0x9550)
#define   DFR_DISABLE				(1 << 9)
// DSS = Dual Sub-Slice clock gating (Gen11+ EU topology)
#define GEN11_SUBSLICE_UNIT_LEVEL_CLKGATE	(0x9524)
#define   DSS_ROUTER_CLKGATE_DIS		BIT(28)
#define   GWUNIT_CLKGATE_DIS			BIT(16)

// Reset handshake — coordinates GT/PCH reset sequencing
#define HSW_NDE_RSTWRN_OPT	(0x46408)
#define  MTL_RESET_PICA_HANDSHAKE_EN	BIT(6)
#define  RESET_PCH_HANDSHAKE_ENABLE	BIT(4)

// Masked register write helpers come from kern_green.hpp. Keep the single-
// evaluation definitions there instead of replacing them with duplicate
// macros that could evaluate a side-effecting argument twice.
#define GEN9_GAMT_ECO_REG_RW_IA (0x4ab0)
#define   GAMT_ECO_ENABLE_IN_PLACE_DECOMPRESS	(1 << 18)


// ─── GT interrupt registers ─────────────────────────────────────────────────
// DW0/DW1 identify which engine fired; identity regs carry interrupt details
#define GEN11_GT_INTR_DW0		(0x190018)
#define  GEN11_CSME			(31)
#define  GEN11_GUNIT			(28)
#define  GEN11_GUC			(25)
#define  GEN11_WDPERF			(20)
#define  GEN11_KCR			(19)
#define  GEN11_GTPM			(16)
#define  GEN11_BCS			(15)
#define  GEN11_RCS0			(0)

#define GEN11_GT_INTR_DW1		(0x19001c)
#define  GEN11_VECS(x)			(31 - (x))
#define  GEN11_VCS(x)			(x)

#define GEN11_GT_INTR_DW(x)		(0x190018 + ((x) * 4))

#define GEN11_INTR_IDENTITY_REG0	(0x190060)
#define GEN11_INTR_IDENTITY_REG1	(0x190064)
#define  GEN11_INTR_DATA_VALID		(1 << 31)
//#define  GEN11_INTR_ENGINE_CLASS(x)	(((x) & GENMASK(18, 16)) >> 16)
//#define  GEN11_INTR_ENGINE_INSTANCE(x)	(((x) & GENMASK(25, 20)) >> 20)
#define  GEN11_INTR_ENGINE_INTR(x)	((x) & 0xffff)
/* irq instances for OTHER_CLASS */
#define OTHER_GUC_INSTANCE	0
#define OTHER_GTPM_INSTANCE	1

#define GEN11_INTR_IDENTITY_REG(x)	(0x190060 + ((x) * 4))

#define GEN11_IIR_REG0_SELECTOR		(0x190070)
#define GEN11_IIR_REG1_SELECTOR		(0x190074)

#define GEN11_IIR_REG_SELECTOR(x)	(0x190070 + ((x) * 4))

// Per-engine interrupt enable/mask registers
#define GEN11_RENDER_COPY_INTR_ENABLE	(0x190030)
#define GEN11_VCS_VECS_INTR_ENABLE	(0x190034)
#define GEN11_GUC_SG_INTR_ENABLE	(0x190038)
#define GEN11_GPM_WGBOXPERF_INTR_ENABLE	(0x19003c)
#define GEN11_CRYPTO_RSVD_INTR_ENABLE	(0x190040)
#define GEN11_GUNIT_CSME_INTR_ENABLE	(0x190044)

#define GEN11_RCS0_RSVD_INTR_MASK	(0x190090)
#define GEN11_BCS_RSVD_INTR_MASK	(0x1900a0)
#define GEN11_VCS0_VCS1_INTR_MASK	(0x1900a8)
#define GEN11_VCS2_VCS3_INTR_MASK	(0x1900ac)
#define GEN12_VCS4_VCS5_INTR_MASK	(0x1900b0)
#define GEN12_VCS6_VCS7_INTR_MASK	(0x1900b4)
#define GEN11_VECS0_VECS1_INTR_MASK	(0x1900d0)
#define GEN12_VECS2_VECS3_INTR_MASK	(0x1900d4)
#define GEN11_GUC_SG_INTR_MASK		(0x1900e8)
#define GEN11_GPM_WGBOXPERF_INTR_MASK	(0x1900ec)
#define GEN11_CRYPTO_RSVD_INTR_MASK	(0x1900f0)
#define GEN11_GUNIT_CSME_INTR_MASK	(0x1900f4)

// GT interrupt status bits — per-engine interrupt cause flags
#define GT_BLT_FLUSHDW_NOTIFY_INTERRUPT		(1 << 26)
#define GT_BLT_CS_ERROR_INTERRUPT		(1 << 25)
#define GT_BLT_USER_INTERRUPT			(1 << 22)
#define GT_BSD_CS_ERROR_INTERRUPT		(1 << 15)
#define GT_BSD_USER_INTERRUPT			(1 << 12)
#define GT_RENDER_L3_PARITY_ERROR_INTERRUPT_S1	(1 << 11) /* hsw+; rsvd on snb, ivb, vlv */
#define GT_CONTEXT_SWITCH_INTERRUPT		(1 <<  8)
#define GT_RENDER_L3_PARITY_ERROR_INTERRUPT	(1 <<  5) /* !snb */
#define GT_RENDER_PIPECTL_NOTIFY_INTERRUPT	(1 <<  4)
#define GT_RENDER_CS_MASTER_ERROR_INTERRUPT	(1 <<  3)
#define GT_RENDER_SYNC_STATUS_INTERRUPT		(1 <<  2)
#define GT_RENDER_DEBUG_INTERRUPT		(1 <<  1)
#define GT_RENDER_USER_INTERRUPT		(1 <<  0)

// Master interrupt control — top-level IRQ enable/routing
#define GEN11_GFX_MSTR_IRQ		(0x190010)
#define  GEN11_MASTER_IRQ		(1 << 31)
#define  GEN11_PCU_IRQ			(1 << 30)
#define  GEN11_GU_MISC_IRQ		(1 << 29)
#define  GEN11_DISPLAY_IRQ		(1 << 16)
#define  GEN11_GT_DW_IRQ(x)		(1 << (x))
#define  GEN11_GT_DW1_IRQ		(1 << 1)
#define  GEN11_GT_DW0_IRQ		(1 << 0)

// Display interrupt control — enables display engine IRQ routing to CPU
#define GEN11_DISPLAY_INT_CTL		(0x44200)
#define  GEN11_DISPLAY_IRQ_ENABLE	(1 << 31)
#define  GEN11_AUDIO_CODEC_IRQ		(1 << 24)
#define  GEN11_DE_PCH_IRQ		(1 << 23)
#define  GEN11_DE_MISC_IRQ		(1 << 22)
#define  GEN11_DE_HPD_IRQ		(1 << 21)
#define  GEN11_DE_PORT_IRQ		(1 << 20)
#define  GEN11_DE_PIPE_C		(1 << 18)
#define  GEN11_DE_PIPE_B		(1 << 17)
#define  GEN11_DE_PIPE_A		(1 << 16)

#define GT_CS_MASTER_ERROR_INTERRUPT		(3)
#define GT_WAIT_SEMAPHORE_INTERRUPT		(11)

enum ack_type {
	ACK_CLEAR = 0,
	ACK_SET
};

// RPS = Render Performance State frequency caps (read from RP_STATE_CAP)
struct intel_rps_freq_caps {
	uint8_t rp0_freq;   // RP0 — max turbo frequency
	uint8_t rp1_freq;   // RP1 — efficient/nominal frequency
	uint8_t min_freq;   // RPn — minimum frequency
};

enum ConnectorType : uint32_t {
	ConnectorZero       = 0x0,
	ConnectorDummy      = 0x1,   /* Always used as dummy, seems to sometimes work as VGA */
	ConnectorLVDS       = 0x2,   /* Just like on AMD LVDS is used for eDP */
	ConnectorDigitalDVI = 0x4,   /* This is not eDP despite a common misbelief */
	ConnectorSVID       = 0x8,
	ConnectorVGA        = 0x10,
	ConnectorDP         = 0x400,
	ConnectorHDMI       = 0x800,
	ConnectorAnalogDVI  = 0x2000
};

/* I can see very few mentioned in the code (0x1, 0x8, 0x40), though connectors themselves define way more! */

union ConnectorFlags {
	struct ConnectorFlagBits {
		/* Bits 1, 2, 8 are mentioned in AppleIntelFramebufferController::GetGPUCapability */
		/* Lets apperture memory to be not required AppleIntelFramebuffer::isApertureMemoryRequired */
		uint8_t CNAlterAppertureRequirements :1;  /* 0x1 */
		uint8_t CNUnknownFlag_2              :1;  /* 0x2 */
		uint8_t CNUnknownFlag_4              :1;  /* 0x4 */
		/* Normally set for LVDS displays (i.e. built-in displays) */
		uint8_t CNConnectorAlwaysConnected   :1;  /* 0x8 */
		/* AppleIntelFramebuffer::maxSupportedDepths checks this and returns 2 IODisplayModeInformation::maxDepthIndex ?? */
		uint8_t CNUnknownFlag_10             :1;  /* 0x10 */
		uint8_t CNUnknownFlag_20             :1;  /* 0x20 */
		/* Disable blit translation table? AppleIntelFramebufferController::ConfigureBufferTranslation */
		uint8_t CNDisableBlitTranslationTable:1;  /* 0x40 */
		/* Used in AppleIntelFramebufferController::setPowerWellState */
		/* Activates MISC IO power well (SKL_DISP_PW_MISC_IO) */
		uint8_t CNUseMiscIoPowerWell         :1;  /* 0x80 */
		/* Used in AppleIntelFramebufferController::setPowerWellState */
		/* Activates Power Well 2 usage (SKL_PW_CTL_IDX_PW_2) */
		/* May help with HDMI audio configuration issues */
		/* REF: https://github.com/acidanthera/bugtracker/issues/1189 */
		uint8_t CNUsePowerWell2              :1;  /* 0x100 */
		uint8_t CNUnknownFlag_200            :1;  /* 0x200 */
		uint8_t CNUnknownFlag_400            :1;  /* 0x400 */
		/* Sets fAvailableLaneCount to 30 instead of 20 when specified */
		uint8_t CNIncreaseLaneCount          :1;  /* 0x800 */
		uint8_t CNUnknownFlag_1000           :1;  /* 0x1000 */
		uint8_t CNUnknownFlag_2000           :1;  /* 0x2000 */
		uint8_t CNUnknownFlag_4000           :1;  /* 0x4000 */
		uint8_t CNUnknownFlag_8000           :1;  /* 0x8000 */
		uint16_t CNUnknownZeroFlags;
	} bits;
	uint32_t value;
};

struct PACKED ConnectorInfo {
	/* Watch out, this is really messy (see AppleIntelFramebufferController::MapFBToPort).
	 * I am not fully sure why this exists, and recommend setting index to array index (i.e. the sequential number from 0).
	 *
	 * The only accepted values are 0, 1, 2, 3, and -1 (0xFF). When index is equal to array index the logic is simple:
	 * Port with index    0    is always considered built-in (of LVDS type) regardless of any other values.
	 * Ports with indexes 1~3  are checked against type, HDMI will allow the use of digital audio, otherwise DP is assumed.
	 * Port with index    0xFF is ignored and skipped.
	 *
	 * When index != array index port type will be read from connector[index].type.
	 * Say, we have 2 active ports:
	 * 0 - [1]     busId 4 type LVDS
	 * 1 - [2]     busId 5 type DP
	 * 2 - [3]     busId 6 type HDMI
	 * 3 - [-1]    busId 0 type Dummy
	 * This will result in 2 framebuffers which types will be shifted:
	 * 0 - busId 4 type DP
	 * 1 - busId 5 type HDMI
	 * In fact BusId values are also read as connector[index].busId, but are later mapped back via
	 * AppleIntelFramebufferController::getGMBusIDfromPort by looking up a connector with the specified index.
	 * The lookup will stop as soon as a special marker connector (-1) is found. To illustrate, if we have 2 active ports:
	 * 0 - [1]     busId 4 type LVDS
	 * 1 - [2]     busId 5 type DP
	 * 2 - [-1]    busId 6 type HDMI
	 * 3 - [-1]    busId 0 type Dummy
	 * The result will be 2 framebuffers which types and the second busId will be shifted:
	 * 0 - busId 4 type DP
	 * 1 - busId 6 type HDMI
	 * It is also used for port-number calculation.
	 * - LVDS displays (more precisely, displays with CNConnectorAlwaysConnected flag set) get port-number 0.
	 * - Other displays go through index - port-number mapping: 1 - 5, 2 - 6, 3 - 7, or fallback to 0.
	 */
	int8_t index;
	/* Proven by AppleIntelFramebufferController::MapFBToPort, by a call to AppleIntelFramebufferController::getGMBusIDfromPort.
	 * This is GMBUS (Graphic Management Bus) ID described in https://01.org/sites/default/files/documentation/intel-gfx-prm-osrc-hsw-display_0.pdf.
	 * The use could be found in Intel Linux Graphics Driver source code:
	 * https://github.com/torvalds/linux/blob/6481d5ed076e69db83ca75e751ad492a6fb669a7/drivers/gpu/drm/i915/intel_i2c.c#L43
	 * https://github.com/torvalds/linux/blob/605dc7761d2701f73c17183649de0e3044609817/drivers/gpu/drm/i915/i915_reg.h#L3053
	 * However, it should be noted that Apple identifiers are slightly different from Linux driver.
	 * In Linux 0 means disabled, however, for Apple it has some special meaning and is used for internal display.
	 * Other than that the values are the same:
	 * - GMBUS_PIN_DPC    (4)  HDMIC
	 * - GMBUS_PIN_DPB    (5)  SDVO, HDMIB
	 * - GMBUS_PIN_DPD    (6)  HDMID
	 * - GMBUS_PIN_VGADDC (2)  VGA until Broadwell inclusive.
	 * So basically you could use 4, 5, 6 for arbitrary HDMI or DisplayPort displays.
	 * Since 5 supports SDVO (https://en.wikipedia.org/wiki/Serial_Digital_Video_Out), it may also be used to support DVI displays.
	 * Starting with Skylake VGA works via SDVO too (instead of a dedicated GMBUS_PIN_VGADDC id).
	 */
	uint8_t busId;
	/* Appears to be used for grouping ports just like Piker says, but I cannot find the usage. */
	uint8_t pipe;
	uint8_t pad;
	ConnectorType type;
	/* These are connector flags, they have nothing to do with delays regardless of what Piker says.
	 * I tried to describe some in ConnectorFlags.
	 */
	ConnectorFlags flags;
};

// ICL+ connector info — widened to 32-bit fields vs 8-bit in older gens
struct PACKED ConnectorInfoICL {
	uint32_t index;
	uint32_t busId;
	uint32_t pipe;
	uint32_t pad;
	ConnectorType type;
	ConnectorFlags flags;
};
struct PACKED FramebufferCNLCurrents {
	uint32_t value1;
	uint32_t pad;
	uint64_t valu2;
};
// FramebufferICLLP — ICL Low-Power framebuffer descriptor (6 connectors, used by ICL FB kext)
struct PACKED FramebufferICLLP {
	uint32_t framebufferId;
	/* Unclear what values really are, yet 4 stands for non-LP chipset.
	 * See AppleIntelFramebufferController::start.
	 */
	uint32_t fPchType;
	uint64_t fModelNameAddr;
	/* While it is hard to be sure, because having 0 here results in online=true returned by
	 * AppleIntelFramebuffer::GetOnlineInfo, after all it appears to be the case, and the unused
	 * so-called mobile framebufers are simply set to fail-safe defaults.
	 * For some reason it is often called fDisabled...
	 */
	uint8_t  fMobile;
	uint8_t  fPipeCount;
	uint8_t  fPortCount;
	uint8_t  fFBMemoryCount;
	/* This one is per framebuffer fStolenMemorySize * fFBMemoryCount */
	uint32_t fStolenMemorySize;
	/* This is for boot framebuffer from what I can understand */
	uint32_t fFramebufferMemorySize;
	uint32_t fUnifiedMemorySize;
	ConnectorInfoICL connectors[6];
	/* Flags are quite different in ICL now */
	union { uint32_t value; } flags;
	uint32_t unk2;
	FramebufferCNLCurrents currents[3];
	uint32_t unk3[2];
	uint32_t camelliaVersion;
	uint32_t unk4[3];
	uint32_t fNumTransactionsThreshold;
	/* Defaults to 14, used when UseVideoTurbo bit is set */
	uint32_t fVideoTurboFreq;
	uint32_t fSliceCount;
	uint32_t fEuCount;
	uint32_t unk5;
	uint8_t unk6;
	uint8_t pad[3];
};

// FramebufferICL — TGL framebuffer descriptor (3 connectors, different layout from ICLLP)
struct PACKED FramebufferICL {

	uint32_t framebufferId;

	uint32_t fPchType;
	uint64_t fModelNameAddr;

	uint8_t  fMobile;
	uint8_t  fPipeCount;
	uint8_t  fPortCount;
	uint8_t  fFBMemoryCount;

	uint32_t fStolenMemorySize;
	uint32_t fFramebufferMemorySize;
	uint32_t fUnifiedMemorySize;
	
	ConnectorInfoICL connectors[3];//144 bytes
	uint64_t flags;
	uint64_t empty0;
	
	uint64_t combo1;
	uint64_t empty1;
	uint64_t combo2;
	uint64_t empty2;
	uint64_t combo3;
	uint64_t field1;
	uint32_t camelliaVersion;
	uint32_t fNumTransactionsThreshold;
	uint32_t fVideoTurboFreq;
	uint32_t empty3;
	uint32_t empty4;
	uint32_t empty5;
	uint32_t slice;
	uint32_t eu;
	uint32_t subslice;
	uint32_t empty6;
	
};

// Platform Controller Hub identification — determines south display engine variant
enum intel_pch {
	PCH_NOP = -1,	/* PCH without south display */
	PCH_NONE = 0,	/* No PCH present */
	PCH_IBX,	/* Ibexpeak PCH */
	PCH_CPT,	/* Cougarpoint/Pantherpoint PCH */
	PCH_LPT,	/* Lynxpoint/Wildcatpoint PCH */
	PCH_SPT,        /* Sunrisepoint/Kaby Lake PCH */
	PCH_CNP,        /* Cannon/Comet Lake PCH */
	PCH_ICP,	/* Ice Lake/Jasper Lake PCH */
	PCH_TGP,	/* Tiger Lake/Mule Creek Canyon PCH */
	PCH_ADP,	/* Alder Lake PCH */

	/* Fake PCHs, functionality handled on the same PCI dev */
	PCH_DG1 = 1024,
	PCH_DG2,
	PCH_MTL,
	PCH_LNL,
};



// ═══════════════════════════════════════════════════════════════════════════
// Gen11 — Lilu route/patch class for the admitted TGL framebuffer and accelerator kexts.
// Each "static ... / mach_vm_address_t o..." pair is a Lilu function route:
//   static method = our wrapper, mach_vm_address_t = saved original pointer.
// ═══════════════════════════════════════════════════════════════════════════
class Gen11 {

private:

	// ── GuC (Graphics micro-Controller) firmware ──
	static unsigned long loadGuCBinary(void *that);  // route: intercept GuC FW load

	// ── Accelerator firmware & scheduler ──
	static bool vfMmioHostToGuCAction(void *that, const uint32_t *request,
	                                  unsigned int requestLength, int timeout,
	                                  uint32_t *response);
	static bool vfLegacyHostToGuCAction(void *that, const uint32_t *request,
	                                  unsigned int requestLength, int timeout,
	                                  uint32_t *response);
	static uint32_t vfCreateUkContext(void *that, uint64_t owner, int priority);
	mach_vm_address_t vfAllocContext {};
	mach_vm_address_t vfReleaseContext {};
	mach_vm_address_t vfSharedMappedBufferWithOptions {};
	mach_vm_address_t vfWorkQueueWithOptions {};
	static bool vfWorkQueueInit(void *that, void *accelerator, uint32_t id, void *process);
	mach_vm_address_t oVfWorkQueueInit {};
	static void vfWorkQueueFree(void *that);
	mach_vm_address_t oVfWorkQueueFree {};
	mach_vm_address_t vfOSObjectFree {};
	static void vfCtbFree(void *that);
	mach_vm_address_t oVfCtbFree {};
	static uint32_t vfAllocContextId(void *that, uint64_t owner, bool clear);
	static void vfReleaseContextId(void *that, uint32_t id);
	static uint16_t vfAcquireDoorbell(void *that, void *descriptor, bool pin);
	static void vfReleaseDoorbell(void *that, void *descriptor);
	static bool vfAllocUkDoorbell(void *that, uint32_t contextId, bool pin);
	static uint16_t vfReacquireDoorbell(void *that, uint32_t contextId);
	static bool vfIsGuCIdle(void *that);
	static bool vfIsContextIdle(void *that, uint32_t contextId);
	static bool vfIsKmdContextIdle(void *that, const uint32_t *descriptor);
	static void vfTransferOwnership(void *that, const void *backing, int owner);
	static void vfInitDoorbells(void *that);
	static bool vfReadDoorbellSQIDIConfig(void *that);
	static bool vfCtbInitWithAccelerator(void *that, void *accelerator);
	mach_vm_address_t oVfCtbInitWithAccelerator {};
	static void vfCtbChannelInit(void *that);
	static bool vfCtbGucToHostAction(void *that, uint32_t *message);
	static void vfSoftwareGuCInterrupt(void *that, IOInterruptEventSource *source, int count);
	mach_vm_address_t vfCtbSoftwareInterrupt {};
	static void vfInvalidateTLB(void *that);
	static void vfBaseInvalidateTLB(const void *that);
	static bool vfInterruptFilterHandler(void *that, void *eventSource);
	mach_vm_address_t vfServiceInterrupts {};
	static void vfReadAndClearInterrupts(void *that, void *interrupts);
	static void vfEnableInterrupts(void *that);
	static void vfDisableInterrupts(void *that);
	static void *vfCtbMappedBufferWithOptions(void *accelTask, unsigned long size,
	                                          unsigned int type, unsigned int flags);
	mach_vm_address_t oVfCtbMappedBufferWithOptions {};
	static bool vfAttachContextDesc(void *that, const uint32_t *descriptor);
	mach_vm_address_t oVfAttachContextDesc {};
	static void vfDetachContextDesc(void *that, const uint32_t *descriptor);
	mach_vm_address_t oVfDetachContextDesc {};
	static bool vfSubmitWorkItem(void *that, unsigned int legacyContextId,
	                             const uint32_t *descriptor, IGHwCsType hwCsType,
	                             unsigned int channelId, unsigned int ringSequence,
	                             unsigned int ringTail);
	mach_vm_address_t oVfSubmitWorkItem {};
	mach_vm_address_t vfSharedMappedBufferGetVirtualAddress {};
	mach_vm_address_t vfMappedBufferGetGPUVirtualAddress {};
	
	// ── Accelerator start & forcewake ──
	static unsigned long start(void *that,void  *param_1);   // IntelAccelerator::start wrapper
	static void acceleratorStop(void *that, void *provider); // final VF DMA-quiescence intent
	mach_vm_address_t ostart {};
	mach_vm_address_t oAcceleratorStop {};


	static void *igAccelTaskWithOptions(void *that);  // V216: repair VF bootstrap identity and propagate allocation failures
	mach_vm_address_t oigAccelTaskWithOptions {};
	mach_vm_address_t igAccelTaskCounter {};  // V216: IGAccelTask::fTaskCounter, repaired before VF kernel-task construction

	static bool IGAccelTaskIsKernelGPUTask(const void *that);  // V214: bootstrap the first VF task from Global GTT
	mach_vm_address_t oIGAccelTaskIsKernelGPUTask {};

	static bool submitBlit(void *that, void *param_1, void *param_2, void *param_3, bool param_4);
	mach_vm_address_t osubmitBlit {};
	
	static void forceWake(void *that, bool set, uint32_t dom, uint8_t ctx);
	static void wrapSafeForceWake(void *that, bool set, uint32_t dom);
	
	
	// Saved original function pointers for accelerator
	mach_vm_address_t orgInitSchedControl {};  // scheduler init original
	
	// ── Display buffer & memory management ──
	static bool IGHardwareGlobalPageTableInitWithOptions(void *that,
	                                                    void *accelerator,
	                                                    const NGIGAddressRange &range,
	                                                    void *mmioBase,
	                                                    uint64_t dummyPage,
	                                                    uint32_t options);
	mach_vm_address_t oIGHardwareGlobalPageTableInitWithOptions {};

	// V217: Gen12 VF direct-GGTT bridge. Apple keeps its normal Gen11 page-table
	// algorithms, but all ranges and DMA addresses are checked against the
	// PF-provisioned assignment before the BAR0 PTE aperture can be written.
	static bool IGMemoryManagerInitSegments(void *that);
	static bool IGHardwareGlobalPageTableMapRange(void *that,
	                                              const NGIGAddressRange &range,
	                                              uint64_t physical,
	                                              uint64_t flags);
	static bool IGHardwareGlobalPageTableMapRangeRotated(void *that,
	                                                     void *rangeIterator,
	                                                     void *physicalIterator,
	                                                     uint64_t flags);
	static void IGHardwareGlobalPageTableUnmapRange(void *that,
	                                                const NGIGAddressRange &range);
	static bool IGHardwareGlobalPageTableMapRangeDummy(void *that,
	                                                   const NGIGAddressRange &range,
	                                                   uint64_t flags);

	// ── 3D Blit engine (GPU-accelerated blitting via 3D pipeline) ──
	mach_vm_address_t oIGMappedBuffergetMemory {};

	static void * getBlit2DContext(void *that,bool param_1);
	mach_vm_address_t ogetBlit2DContext {};

	static void * getDepthResolveContext(void *that,bool param_1);
	mach_vm_address_t ogetDepthResolveContext {};

	static void * getColorResolveContext(void *that,bool param_1);
	mach_vm_address_t ogetColorResolveContext {};
	
	static void * getBlit3DContext(void *that,bool param_1);
	mach_vm_address_t ogetBlit3DContext {};
	
	// IntelAccelerator personality registration in IOCatalogue. Lives in the TGL
	// HW-kext path, not Framebuffer — must run before the FBController's
	// registerService() so IOKit can match IntelAccelerator. Idempotent.
	void injectAcceleratorPersonality();
	bool acceleratorPersonalityInjected {false};
	
	static unsigned long stopGraphicsEngine(void *that);

	static unsigned long startGraphicsEngine(void *that);  // contain PF-owned ring lifecycle on a VF

	static void populateResetRegisterList(void *that);  // contain PF-owned reset-register state on a VF

	// A VF has no guest-owned INSTDONE state. Report idle only from the tracked
	// direct-LRCA lifecycle; physical schedulers keep their native implementations.
	static bool wrapIGScheduler5IsGpuIdle(const void *that);
	static bool wrapIGScheduler4IsGpuIdle(const void *that);


	static uint8_t barrierSubmission(void *queue, void *accelerator, void *cmdDesc,
	                                void *event, uint16_t count, const uint16_t *list);
	mach_vm_address_t obarrierSubmission {};
	
public:

	// Resolved from IOAcceleratorFamily2 by NGreen::processKext — needed by blit3d scratch init.
	void init();  // register kextInfos with Lilu
	static Gen11 *callback;  // singleton for Lilu static callbacks
	bool processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size);
	
	// Direct MMIO access helpers (bypass the kext's register methods)
	static void tWriteRegister32(unsigned long a, unsigned int b);
	static void tWriteRegister64(void volatile* a, unsigned long b, unsigned long long c);
	static unsigned int tReadRegister32(unsigned long a);
	static unsigned long long tReadRegister64(void volatile* a, unsigned long b);
	static uint64_t tgetPMTNow();              // read GT timestamp
	static bool thwSetupDSBMemory();           // DSB = Display State Buffer
};

#endif /* kern_gen8_hpp */
