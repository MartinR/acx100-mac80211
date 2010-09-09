/***********************************************************************
 ** Copyright (C) 2003  ACX100 Open Source Project
 **
 ** The contents of this file are subject to the Mozilla Public
 ** License Version 1.1 (the "License"); you may not use this file
 ** except in compliance with the License. You may obtain a copy of
 ** the License at http://www.mozilla.org/MPL/
 **
 ** Software distributed under the License is distributed on an "AS
 ** IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or
 ** implied. See the License for the specific language governing
 ** rights and limitations under the License.
 **
 ** Alternatively, the contents of this file may be used under the
 ** terms of the GNU Public License version 2 (the "GPL"), in which
 ** case the provisions of the GPL are applicable instead of the
 ** above.  If you wish to allow the use of your version of this file
 ** only under the terms of the GPL and not to allow others to use
 ** your version of this file under the MPL, indicate your decision
 ** by deleting the provisions above and replace them with the notice
 ** and other provisions required by the GPL.  If you do not delete
 ** the provisions above, a recipient may use your version of this
 ** file under either the MPL or the GPL.
 ** ---------------------------------------------------------------------
 ** Inquiries regarding the ACX100 Open Source Project can be
 ** made directly to:
 **
 ** acx100-users@lists.sf.net
 ** http://acx100.sf.net
 ** ---------------------------------------------------------------------
 **
 ** Slave memory interface support:
 **
 ** Todd Blumer - SDG Systems
 ** Bill Reese - HP
 ** Eric McCorkle - Shadowsun
 */

#define ACX_MAC80211_MEM 1

#include <linux/version.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 33)
#include <linux/utsrelease.h>
#else
#include <generated/utsrelease.h>
#endif

#include <linux/compiler.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/wireless.h>
#include <linux/netdevice.h>
#include <linux/ioport.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include <linux/nl80211.h>

#include <net/iw_handler.h>
#include <net/mac80211.h>

#include <asm/io.h>

#include "acx.h"

/*
 * BOM Config
 * ==================================================
 */

// OW PM for later
#undef CONFIG_PM

/*
 * non-zero makes it dump the ACX memory to the console then
 * panic when you cat /proc/driver/acx_wlan0_diag
 */
#define DUMP_MEM_DEFINED 1

#define DUMP_MEM_DURING_DIAG 0
#define DUMP_IF_SLOW 0

// OW #define PATCH_AROUND_BAD_SPOTS 1
#define PATCH_AROUND_BAD_SPOTS 1
#define HX4700_FIRMWARE_CHECKSUM 0x0036862e
#define HX4700_ALTERNATE_FIRMWARE_CHECKSUM 0x00368a75

/* Pick one */
/* #define INLINE_IO static */
#define INLINE_IO static inline

#define FW_NO_AUTO_INCREMENT	1

#define MAX_IRQLOOPS_PER_JIFFY  (20000/HZ)

/*
 * BOM Prototypes
 * ... static and also none-static for overview reasons (maybe not best practice ...)
 * ==================================================
 */

// Logging
static void acxmem_log_rxbuffer(const acx_device_t *adev);
static void acxmem_log_txbuffer(acx_device_t *adev);
#if DUMP_MEM_DEFINED > 0
static void acxmem_dump_mem(acx_device_t *adev, u32 start, int length);
#endif

// Data Access
INLINE_IO u32 read_id_register(acx_device_t *adev);
INLINE_IO u32 read_reg32(acx_device_t *adev, unsigned int offset);
INLINE_IO u16 read_reg16(acx_device_t *adev, unsigned int offset);
INLINE_IO u8 read_reg8(acx_device_t *adev, unsigned int offset);
INLINE_IO void write_reg32(acx_device_t *adev, unsigned int offset, u32 val);
INLINE_IO void write_reg16(acx_device_t *adev, unsigned int offset, u16 val);
INLINE_IO void write_reg8(acx_device_t *adev, unsigned int offset, u8 val);
INLINE_IO void write_flush(acx_device_t *adev);
INLINE_IO void set_regbits(acx_device_t *adev, unsigned int offset, u32 bits);
INLINE_IO void clear_regbits(acx_device_t *adev, unsigned int offset, u32 bits);
INLINE_IO void write_slavemem32(acx_device_t *adev, u32 slave_address, u32 val);
INLINE_IO u32 read_slavemem32(acx_device_t *adev, u32 slave_address);
INLINE_IO void write_slavemem8(acx_device_t *adev, u32 slave_address, u8 val);
INLINE_IO u8 read_slavemem8(acx_device_t *adev, u32 slave_address);
INLINE_IO void write_slavemem16(acx_device_t *adev, u32 slave_address, u16 val);
INLINE_IO u16 read_slavemem16(acx_device_t *adev, u32 slave_address);
static void acxmem_copy_from_slavemem(acx_device_t *adev, u8 *destination, u32 source, int count);
static void acxmem_copy_to_slavemem(acx_device_t *adev, u32 destination, u8 *source, int count);
static void acxmem_chaincopy_to_slavemem(acx_device_t *adev, u32 destination, u8 *source, int count);
static void acxmem_chaincopy_from_slavemem(acx_device_t *adev, u8 *destination, u32 source, int count);

int acxmem_create_hostdesc_queues(acx_device_t *adev);
static int acxmem_create_rx_host_desc_queue(acx_device_t *adev);
static int acxmem_create_tx_host_desc_queue(acx_device_t *adev);
void acxmem_create_desc_queues(acx_device_t *adev, u32 tx_queue_start, u32 rx_queue_start);
static void acxmem_create_rx_desc_queue(acx_device_t *adev, u32 rx_queue_start);
static void acxmem_create_tx_desc_queue(acx_device_t *adev, u32 tx_queue_start);
void acxmem_free_desc_queues(acx_device_t *adev);
static void acxmem_delete_dma_regions(acx_device_t *adev);
static void *acxmem_allocate(acx_device_t *adev, size_t size, dma_addr_t *phy, const char *msg);

// Firmware, EEPROM, Phy
int acxmem_upload_radio(acx_device_t *adev);
int acxmem_read_eeprom_byte(acx_device_t *adev, u32 addr, u8 *charbuf);
#ifdef UNUSED
int acxmem_s_write_eeprom(acx_device_t *adev, u32 addr, u32 len, const u8 *charbuf);
#endif
static inline void acxmem_read_eeprom_area(acx_device_t *adev);
int acxmem_read_phy_reg(acx_device_t *adev, u32 reg, u8 *charbuf);
int acxmem_write_phy_reg(acx_device_t *adev, u32 reg, u8 value);
static int acxmem_write_fw(acx_device_t *adev, const firmware_image_t *fw_image, u32 offset);
static int acxmem_validate_fw(acx_device_t *adev, const firmware_image_t *fw_image, u32 offset);
static int acxmem_upload_fw(acx_device_t *adev);

#if defined(NONESSENTIAL_FEATURES)
static void acx_show_card_eeprom_id(acx_device_t *adev);
#endif

// CMDs (Control Path)
int acxmem_issue_cmd_timeo_debug(acx_device_t *adev, unsigned cmd, void *buffer, unsigned buflen, unsigned cmd_timeout, const char* cmdstr);
static inline void acxmem_write_cmd_type_status(acx_device_t *adev, u16 type, u16 status);
static u32 acxmem_read_cmd_type_status(acx_device_t *adev);
static inline void acxmem_init_mboxes(acx_device_t *adev);

// Init, Configure (Control Path)
int acxmem_reset_dev(acx_device_t *adev);
static int acxmem_verify_init(acx_device_t *adev);
static int acxmem_complete_hw_reset(acx_device_t *adev);
static void acxmem_reset_mac(acx_device_t *adev);
static void acxmem_up(struct ieee80211_hw *hw);
//static void acxmem_i_set_multicast_list(struct net_device *ndev);

// Other (Control Path)

// Proc, Debug
int acxmem_proc_diag_output(struct seq_file *file, acx_device_t *adev);
int acxmem_proc_eeprom_output(char *buf, acx_device_t *adev);

// Rx Path
static void acxmem_process_rxdesc(acx_device_t *adev);

// Tx Path
tx_t *acxmem_alloc_tx(acx_device_t *adev, unsigned int len);
void acxmem_dealloc_tx(acx_device_t *adev, tx_t *tx_opaque);

void *acxmem_get_txbuf(acx_device_t *adev, tx_t *tx_opaque);
static int acxmem_get_txbuf_space_needed(acx_device_t *adev, unsigned int len);
static u32 acxmem_allocate_acx_txbuf_space(acx_device_t *adev, int count);
static void acxmem_reclaim_acx_txbuf_space(acx_device_t *adev, u32 blockptr);
static void acxmem_init_acx_txbuf(acx_device_t *adev);
void acxmem_init_acx_txbuf2(acx_device_t *adev);
static inline txdesc_t *acxmem_get_txdesc(acx_device_t *adev, int index);
static inline txdesc_t *acxmem_advance_txdesc(acx_device_t *adev, txdesc_t* txdesc, int inc);
static txhostdesc_t *acxmem_get_txhostdesc(acx_device_t *adev, txdesc_t* txdesc);

void acxmem_tx_data(acx_device_t *adev, tx_t *tx_opaque, int len, struct ieee80211_tx_info *ieeectl, struct sk_buff *skb);
unsigned int acxmem_clean_txdesc(acx_device_t *adev);
void acxmem_clean_txdesc_emergency(acx_device_t *adev);

void acxmem_update_queue_indicator(acx_device_t *adev, int txqueue);
int acx100mem_set_tx_level(acx_device_t *adev, u8 level_dbm);
//static void acxmem_i_tx_timeout(struct net_device *ndev);

// Irq Handling, Timer
static void acxmem_irq_enable(acx_device_t *adev);
static void acxmem_irq_disable(acx_device_t *adev);
void acxmem_irq_work(struct work_struct *work);
static irqreturn_t acxmem_i_interrupt(int irq, void *dev_id);
static void acxmem_handle_info_irq(acx_device_t *adev);
void acxmem_set_interrupt_mask(acx_device_t *adev);

// Mac80211 Ops
static int acxmem_e_op_start(struct ieee80211_hw *hw);
static void acxmem_e_op_stop(struct ieee80211_hw *hw);

// Helpers
void acxmem_l_power_led(acx_device_t *adev, int enable);
INLINE_IO int acxmem_adev_present(acx_device_t *adev);
static char acxmem_printable(char c);
//static void update_link_quality_led(acx_device_t *adev);

// Ioctls
//int acx111pci_ioctl_info(struct ieee80211_hw *hw, struct iw_request_info *info, struct iw_param *vwrq, char *extra);
//int acx100mem_ioctl_set_phy_amp_bias(struct ieee80211_hw *hw, struct iw_request_info *info, struct iw_param *vwrq, char *extra);

// Driver, Module
static int __devinit acxmem_e_probe(struct platform_device *pdev);
static int __devexit acxmem_e_remove(struct platform_device *pdev);
#ifdef CONFIG_PM
static int acxmem_e_suspend(struct platform_device *pdev, pm_message_t state);
static int acxmem_e_resume(struct platform_device *pdev);
#endif
int __init acxmem_e_init_module(void);
void __exit acxmem_e_cleanup_module(void);

/*
 * BOM Defines, static vars, etc.
 * ==================================================
 */

#define CARD_EEPROM_ID_SIZE 6
#define REG_ACX_VENDOR_ID 0x900

// This is the vendor id on the HX4700, anyway
#define ACX_VENDOR_ID 0x8400104c

typedef enum {
	ACX_SOFT_RESET = 0,
	ACX_SLV_REG_ADDR, ACX_SLV_REG_DATA, ACX_SLV_REG_ADATA,
	ACX_SLV_MEM_CP, ACX_SLV_MEM_ADDR, ACX_SLV_MEM_DATA, ACX_SLV_MEM_CTL,
} acxreg_t;

//OW 20090815 #define WLAN_A4FR_MAXLEN_WEP_FCS	(30 + 2312 + 4)

#define RX_BUFFER_SIZE (sizeof(rxbuffer_t) + 32)

static const u16 IO_ACX100[] = {
0x0000, /* IO_ACX_SOFT_RESET */

0x0014, /* IO_ACX_SLV_MEM_ADDR */
0x0018, /* IO_ACX_SLV_MEM_DATA */
0x001c, /* IO_ACX_SLV_MEM_CTL */
0x0020, /* IO_ACX_SLV_END_CTL */

0x0034, /* IO_ACX_FEMR */

0x007c, /* IO_ACX_INT_TRIG */
0x0098, /* IO_ACX_IRQ_MASK */
0x00a4, /* IO_ACX_IRQ_STATUS_NON_DES */
0x00a8, /* IO_ACX_IRQ_REASON */
0x00ac, /* IO_ACX_IRQ_ACK */
0x00b0, /* IO_ACX_HINT_TRIG */

0x0104, /* IO_ACX_ENABLE */

0x0250, /* IO_ACX_EEPROM_CTL */
0x0254, /* IO_ACX_EEPROM_ADDR */
0x0258, /* IO_ACX_EEPROM_DATA */
0x025c, /* IO_ACX_EEPROM_CFG */

0x0268, /* IO_ACX_PHY_ADDR */
0x026c, /* IO_ACX_PHY_DATA */
0x0270, /* IO_ACX_PHY_CTL */

0x0290, /* IO_ACX_GPIO_OE */

0x0298, /* IO_ACX_GPIO_OUT */

0x02a4, /* IO_ACX_CMD_MAILBOX_OFFS */
0x02a8, /* IO_ACX_INFO_MAILBOX_OFFS */
0x02ac, /* IO_ACX_EEPROM_INFORMATION */

0x02d0, /* IO_ACX_EE_START */
0x02d4, /* IO_ACX_SOR_CFG */
0x02d8 /* IO_ACX_ECPU_CTRL */
};

static const u16 IO_ACX111[] = { 0x0000, /* IO_ACX_SOFT_RESET */

0x0014, /* IO_ACX_SLV_MEM_ADDR */
0x0018, /* IO_ACX_SLV_MEM_DATA */
0x001c, /* IO_ACX_SLV_MEM_CTL */
0x0020, /* IO_ACX_SLV_END_CTL */

0x0034, /* IO_ACX_FEMR */

0x00b4, /* IO_ACX_INT_TRIG */
0x00d4, /* IO_ACX_IRQ_MASK */
/* we do mean NON_DES (0xf0), not NON_DES_MASK which is at 0xe0: */
0x00f0, /* IO_ACX_IRQ_STATUS_NON_DES */
0x00e4, /* IO_ACX_IRQ_REASON */
0x00e8, /* IO_ACX_IRQ_ACK */
0x00ec, /* IO_ACX_HINT_TRIG */

0x01d0, /* IO_ACX_ENABLE */

0x0338, /* IO_ACX_EEPROM_CTL */
0x033c, /* IO_ACX_EEPROM_ADDR */
0x0340, /* IO_ACX_EEPROM_DATA */
0x0344, /* IO_ACX_EEPROM_CFG */

0x0350, /* IO_ACX_PHY_ADDR */
0x0354, /* IO_ACX_PHY_DATA */
0x0358, /* IO_ACX_PHY_CTL */

0x0374, /* IO_ACX_GPIO_OE */

0x037c, /* IO_ACX_GPIO_OUT */

0x0388, /* IO_ACX_CMD_MAILBOX_OFFS */
0x038c, /* IO_ACX_INFO_MAILBOX_OFFS */
0x0390, /* IO_ACX_EEPROM_INFORMATION */

0x0100, /* IO_ACX_EE_START */
0x0104, /* IO_ACX_SOR_CFG */
0x0108, /* IO_ACX_ECPU_CTRL */
};


/*
 * BOM Logging
 * ==================================================
 */

static void acxmem_log_rxbuffer(const acx_device_t *adev) {
	register const struct rxhostdesc *rxhostdesc;
	int i;
	/* no FN_ENTER here, we don't want that */

	rxhostdesc = adev->rxhostdesc_start;
	if (unlikely(!rxhostdesc)) return;
	for (i = 0; i < RX_CNT; i++) {
		if ((rxhostdesc->Ctl_16 & cpu_to_le16(DESC_CTL_HOSTOWN))
		    && (rxhostdesc->Status & cpu_to_le32(DESC_STATUS_FULL)))
			printk("acx: rx: buf %d full\n", i);
		rxhostdesc++;
	}
}

static void acxmem_log_txbuffer(acx_device_t *adev) {
	txdesc_t *txdesc;
	int i;
	u8 Ctl_8;

	/* no FN_ENTER here, we don't want that */
	/* no locks here, since it's entirely non-critical code */
	txdesc = adev->txdesc_start;
	if (unlikely(!txdesc))
			return;
	printk("acx: tx: desc->Ctl8's: ");
	for (i = 0; i < TX_CNT; i++) {
		Ctl_8 = read_slavemem8(adev, (u32) &(txdesc->Ctl_8));
		printk("%02X ", Ctl_8);
		txdesc = acxmem_advance_txdesc(adev, txdesc, 1);
	}
	printk("\n");
}

#if DUMP_MEM_DEFINED > 0
static void acxmem_dump_mem(acx_device_t *adev, u32 start, int length) {
	int i;
	u8 buf[16];

	while (length > 0) {
		printk("%04x ", start);
		acxmem_copy_from_slavemem(adev, buf, start, 16);
		for (i = 0; (i < 16) && (i < length); i++) {
			printk("%02x ", buf[i]);
		}
		for (i = 0; (i < 16) && (i < length); i++) {
			printk("%c", acxmem_printable(buf[i]));
		}
		printk("\n");
		start += 16;
		length -= 16;
	}
}
#endif

/*
 * Locking in mem
 * ==================================================
 */

/*
* Locking in mem is more complex as for pci, because the different data-access 
* functions below need to be protected against incoming interrupts. 
* 
* Data-access on the mem device is always going in serveral, none-atomic steps, 
* involving 2 or more register writes (e.g. ACX_SLV_REG_ADDR, ACX_SLV_REG_DATA).
*
* If an interrupt is serviced while a data-access function is ongoing, this 
* may give access interference with by the involved operations, since the 
* irq routine is also using the same data-access functions.
* 
* In case of interference, this often manifests during driver operations as
* failure of device cmds and subsequent hanging of the device. It especially 
* appeared during sw-scans while a connection was up. 
* 
* For this reason, irqs shall be off while data access functions are executed,
* and for this we'll use the acx-spinlock.
*
* In pci we don't have this problem, because all data-access functions are 
* atomic enough and we use dma (and the sw-scan problem is also not observed in 
* pci, which indicates confirmation).
* 
* Apart from this, the pure acx-sem locking is already coordinating accesses 
* well enough, that simple driver operation without inbetween scans work without 
* problems.
*
* Different locking approaches a possible to solves this (e.g. fine vs 
* coarse-grained). 
* 
* The chosen approach is:
* 
* 1) Mem.c data-access functions contain all a check to insure, they are executed 
* under the acx-spinlock. 
* => This is the red line that tells, if something needs coverage.
* 
* 2) The scope of acx-spinlocking is local, in this case here only to mem.c. 
* All common.c functions are already protected by the sem.
*
* 3) In order to consolidate locking calls and also to account for the logic 
* of the various write_flush() calls around, locking in mem should be: 
* 
* a) as coarse-grained as possible, and ...
* 
* b) ... as fine-grained as required. Basically that means, that before 
* functions, that sleep, unlocking needs to be done. And locking is taken up 
* again inside the sleeping function. Specifically the cmd-functions are used 
* in this path.
*
* Once stable, the locking checks in the data-access functions could be #defined
* away. Mem.c is anyway more used two smaller cpus (pxa UP e.g.), so the implied 
* runtime constraints by the lock won't take much effect. 
* 
*/

#define acxmem_lock_flags		unsigned long flags
#define acxmem_lock()			spin_lock_irqsave(&adev->spinlock, flags)
#define acxmem_unlock()			spin_unlock_irqrestore(&adev->spinlock, flags)

/*
 * BOM Data Access
 * 
 * => See locking comment above  
 * ==================================================
 */

/* OS I/O routines *always* be endianness-clean but having them doesn't hurt */
#define acx_readl(v)	le32_to_cpu(readl((v)))
#define acx_readw(v)	le16_to_cpu(readw((v)))
#define acx_writew(v,r)	writew(le16_to_cpu((v)), r)
#define acx_writel(v,r)	writel(le32_to_cpu((v)), r)

#define ACXMEM_WARN_NOT_SPIN_LOCKED \
do { \
	if (!spin_is_locked(&adev->spinlock)){\
		logf0(L_ANY, "mem: warning: data access not locked!\n");\
		dump_stack(); \
	}\
} while (0)

INLINE_IO u32 read_id_register(acx_device_t *adev) {
	ACXMEM_WARN_NOT_SPIN_LOCKED;
	acx_writel (0x24, &adev->iobase[ACX_SLV_REG_ADDR]);
	return acx_readl (&adev->iobase[ACX_SLV_REG_DATA]);
}

INLINE_IO u32 read_reg32(acx_device_t *adev, unsigned int offset) {
	u32 val;
	u32 addr;

	ACXMEM_WARN_NOT_SPIN_LOCKED;

	if (offset > IO_ACX_ECPU_CTRL)
		addr = offset;
	else
		addr = adev->io[offset];

	if (addr < 0x20) {
		return acx_readl(((u8*)adev->iobase) + addr);
	}

	acx_writel( addr, &adev->iobase[ACX_SLV_REG_ADDR] );
	val = acx_readl( &adev->iobase[ACX_SLV_REG_DATA] );

	return val;
}

INLINE_IO u16 read_reg16(acx_device_t *adev, unsigned int offset) {
	u16 lo;
	u32 addr;

	ACXMEM_WARN_NOT_SPIN_LOCKED;

	if (offset > IO_ACX_ECPU_CTRL)
		addr = offset;
	else
		addr = adev->io[offset];

	if (addr < 0x20) {
		return acx_readw(((u8 *) adev->iobase) + addr);
	}

	acx_writel( addr, &adev->iobase[ACX_SLV_REG_ADDR] );
	lo = acx_readw( (u16 *)&adev->iobase[ACX_SLV_REG_DATA] );

	return lo;
}

INLINE_IO u8 read_reg8(acx_device_t *adev, unsigned int offset) {
	u8 lo;
	u32 addr;

	ACXMEM_WARN_NOT_SPIN_LOCKED;

	if (offset > IO_ACX_ECPU_CTRL)
		addr = offset;
	else
		addr = adev->io[offset];

	if (addr < 0x20)
		return readb(((u8 *)adev->iobase) + addr);

	acx_writel( addr, &adev->iobase[ACX_SLV_REG_ADDR] );
	lo = acx_readw( (u8 *)&adev->iobase[ACX_SLV_REG_DATA] );

	return (u8) lo;
}

INLINE_IO void write_reg32(acx_device_t *adev, unsigned int offset, u32 val) {
	u32 addr;

	ACXMEM_WARN_NOT_SPIN_LOCKED;

	if (offset > IO_ACX_ECPU_CTRL)
		addr = offset;
	else
		addr = adev->io[offset];

	if (addr < 0x20) {
		acx_writel(val, ((u8*)adev->iobase) + addr);
		return;
	}

	acx_writel( addr, &adev->iobase[ACX_SLV_REG_ADDR] );
	acx_writel( val, &adev->iobase[ACX_SLV_REG_DATA] );
}

INLINE_IO void write_reg16(acx_device_t *adev, unsigned int offset, u16 val) {
	u32 addr;

	ACXMEM_WARN_NOT_SPIN_LOCKED;

	if (offset > IO_ACX_ECPU_CTRL)
		addr = offset;
	else
		addr = adev->io[offset];

	if (addr < 0x20) {
		acx_writew(val, ((u8 *)adev->iobase) + addr);
		return;
	}
	acx_writel( addr, &adev->iobase[ACX_SLV_REG_ADDR] );
	acx_writew( val, (u16 *) &adev->iobase[ACX_SLV_REG_DATA] );
}

INLINE_IO void write_reg8(acx_device_t *adev, unsigned int offset, u8 val) {
	u32 addr;

	ACXMEM_WARN_NOT_SPIN_LOCKED;

	if (offset > IO_ACX_ECPU_CTRL)
		addr = offset;
	else
		addr = adev->io[offset];

	if (addr < 0x20) {
		writeb(val, ((u8 *) adev->iobase) + addr);
		return;
	}
	acx_writel( addr, &adev->iobase[ACX_SLV_REG_ADDR] );
	writeb( val, (u8 *)&adev->iobase[ACX_SLV_REG_DATA] );
}

/* Handle PCI posting properly:
 * Make sure that writes reach the adapter in case they require to be executed
 * *before* the next write, by reading a random (and safely accessible) register.
 * This call has to be made if there is no read following (which would flush the data
 * to the adapter), yet the written data has to reach the adapter immediately. */
INLINE_IO void write_flush(acx_device_t *adev) {
	/* readb(adev->iobase + adev->io[IO_ACX_INFO_MAILBOX_OFFS]); */
	/* faster version (accesses the first register, IO_ACX_SOFT_RESET,
	 * which should also be safe): */
	ACXMEM_WARN_NOT_SPIN_LOCKED;
	(void) acx_readl(adev->iobase);
}

INLINE_IO void set_regbits(acx_device_t *adev, unsigned int offset, u32 bits) {
	u32 tmp;

	ACXMEM_WARN_NOT_SPIN_LOCKED;

	tmp = read_reg32(adev, offset);
	tmp = tmp | bits;
	write_reg32(adev, offset, tmp);
	write_flush(adev);
}

INLINE_IO void clear_regbits(acx_device_t *adev, unsigned int offset, u32 bits) {
	u32 tmp;

	ACXMEM_WARN_NOT_SPIN_LOCKED;

	tmp = read_reg32(adev, offset);
	tmp = tmp & ~bits;
	write_reg32(adev, offset, tmp);
	write_flush(adev);
}

/*
 * Copy from PXA memory to the ACX memory.  This assumes both the PXA and ACX
 * addresses are 32 bit aligned.  Count is in bytes.
 */
INLINE_IO void write_slavemem32(acx_device_t *adev, u32 slave_address, u32 val) {
	ACXMEM_WARN_NOT_SPIN_LOCKED;

	write_reg32(adev, IO_ACX_SLV_MEM_CTL, 0x0);
	write_reg32(adev, IO_ACX_SLV_MEM_ADDR, slave_address);
	udelay (10);
	write_reg32(adev, IO_ACX_SLV_MEM_DATA, val);
}

INLINE_IO u32 read_slavemem32(acx_device_t *adev, u32 slave_address) {
	u32 val;

	ACXMEM_WARN_NOT_SPIN_LOCKED;

	write_reg32(adev, IO_ACX_SLV_MEM_CTL, 0x0);
	write_reg32(adev, IO_ACX_SLV_MEM_ADDR, slave_address);
	udelay (10);
	val = read_reg32(adev, IO_ACX_SLV_MEM_DATA);

	return val;
}

INLINE_IO void write_slavemem8(acx_device_t *adev, u32 slave_address, u8 val) {
	u32 data;
	u32 base;
	int offset;

	ACXMEM_WARN_NOT_SPIN_LOCKED;

	/*
	 * Get the word containing the target address and the byte offset in that word.
	 */
	base = slave_address & ~3;
	offset = (slave_address & 3) * 8;

	data = read_slavemem32(adev, base);
	data &= ~(0xff << offset);
	data |= val << offset;
	write_slavemem32(adev, base, data);
}

INLINE_IO u8 read_slavemem8(acx_device_t *adev, u32 slave_address) {
	u8 val;
	u32 base;
	u32 data;
	int offset;

	ACXMEM_WARN_NOT_SPIN_LOCKED;

	base = slave_address & ~3;
	offset = (slave_address & 3) * 8;

	data = read_slavemem32(adev, base);

	val = (data >> offset) & 0xff;

	return val;
}

/*
 * doesn't split across word boundaries
 */
INLINE_IO void write_slavemem16(acx_device_t *adev, u32 slave_address, u16 val) {
	u32 data;
	u32 base;
	int offset;

	ACXMEM_WARN_NOT_SPIN_LOCKED;

	/*
	 * Get the word containing the target address and the byte offset in that word.
	 */
	base = slave_address & ~3;
	offset = (slave_address & 3) * 8;

	data = read_slavemem32(adev, base);
	data &= ~(0xffff << offset);
	data |= val << offset;
	write_slavemem32(adev, base, data);
}

/*
 * doesn't split across word boundaries
 */
INLINE_IO u16 read_slavemem16(acx_device_t *adev, u32 slave_address) {
	u16 val;
	u32 base;
	u32 data;
	int offset;

	ACXMEM_WARN_NOT_SPIN_LOCKED;

	base = slave_address & ~3;
	offset = (slave_address & 3) * 8;

	data = read_slavemem32(adev, base);

	val = (data >> offset) & 0xffff;

	return val;
}

/*
 * Copy from slave memory
 *
 * TODO - rewrite using address autoincrement, handle partial words
 */
static void acxmem_copy_from_slavemem(acx_device_t *adev, u8 *destination, u32 source,
		int count) {
	u32 tmp = 0;
	u8 *ptmp = (u8 *) &tmp;

	ACXMEM_WARN_NOT_SPIN_LOCKED;

	/*
	 * Right now I'm making the assumption that the destination is aligned, but
	 * I'd better check.
	 */
	if ((u32) destination & 3) {
		printk(
				"acx copy_from_slavemem: warning!  destination not word-aligned!\n");
	}

	while (count >= 4) {
		write_reg32(adev, IO_ACX_SLV_MEM_ADDR, source);
		udelay (10);
		*((u32 *) destination) = read_reg32(adev, IO_ACX_SLV_MEM_DATA);
		count -= 4;
		source += 4;
		destination += 4;
	}

	/*
	 * If the word reads above didn't satisfy the count, read one more word
	 * and transfer a byte at a time until the request is satisfied.
	 */
	if (count) {
		write_reg32(adev, IO_ACX_SLV_MEM_ADDR, source);
		udelay (10);
		tmp = read_reg32(adev, IO_ACX_SLV_MEM_DATA);
		while (count--) {
			*destination++ = *ptmp++;
		}
	}

}

/*
 * Copy to slave memory
 *
 * TODO - rewrite using autoincrement, handle partial words
 */
static void acxmem_copy_to_slavemem(acx_device_t *adev, u32 destination, u8 *source,
		int count) {
	u32 tmp = 0;
	u8* ptmp = (u8 *) &tmp;
	static u8 src[512]; /* make static to avoid huge stack objects */

	ACXMEM_WARN_NOT_SPIN_LOCKED;

	/*
	 * For now, make sure the source is word-aligned by copying it to a word-aligned
	 * buffer.  Someday rewrite to avoid the extra copy.
	 */
	if (count > sizeof(src)) {
		printk("acx copy_to_slavemem: Warning! buffer overflow!\n");
		count = sizeof(src);
	}
	memcpy(src, source, count);
	source = src;

	while (count >= 4) {
		write_reg32(adev, IO_ACX_SLV_MEM_ADDR, destination);
		udelay (10);
		write_reg32(adev, IO_ACX_SLV_MEM_DATA, *((u32 *) source));
		count -= 4;
		source += 4;
		destination += 4;
	}

	/*
	 * If there are leftovers read the next word from the acx and merge in
	 * what they want to write.
	 */
	if (count) {
		write_reg32(adev, IO_ACX_SLV_MEM_ADDR, destination);
		udelay (10);
		tmp = read_reg32(adev, IO_ACX_SLV_MEM_DATA);
		while (count--) {
			*ptmp++ = *source++;
		}
		/*
		 * reset address in case we're currently in auto-increment mode
		 */
		write_reg32(adev, IO_ACX_SLV_MEM_ADDR, destination);
		udelay (10);
		write_reg32(adev, IO_ACX_SLV_MEM_DATA, tmp);
		udelay (10);
	}

}

/*
 * Block copy to slave buffers using memory block chain mode.  Copies to the ACX
 * transmit buffer structure with minimal intervention on our part.
 * Interrupts should be disabled when calling this.
 */
static void acxmem_chaincopy_to_slavemem(acx_device_t *adev, u32 destination, u8 *source,
		int count) {
	u32 val;
	u32 *data = (u32 *) source;
	static u8 aligned_source[WLAN_A4FR_MAXLEN_WEP_FCS];

	ACXMEM_WARN_NOT_SPIN_LOCKED;

	/*
	 * Warn if the pointers don't look right.  Destination must fit in [23:5] with
	 * zero elsewhere and source should be 32 bit aligned.
	 * This should never happen since we're in control of both, but I want to know about
	 * it if it does.
	 */
	if ((destination & 0x00ffffe0) != destination) {
		printk("acx chaincopy: destination block 0x%04x not aligned!\n",
				destination);
	}
	if (count > sizeof aligned_source) {
		printk(KERN_ERR "chaincopy_to_slavemem overflow!\n");
		count = sizeof aligned_source;
	}
	if ((u32) source & 3) {
		memcpy(aligned_source, source, count);
		data = (u32 *) aligned_source;
	}

	/*
	 * SLV_MEM_CTL[17:16] = memory block chain mode with auto-increment
	 * SLV_MEM_CTL[5:2] = offset to data portion = 1 word
	 */
	val = 2 << 16 | 1 << 2;
	acx_writel (val, &adev->iobase[ACX_SLV_MEM_CTL]);

	/*
	 * SLV_MEM_CP[23:5] = start of 1st block
	 * SLV_MEM_CP[3:2] = offset to memblkptr = 0
	 */
	val = destination & 0x00ffffe0;
	acx_writel (val, &adev->iobase[ACX_SLV_MEM_CP]);

	/*
	 * SLV_MEM_ADDR[23:2] = SLV_MEM_CTL[5:2] + SLV_MEM_CP[23:5]
	 */
	val = (destination & 0x00ffffe0) + (1 << 2);
	acx_writel (val, &adev->iobase[ACX_SLV_MEM_ADDR]);

	/*
	 * Write the data to the slave data register, rounding up to the end
	 * of the word containing the last byte (hence the > 0)
	 */
	while (count > 0) {
		acx_writel (*data++, &adev->iobase[ACX_SLV_MEM_DATA]);
		count -= 4;
	}

}

/*
 * Block copy from slave buffers using memory block chain mode.  Copies from the ACX
 * receive buffer structures with minimal intervention on our part.
 * Interrupts should be disabled when calling this.
 */
static void acxmem_chaincopy_from_slavemem(acx_device_t *adev, u8 *destination, u32 source,
		int count) {
	u32 val;
	u32 *data = (u32 *) destination;
	static u8 aligned_destination[WLAN_A4FR_MAXLEN_WEP_FCS];
	int saved_count = count;

	ACXMEM_WARN_NOT_SPIN_LOCKED;

	/*
	 * Warn if the pointers don't look right.  Destination must fit in [23:5] with
	 * zero elsewhere and source should be 32 bit aligned.
	 * Turns out the network stack sends unaligned things, so fix them before
	 * copying to the ACX.
	 */
	if ((source & 0x00ffffe0) != source) {
		printk("acx chaincopy: source block 0x%04x not aligned!\n", source);
		acxmem_dump_mem(adev, 0, 0x10000);
	}
	if ((u32) destination & 3) {
		//printk ("acx chaincopy: data destination not word aligned!\n");
		data = (u32 *) aligned_destination;
		if (count > sizeof aligned_destination) {
			printk(KERN_ERR "chaincopy_from_slavemem overflow!\n");
			count = sizeof aligned_destination;
		}
	}

	/*
	 * SLV_MEM_CTL[17:16] = memory block chain mode with auto-increment
	 * SLV_MEM_CTL[5:2] = offset to data portion = 1 word
	 */
	val = (2 << 16) | (1 << 2);
	acx_writel (val, &adev->iobase[ACX_SLV_MEM_CTL]);

	/*
	 * SLV_MEM_CP[23:5] = start of 1st block
	 * SLV_MEM_CP[3:2] = offset to memblkptr = 0
	 */
	val = source & 0x00ffffe0;
	acx_writel (val, &adev->iobase[ACX_SLV_MEM_CP]);

	/*
	 * SLV_MEM_ADDR[23:2] = SLV_MEM_CTL[5:2] + SLV_MEM_CP[23:5]
	 */
	val = (source & 0x00ffffe0) + (1 << 2);
	acx_writel (val, &adev->iobase[ACX_SLV_MEM_ADDR]);

	/*
	 * Read the data from the slave data register, rounding up to the end
	 * of the word containing the last byte (hence the > 0)
	 */
	while (count > 0) {
		*data++ = acx_readl (&adev->iobase[ACX_SLV_MEM_DATA]);
		count -= 4;
	}

	/*
	 * If the destination wasn't aligned, we would have saved it in
	 * the aligned buffer, so copy it where it should go.
	 */
	if ((u32) destination & 3) {
		memcpy(destination, aligned_destination, saved_count);
	}

}

int acxmem_create_hostdesc_queues(acx_device_t *adev) {
	int result;
	result = acxmem_create_tx_host_desc_queue(adev);
	if (OK != result)
		return result;
	result = acxmem_create_rx_host_desc_queue(adev);
	return result;
}

/*
 * acxmem_s_create_rx_host_desc_queue
 *
 * the whole size of a data buffer (header plus data body)
 * plus 32 bytes safety offset at the end
 */
static int acxmem_create_rx_host_desc_queue(acx_device_t *adev) {
	rxhostdesc_t *hostdesc;
	rxbuffer_t *rxbuf;
	int i;

	FN_ENTER;

	/* allocate the RX host descriptor queue pool */
	adev->rxhostdesc_area_size = RX_CNT * sizeof(*hostdesc);

	adev->rxhostdesc_start = acxmem_allocate(adev, adev->rxhostdesc_area_size,
			&adev->rxhostdesc_startphy, "rxhostdesc_start");
	if (!adev->rxhostdesc_start)
		goto fail;

	/* check for proper alignment of RX host descriptor pool */
	if ((long) adev->rxhostdesc_start & 3) {
		printk("acx: driver bug: dma alloc returns unaligned address\n");
		goto fail;
	}

	/* allocate Rx buffer pool which will be used by the acx
	 * to store the whole content of the received frames in it */
	adev->rxbuf_area_size = RX_CNT * RX_BUFFER_SIZE;

	adev->rxbuf_start = acxmem_allocate(adev, adev->rxbuf_area_size,
			&adev->rxbuf_startphy, "rxbuf_start");
	if (!adev->rxbuf_start)
		goto fail;

	rxbuf = adev->rxbuf_start;
	hostdesc = adev->rxhostdesc_start;

	/* don't make any popular C programming pointer arithmetic mistakes
	 * here, otherwise I'll kill you...
	 * (and don't dare asking me why I'm warning you about that...) */
	for (i = 0; i < RX_CNT; i++) {
		hostdesc->data = rxbuf;
		hostdesc->length = cpu_to_le16(RX_BUFFER_SIZE);
		rxbuf++;
		hostdesc++;
	}
	hostdesc--;
	FN_EXIT1(OK);
	return OK;
	fail: printk("acx: create_rx_host_desc_queue FAILED\n");
	/* dealloc will be done by free function on error case */
	FN_EXIT1(NOT_OK);
	return NOT_OK;
}

/*
 * In the generic slave memory access mode, most of the stuff in
 * the txhostdesc_t is unused.  It's only here because the rest of
 * the ACX driver expects it to be since the PCI version uses indirect
 * host memory organization with DMA.  Since we're not using DMA the
 * only use we have for the host descriptors is to store the packets
 * on the way out.
 */
static int acxmem_create_tx_host_desc_queue(acx_device_t *adev) {
	txhostdesc_t *hostdesc;
	u8 *txbuf;
	int i;

	FN_ENTER;

	/* allocate TX buffer */
	/* WLAN_A4FR_MAXLEN_WEP_FCS */
	adev->txbuf_area_size = TX_CNT * WLAN_A4FR_MAXLEN_WEP_FCS;

	adev->txbuf_start = acxmem_allocate(adev, adev->txbuf_area_size,
			&adev->txbuf_startphy, "txbuf_start");
	if (!adev->txbuf_start)
		goto fail;

	/* allocate the TX host descriptor queue pool */
	adev->txhostdesc_area_size = TX_CNT * 2 * sizeof(*hostdesc);

	adev->txhostdesc_start = acxmem_allocate(adev, adev->txhostdesc_area_size,
			&adev->txhostdesc_startphy, "txhostdesc_start");
	if (!adev->txhostdesc_start)
		goto fail;

	/* check for proper alignment of TX host descriptor pool */
	if ((long) adev->txhostdesc_start & 3) {
		printk("acx: driver bug: dma alloc returns unaligned address\n");
		goto fail;
	}

	hostdesc = adev->txhostdesc_start;
	txbuf = adev->txbuf_start;

#if 0
	/* Each tx buffer is accessed by hardware via
	 ** txdesc -> txhostdesc(s) -> txbuffer(s).
	 ** We use only one txhostdesc per txdesc, but it looks like
	 ** acx111 is buggy: it accesses second txhostdesc
	 ** (via hostdesc.desc_phy_next field) even if
	 ** txdesc->length == hostdesc->length and thus
	 ** entire packet was placed into first txhostdesc.
	 ** Due to this bug acx111 hangs unless second txhostdesc
	 ** has le16_to_cpu(hostdesc.length) = 3 (or larger)
	 ** Storing NULL into hostdesc.desc_phy_next
	 ** doesn't seem to help.
	 **
	 ** Update: although it worked on Xterasys XN-2522g
	 ** with len=3 trick, WG311v2 is even more bogus, doesn't work.
	 ** Keeping this code (#ifdef'ed out) for documentational purposes.
	 */
	for (i = 0; i < TX_CNT*2; i++) {
		hostdesc_phy += sizeof(*hostdesc);
		if (!(i & 1)) {
			hostdesc->data_phy = cpu2acx(txbuf_phy);
			/* hostdesc->data_offset = ... */
			/* hostdesc->reserved = ... */
			hostdesc->Ctl_16 = cpu_to_le16(DESC_CTL_HOSTOWN);
			/* hostdesc->length = ... */
			hostdesc->desc_phy_next = cpu2acx(hostdesc_phy);
			hostdesc->pNext = ptr2acx(NULL);
			/* hostdesc->Status = ... */
			/* below: non-hardware fields */
			hostdesc->data = txbuf;

			txbuf += WLAN_A4FR_MAXLEN_WEP_FCS;
			txbuf_phy += WLAN_A4FR_MAXLEN_WEP_FCS;
		} else {
			/* hostdesc->data_phy = ... */
			/* hostdesc->data_offset = ... */
			/* hostdesc->reserved = ... */
			/* hostdesc->Ctl_16 = ... */
			hostdesc->length = cpu_to_le16(3); /* bug workaround */
			/* hostdesc->desc_phy_next = ... */
			/* hostdesc->pNext = ... */
			/* hostdesc->Status = ... */
			/* below: non-hardware fields */
			/* hostdesc->data = ... */
		}
		hostdesc++;
	}
#endif

	/* We initialize two hostdescs so that they point to adjacent
	 ** memory areas. Thus txbuf is really just a contiguous memory area */
	for (i = 0; i < TX_CNT * 2; i++) {
		/* ->data is a non-hardware field: */
		hostdesc->data = txbuf;

		if (!(i & 1)) {
			txbuf += WLAN_HDR_A3_LEN;
		} else {
			txbuf += WLAN_A4FR_MAXLEN_WEP_FCS - WLAN_HDR_A3_LEN;
		}
		hostdesc++;
	}
	hostdesc--;

	FN_EXIT1(OK);
	return OK;

	fail:
		// OW FIXME Logging
		printk("acx: create_tx_host_desc_queue FAILED\n");
		/* dealloc will be done by free function on error case */

	FN_EXIT1(NOT_OK);
	return NOT_OK;
}

void acxmem_create_desc_queues(acx_device_t *adev, u32 tx_queue_start,
		u32 rx_queue_start) {
	u32 *p;
	int i;

	acxmem_lock_flags;
	acxmem_lock();

	acxmem_create_tx_desc_queue(adev, tx_queue_start);
	acxmem_create_rx_desc_queue(adev, rx_queue_start);
	p = (u32 *) adev->acx_queue_indicator;
	for (i = 0; i < 4; i++) {
		write_slavemem32(adev, (u32) p, 0);
		p++;
	}
	acxmem_unlock();

}

static void acxmem_create_rx_desc_queue(acx_device_t *adev, u32 rx_queue_start) {
	rxdesc_t *rxdesc;
	u32 mem_offs;
	int i;

	FN_ENTER;

	/* done by memset: adev->rx_tail = 0; */

	/* ACX111 doesn't need any further config: preconfigures itself.
	 * Simply print ring buffer for debugging */
	if (IS_ACX111(adev)) {
		/* rxdesc_start already set here */

		adev->rxdesc_start = (rxdesc_t *) rx_queue_start;

		rxdesc = adev->rxdesc_start;
		for (i = 0; i < RX_CNT; i++) {
			log(L_DEBUG, "acx: rx descriptor %d @ 0x%p\n", i, rxdesc);
			rxdesc = adev->rxdesc_start = (rxdesc_t *) acx2cpu(rxdesc->pNextDesc);
		}
	} else {
		/* we didn't pre-calculate rxdesc_start in case of ACX100 */
		/* rxdesc_start should be right AFTER Tx pool */
		adev->rxdesc_start = (rxdesc_t *) ((u8 *) adev->txdesc_start + (TX_CNT
		* sizeof(txdesc_t)));
		/* NB: sizeof(txdesc_t) above is valid because we know
		 ** we are in if (acx100) block. Beware of cut-n-pasting elsewhere!
		 ** acx111's txdesc is larger! */

		mem_offs = (u32) adev->rxdesc_start;
		while (mem_offs < (u32) adev->rxdesc_start + (RX_CNT * sizeof(*rxdesc))) {
			write_slavemem32(adev, mem_offs, 0);
			mem_offs += 4;
		}

		/* loop over whole receive pool */
		rxdesc = adev->rxdesc_start;
		for (i = 0; i < RX_CNT; i++) {
			log(L_DEBUG, "rx descriptor @ 0x%p\n", rxdesc);
			/* point to next rxdesc */
			write_slavemem32(adev, (u32) &(rxdesc->pNextDesc),
					(u32) cpu_to_le32 ((u8 *) rxdesc + sizeof(*rxdesc)));
			/* go to the next one */
			rxdesc++;
		}
		/* go to the last one */
		rxdesc--;

		/* and point to the first making it a ring buffer */
		write_slavemem32(adev, (u32) &(rxdesc->pNextDesc), (u32) cpu_to_le32 (rx_queue_start));
	}
	FN_EXIT0;
}

static void acxmem_create_tx_desc_queue(acx_device_t *adev, u32 tx_queue_start) {
	txdesc_t *txdesc;
	u32 clr;
	int i;

	FN_ENTER;

	if (IS_ACX100(adev))
		adev->txdesc_size = sizeof(*txdesc);
	else
		/* the acx111 txdesc is 4 bytes larger */
		adev->txdesc_size = sizeof(*txdesc) + 4;

	/*
	 * This refers to an ACX address, not one of ours
	 */
	adev->txdesc_start = (txdesc_t *) tx_queue_start;

	log(L_DEBUG, "acx: adev->txdesc_start=%p\n",
			adev->txdesc_start);

	adev->tx_free = TX_CNT;
	/* done by memset: adev->tx_head = 0; */
	/* done by memset: adev->tx_tail = 0; */
	txdesc = adev->txdesc_start;

	if (IS_ACX111(adev)) {
		/* ACX111 has a preinitialized Tx buffer! */
		/* loop over whole send pool */
		/* FIXME: do we have to do the hostmemptr stuff here?? */
		for (i = 0; i < TX_CNT; i++) {
			txdesc->Ctl_8 = DESC_CTL_HOSTOWN;
			/* reserve two (hdr desc and payload desc) */
			txdesc = acxmem_advance_txdesc(adev, txdesc, 1);
		}
	} else {
		/* ACX100 Tx buffer needs to be initialized by us */
		/* clear whole send pool. sizeof is safe here (we are acx100) */

		/*
		 * adev->txdesc_start refers to device memory, so we can't write
		 * directly to it.
		 */
		clr = (u32) adev->txdesc_start;
		while (clr < (u32) adev->txdesc_start + (TX_CNT * sizeof(*txdesc))) {
			write_slavemem32(adev, clr, 0);
			clr += 4;
		}

		/* loop over whole send pool */
		for (i = 0; i < TX_CNT; i++) {
			log(L_DEBUG, "configure card tx descriptor: 0x%p, "
					"size: 0x%X\n", txdesc, adev->txdesc_size);

			/* initialise ctl */
			/*
			 * No auto DMA here
			 */
			write_slavemem8(adev, (u32) &(txdesc->Ctl_8),
					(u8) (DESC_CTL_HOSTOWN| DESC_CTL_FIRSTFRAG));
			/* done by memset(0): txdesc->Ctl2_8 = 0; */

			/* point to next txdesc */
			write_slavemem32(adev, (u32) &(txdesc->pNextDesc),
					(u32) cpu_to_le32 ((u8 *) txdesc + adev->txdesc_size));

			/* go to the next one */
			/* ++ is safe here (we are acx100) */
			txdesc++;
		}
		/* go back to the last one */
		txdesc--;
		/* and point to the first making it a ring buffer */
		write_slavemem32(adev, (u32) &(txdesc->pNextDesc), (u32) cpu_to_le32 (tx_queue_start));
	}
	FN_EXIT0;
}

/*
 * acxmem_free_desc_queues
 *
 * Releases the queues that have been allocated, the
 * others have been initialised to NULL so this
 * function can be used if only part of the queues were allocated.
 */
void acxmem_free_desc_queues(acx_device_t *adev) {

#define ACX_FREE_QUEUE(size, ptr, phyaddr) \
        if (ptr) { \
                kfree(ptr); \
                ptr = NULL; \
                size = 0; \
        }

	FN_ENTER;

	ACX_FREE_QUEUE(adev->txhostdesc_area_size, adev->txhostdesc_start, adev->txhostdesc_startphy);
	ACX_FREE_QUEUE(adev->txbuf_area_size, adev->txbuf_start, adev->txbuf_startphy);
	adev->txdesc_start = NULL;

	ACX_FREE_QUEUE(adev->rxhostdesc_area_size, adev->rxhostdesc_start, adev->rxhostdesc_startphy);
	ACX_FREE_QUEUE(adev->rxbuf_area_size, adev->rxbuf_start, adev->rxbuf_startphy);
	adev->rxdesc_start = NULL;

	FN_EXIT0;
}

static void acxmem_delete_dma_regions(acx_device_t *adev) {

	//unsigned long flags;

	FN_ENTER;
	/* disable radio Tx/Rx. Shouldn't we use the firmware commands
	 * here instead? Or are we that much down the road that it's no
	 * longer possible here? */
	/*
	 * slave memory interface really doesn't like this.
	 */
	/*
	 write_reg16(adev, IO_ACX_ENABLE, 0);
	 */

	acx_s_mwait(100);

	acxmem_free_desc_queues(adev);

	FN_EXIT0;
}

static void*
acxmem_allocate(acx_device_t *adev, size_t size, dma_addr_t *phy, const char *msg) {
	void *ptr;
	ptr = kmalloc(size, GFP_KERNEL);
	/*
	 * The ACX can't use the physical address, so we'll have to fake it
	 * later and it might be handy to have the virtual address.
	 */
	*phy = (dma_addr_t) NULL;

	if (ptr) {
		log(L_DEBUG, "acx: %s sz=%d adr=0x%p phy=0x%08llx\n",
				msg, (int)size, ptr, (unsigned long long)*phy);
		memset(ptr, 0, size);
		return ptr;
	}
	printk(KERN_ERR "acx: %s allocation FAILED (%d bytes)\n", msg, (int) size);
	return NULL;
}


/*
 * BOM Firmware, EEPROM, Phy
 * ==================================================
 */

/*
 * acxmem_s_upload_radio
 *
 * Uploads the appropriate radio module firmware into the card.
 */
int acxmem_upload_radio(acx_device_t *adev) {
	acx_ie_memmap_t mm;
	firmware_image_t *radio_image;
	acx_cmd_radioinit_t radioinit;
	int res = NOT_OK;
	int try;
	u32 offset;
	u32 size;
	char filename[sizeof("RADIONN.BIN")];

	acxmem_lock_flags;

	if (!adev->need_radio_fw)
		return OK;

	FN_ENTER;

	acx_s_interrogate(adev, &mm, ACX1xx_IE_MEMORY_MAP);
	offset = le32_to_cpu(mm.CodeEnd);

	snprintf(filename, sizeof(filename), "RADIO%02x.BIN", adev->radio_type);
	radio_image = acx_s_read_fw(adev->bus_dev, filename, &size);
	if (!radio_image) {
		printk("acx: can't load radio module '%s'\n", filename);
		goto fail;
	}

	acx_s_issue_cmd(adev, ACX1xx_CMD_SLEEP, NULL, 0);

	for (try = 1; try <= 5; try++) {
		acxmem_lock();
		res = acxmem_write_fw(adev, radio_image, offset);
		log(L_DEBUG|L_INIT, "acx: acx_write_fw (radio): %d\n", res);
		if (OK == res) {
			res = acxmem_validate_fw(adev, radio_image, offset);
			log(L_DEBUG|L_INIT, "acx: acx_validate_fw (radio): %d\n", res);
		}
		acxmem_unlock();

		if (OK == res)
			break;
		printk("acx: radio firmware upload attempt #%d FAILED, "
			"retrying...\n", try);
		acx_s_mwait(1000); /* better wait for a while... */
	}

	acx_s_issue_cmd(adev, ACX1xx_CMD_WAKE, NULL, 0);
	radioinit.offset = cpu_to_le32(offset);

	/* no endian conversion needed, remains in card CPU area: */
	radioinit.len = radio_image->size;

	vfree(radio_image);

	if (OK != res)
		goto fail;

	/* will take a moment so let's have a big timeout */
	acx_s_issue_cmd_timeo(adev, ACX1xx_CMD_RADIOINIT,
			&radioinit, sizeof(radioinit), CMD_TIMEOUT_MS(1000));

	res = acx_s_interrogate(adev, &mm, ACX1xx_IE_MEMORY_MAP);

	fail:
	FN_EXIT1(res);
	return res;
}

/*
 * acxmem_read_eeprom_byte
 *
 * Function called to read an octet in the EEPROM.
 *
 * This function is used by acxmem_e_probe to check if the
 * connected card is a legal one or not.
 *
 * Arguments:
 *	adev		ptr to acx_device structure
 *	addr		address to read in the EEPROM
 *	charbuf		ptr to a char. This is where the read octet
 *			will be stored
 */
int acxmem_read_eeprom_byte(acx_device_t *adev, u32 addr, u8 *charbuf) {
	int result;
	int count;

	FN_ENTER;

	write_reg32(adev, IO_ACX_EEPROM_CFG, 0);
	write_reg32(adev, IO_ACX_EEPROM_ADDR, addr);
	write_flush(adev);
	write_reg32(adev, IO_ACX_EEPROM_CTL, 2);

	count = 0xffff;
	while (read_reg16(adev, IO_ACX_EEPROM_CTL)) {
		/* scheduling away instead of CPU burning loop
		 * doesn't seem to work here at all:
		 * awful delay, sometimes also failure.
		 * Doesn't matter anyway (only small delay). */
		if (unlikely(!--count)) {
			printk("acx: %s: timeout waiting for EEPROM read\n", wiphy_name(
					adev->ieee->wiphy));
			result = NOT_OK;
			goto fail;
		}
		cpu_relax();
	}

	*charbuf = read_reg8(adev, IO_ACX_EEPROM_DATA);
	log(L_DEBUG, "acx: EEPROM at 0x%04X = 0x%02X\n", addr, *charbuf);
	result = OK;

	fail:
	FN_EXIT1(result);
	return result;

}

/*
 * We don't lock hw accesses here since we never r/w eeprom in IRQ
 * Note: this function sleeps only because of GFP_KERNEL alloc
 */
#ifdef UNUSED
int
acxmem_s_write_eeprom(acx_device_t *adev, u32 addr, u32 len,
		const u8 *charbuf)
{
	u8 *data_verify = NULL;
	unsigned long flags;
	int count, i;
	int result = NOT_OK;
	u16 gpio_orig;

	printk("acx: WARNING! I would write to EEPROM now. "
			"Since I really DON'T want to unless you know "
			"what you're doing (THIS CODE WILL PROBABLY "
			"NOT WORK YET!), I will abort that now. And "
			"definitely make sure to make a "
			"/proc/driver/acx_wlan0_eeprom backup copy first!!! "
			"(the EEPROM content includes the PCI config header!! "
			"If you kill important stuff, then you WILL "
			"get in trouble and people DID get in trouble already)\n");
	return OK;

	FN_ENTER;

	data_verify = kmalloc(len, GFP_KERNEL);
	if (!data_verify) {
		goto end;
	}

	/* first we need to enable the OE (EEPROM Output Enable) GPIO line
	 * to be able to write to the EEPROM.
	 * NOTE: an EEPROM writing success has been reported,
	 * but you probably have to modify GPIO_OUT, too,
	 * and you probably need to activate a different GPIO
	 * line instead! */
	gpio_orig = read_reg16(adev, IO_ACX_GPIO_OE);
	write_reg16(adev, IO_ACX_GPIO_OE, gpio_orig & ~1);
	write_flush(adev);

	/* ok, now start writing the data out */
	for (i = 0; i < len; i++) {
		write_reg32(adev, IO_ACX_EEPROM_CFG, 0);
		write_reg32(adev, IO_ACX_EEPROM_ADDR, addr + i);
		write_reg32(adev, IO_ACX_EEPROM_DATA, *(charbuf + i));
		write_flush(adev);
		write_reg32(adev, IO_ACX_EEPROM_CTL, 1);

		count = 0xffff;
		while (read_reg16(adev, IO_ACX_EEPROM_CTL)) {
			if (unlikely(!--count)) {
				printk("acx: WARNING, DANGER!!! "
						"Timeout waiting for EEPROM write\n");
				goto end;
			}
			cpu_relax();
		}
	}

	/* disable EEPROM writing */
	write_reg16(adev, IO_ACX_GPIO_OE, gpio_orig);
	write_flush(adev);

	/* now start a verification run */
	for (i = 0; i < len; i++) {
		write_reg32(adev, IO_ACX_EEPROM_CFG, 0);
		write_reg32(adev, IO_ACX_EEPROM_ADDR, addr + i);
		write_flush(adev);
		write_reg32(adev, IO_ACX_EEPROM_CTL, 2);

		count = 0xffff;
		while (read_reg16(adev, IO_ACX_EEPROM_CTL)) {
			if (unlikely(!--count)) {
				printk("acx: timeout waiting for EEPROM read\n");
				goto end;
			}
			cpu_relax();
		}

		data_verify[i] = read_reg16(adev, IO_ACX_EEPROM_DATA);
	}

	if (0 == memcmp(charbuf, data_verify, len))
	result = OK; /* read data matches, success */

	end:
	kfree(data_verify);
	FN_EXIT1(result);
	return result;
}
#endif /* UNUSED */

static inline void acxmem_read_eeprom_area(acx_device_t *adev) {
#if ACX_DEBUG > 1
	int offs;
	u8 tmp;

	FN_ENTER;

	for (offs = 0x8c; offs < 0xb9; offs++)
		acxmem_read_eeprom_byte(adev, offs, &tmp);

	FN_EXIT0;

#endif
}

/*
 * acxmem_s_read_phy_reg
 *
 * Messing with rx/tx disabling and enabling here
 * (write_reg32(adev, IO_ACX_ENABLE, 0b000000xx)) kills traffic
 */
int acxmem_read_phy_reg(acx_device_t *adev, u32 reg, u8 *charbuf) {
	int result = NOT_OK;
	int count;
	acxmem_lock_flags;

	FN_ENTER;
	acxmem_lock();

	write_reg32(adev, IO_ACX_PHY_ADDR, reg);
	write_flush(adev);
	write_reg32(adev, IO_ACX_PHY_CTL, 2);

	count = 0xffff;
	while (read_reg32(adev, IO_ACX_PHY_CTL)) {
		/* scheduling away instead of CPU burning loop
		 * doesn't seem to work here at all:
		 * awful delay, sometimes also failure.
		 * Doesn't matter anyway (only small delay). */
		if (unlikely(!--count)) {
			printk("acx: %s: timeout waiting for phy read\n", wiphy_name(
					adev->ieee->wiphy));
			*charbuf = 0;
			goto fail;
		}
		cpu_relax();
	}

	log(L_DEBUG, "acx: the count was %u\n", count);
	*charbuf = read_reg8(adev, IO_ACX_PHY_DATA);

	log(L_DEBUG, "acx: radio PHY at 0x%04X = 0x%02X\n", *charbuf, reg);
	result = OK;
	goto fail; /* silence compiler warning */
	fail:

	acxmem_unlock();
	FN_EXIT1(result);
	return result;
}

int acxmem_write_phy_reg(acx_device_t *adev, u32 reg, u8 value) {
	int count;
	acxmem_lock_flags;

	FN_ENTER;
	acxmem_lock();

	/* mprusko said that 32bit accesses result in distorted sensitivity
	 * on his card. Unconfirmed, looks like it's not true (most likely since we
	 * now properly flush writes). */
	write_reg32(adev, IO_ACX_PHY_DATA, value);
	write_reg32(adev, IO_ACX_PHY_ADDR, reg);
	write_flush(adev);
	write_reg32(adev, IO_ACX_PHY_CTL, 1);
	write_flush(adev);

	count = 0xffff;
	while (read_reg32(adev, IO_ACX_PHY_CTL)) {
		/* scheduling away instead of CPU burning loop
		 * doesn't seem to work here at all:
		 * awful delay, sometimes also failure.
		 * Doesn't matter anyway (only small delay). */
		if (unlikely(!--count)) {
			printk("acx: %s: timeout waiting for phy read\n", wiphy_name(
					adev->ieee->wiphy));
			goto fail;
		}
		cpu_relax();
	}

	log(L_DEBUG, "acx: radio PHY write 0x%02X at 0x%04X\n", value, reg);
	fail:

	acxmem_unlock();
	FN_EXIT1(OK);
	return OK;
}

/*
 * acxmem_s_write_fw
 *
 * Write the firmware image into the card.
 *
 * Arguments:
 *	adev		wlan device structure
 *	fw_image	firmware image.
 *
 * Returns:
 *	1	firmware image corrupted
 *	0	success
 */
static int acxmem_write_fw(acx_device_t *adev,
		const firmware_image_t *fw_image, u32 offset) {
	int len, size, checkMismatch = -1;
	u32 sum, v32, tmp, id;

	/* we skip the first four bytes which contain the control sum */
	const u8 *p = (u8*) fw_image + 4;

	FN_ENTER;

	/* start the image checksum by adding the image size value */
	sum = p[0] + p[1] + p[2] + p[3];
	p += 4;

#ifdef NOPE
#if FW_NO_AUTO_INCREMENT
	write_reg32(adev, IO_ACX_SLV_MEM_CTL, 0); /* use basic mode */
#else
	write_reg32(adev, IO_ACX_SLV_MEM_CTL, 1); /* use autoincrement mode */
	write_reg32(adev, IO_ACX_SLV_MEM_ADDR, offset); /* configure start address */
	write_flush(adev);
#endif
#endif
	len = 0;
	size = le32_to_cpu(fw_image->size) & (~3);

	while (likely(len < size)) {
		v32 = be32_to_cpu(*(u32*)p);
		sum += p[0] + p[1] + p[2] + p[3];
		p += 4;
		len += 4;

#ifdef NOPE
#if FW_NO_AUTO_INCREMENT
		write_reg32(adev, IO_ACX_SLV_MEM_ADDR, offset + len - 4);
		write_flush(adev);
#endif
		write_reg32(adev, IO_ACX_SLV_MEM_DATA, v32);
		write_flush(adev);
#endif
		write_slavemem32(adev, offset + len - 4, v32);

		id = read_id_register(adev);

		/*
		 * check the data written
		 */
		tmp = read_slavemem32(adev, offset + len - 4);
		if (checkMismatch && (tmp != v32)) {
			printk(
					"first data mismatch at 0x%08x good 0x%08x bad 0x%08x id 0x%08x\n",
					offset + len - 4, v32, tmp, id);
			checkMismatch = 0;
		}
	}
	log(L_DEBUG, "acx: firmware written, size:%d sum1:%x sum2:%x\n",
			size, sum, le32_to_cpu(fw_image->chksum));

	/* compare our checksum with the stored image checksum */
	FN_EXIT1(sum != le32_to_cpu(fw_image->chksum));
	return (sum != le32_to_cpu(fw_image->chksum));
}

/*
 * acxmem_s_validate_fw
 *
 * Compare the firmware image given with
 * the firmware image written into the card.
 *
 * Arguments:
 *	adev		wlan device structure
 *	fw_image	firmware image.
 *
 * Returns:
 *	NOT_OK	firmware image corrupted or not correctly written
 *	OK	success
 */
static int acxmem_validate_fw(acx_device_t *adev,
		const firmware_image_t *fw_image, u32 offset) {
	u32 sum, v32, w32;
	int len, size;
	int result = OK;
	/* we skip the first four bytes which contain the control sum */
	const u8 *p = (u8*) fw_image + 4;

	FN_ENTER;

	/* start the image checksum by adding the image size value */
	sum = p[0] + p[1] + p[2] + p[3];
	p += 4;

	write_reg32(adev, IO_ACX_SLV_END_CTL, 0);

#if FW_NO_AUTO_INCREMENT
	write_reg32(adev, IO_ACX_SLV_MEM_CTL, 0); /* use basic mode */
#else
	write_reg32(adev, IO_ACX_SLV_MEM_CTL, 1); /* use autoincrement mode */
	write_reg32(adev, IO_ACX_SLV_MEM_ADDR, offset); /* configure start address */
#endif

	len = 0;
	size = le32_to_cpu(fw_image->size) & (~3);

	while (likely(len < size)) {
		v32 = be32_to_cpu(*(u32*)p);
		p += 4;
		len += 4;

#ifdef NOPE
#if FW_NO_AUTO_INCREMENT
		write_reg32(adev, IO_ACX_SLV_MEM_ADDR, offset + len - 4);
#endif
		udelay(10);
		w32 = read_reg32(adev, IO_ACX_SLV_MEM_DATA);
#endif

		w32 = read_slavemem32(adev, offset + len - 4);

		if (unlikely(w32 != v32)) {
			printk("acx: FATAL: firmware upload: "
				"data parts at offset %d don't match\n(0x%08X vs. 0x%08X)!\n"
				"I/O timing issues or defective memory, with DWL-xx0+? "
				"ACX_IO_WIDTH=16 may help. Please report\n", len, v32, w32);
			result = NOT_OK;
			break;
		}

		sum += (u8) w32 + (u8) (w32 >> 8) + (u8) (w32 >> 16) + (u8) (w32 >> 24);
	}

	/* sum control verification */
	if (result != NOT_OK) {
		if (sum != le32_to_cpu(fw_image->chksum)) {
			printk("acx: FATAL: firmware upload: "
				"checksums don't match!\n");
			result = NOT_OK;
		}
	}

	FN_EXIT1(result);
	return result;
}

static int acxmem_upload_fw(acx_device_t *adev) {
	firmware_image_t *fw_image = NULL;
	int res = NOT_OK;
	int try;
	u32 file_size;
	char *filename = "WLANGEN.BIN";

	acxmem_lock_flags;

#ifdef PATCH_AROUND_BAD_SPOTS
	u32 offset;
	int i;
	/*
	 * arm-linux-objdump -d patch.bin, or
	 * od -Ax -t x4 patch.bin after finding the bounds
	 * of the .text section with arm-linux-objdump -s patch.bin
	 */
	u32 patch[] = { 0xe584c030, 0xe59fc008, 0xe92d1000, 0xe59fc004, 0xe8bd8000,
			0x0000080c, 0x0000aa68, 0x605a2200, 0x2c0a689c, 0x2414d80a,
			0x2f00689f, 0x1c27d007, 0x06241e7c, 0x2f000e24, 0xe000d1f6,
			0x602e6018, 0x23036468, 0x480203db, 0x60ca6003, 0xbdf0750a,
			0xffff0808 };
#endif

	FN_ENTER;

	/* No combined image; tell common we need the radio firmware, too */
	adev->need_radio_fw = 1;

	fw_image = acx_s_read_fw(adev->bus_dev, filename, &file_size);
	if (!fw_image) {
		FN_EXIT1(NOT_OK);
		return NOT_OK;
	}

	for (try = 1; try <= 5; try++) {

		acxmem_lock();
		res = acxmem_write_fw(adev, fw_image, 0);
		log(L_DEBUG|L_INIT, "acx: acx_write_fw (main): %d\n", res);
		if (OK == res) {
			res = acxmem_validate_fw(adev, fw_image, 0);
			log(L_DEBUG|L_INIT, "acx: acx_validate_fw "
					"(main): %d\n", res);
		}
		acxmem_unlock();

		if (OK == res) {
			SET_BIT(adev->dev_state_mask, ACX_STATE_FW_LOADED);
			break;
		}
		printk("acx: firmware upload attempt #%d FAILED, "
			"retrying...\n", try);
		acx_s_mwait(1000); /* better wait for a while... */
	}

#ifdef PATCH_AROUND_BAD_SPOTS
	acxmem_lock();
	/*
	 * Only want to do this if the firmware is exactly what we expect for an
	 * iPaq 4700; otherwise, bad things would ensue.
	 */
	if ((HX4700_FIRMWARE_CHECKSUM == fw_image->chksum)
			|| (HX4700_ALTERNATE_FIRMWARE_CHECKSUM == fw_image->chksum)) {
		/*
		 * Put the patch after the main firmware image.  0x950c contains
		 * the ACX's idea of the end of the firmware.  Use that location to
		 * load ours (which depends on that location being 0xab58) then
		 * update that location to point to after ours.
		 */

		offset = read_slavemem32(adev, 0x950c);

		log (L_DEBUG, "acx: patching in at 0x%04x\n", offset);

		for (i = 0; i < sizeof(patch) / sizeof(patch[0]); i++) {
			write_slavemem32(adev, offset, patch[i]);
			offset += sizeof(u32);
		}

		/*
		 * Patch the instruction at 0x0804 to branch to our ARM patch at 0xab58
		 */
		write_slavemem32(adev, 0x0804, 0xea000000 + (0xab58 - 0x0804 - 8) / 4);

		/*
		 * Patch the instructions at 0x1f40 to branch to our Thumb patch at 0xab74
		 *
		 * 4a00 ldr r2, [pc, #0]
		 * 4710 bx  r2
		 * .data 0xab74+1
		 */
		write_slavemem32(adev, 0x1f40, 0x47104a00);
		write_slavemem32(adev, 0x1f44, 0x0000ab74 + 1);

		/*
		 * Bump the end of the firmware up to beyond our patch.
		 */
		write_slavemem32(adev, 0x950c, offset);

	}
	acxmem_unlock();
#endif

	vfree(fw_image);

	FN_EXIT1(res);
	return res;
}

#if defined(NONESSENTIAL_FEATURES)
typedef struct device_id {
	unsigned char id[6];
	char *descr;
	char *type;
}device_id_t;

static const device_id_t
device_ids[] =
{
	{
		{	'G', 'l', 'o', 'b', 'a', 'l'},
		NULL,
		NULL,
	},
	{
		{	0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
		"uninitialized",
		"SpeedStream SS1021 or Gigafast WF721-AEX"
	},
	{
		{	0x80, 0x81, 0x82, 0x83, 0x84, 0x85},
		"non-standard",
		"DrayTek Vigor 520"
	},
	{
		{	'?', '?', '?', '?', '?', '?'},
		"non-standard",
		"Level One WPC-0200"
	},
	{
		{	0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
		"empty",
		"DWL-650+ variant"
	}
};

static void
acx_show_card_eeprom_id(acx_device_t *adev)
{
	unsigned char buffer[CARD_EEPROM_ID_SIZE];
	int i;

	FN_ENTER;

	memset(&buffer, 0, CARD_EEPROM_ID_SIZE);
	/* use direct EEPROM access */
	for (i = 0; i < CARD_EEPROM_ID_SIZE; i++) {
		if (OK != acxmem_read_eeprom_byte(adev,
						ACX100_EEPROM_ID_OFFSET + i,
						&buffer[i])) {
			printk("acx: reading EEPROM FAILED\n");
			break;
		}
	}

	for (i = 0; i < ARRAY_SIZE(device_ids); i++) {
		if (!memcmp(&buffer, device_ids[i].id, CARD_EEPROM_ID_SIZE)) {
			if (device_ids[i].descr) {
				printk("acx: EEPROM card ID string check "
						"found %s card ID: is this %s?\n",
						device_ids[i].descr, device_ids[i].type);
			}
			break;
		}
	}
	if (i == ARRAY_SIZE(device_ids)) {
		printk("acx: EEPROM card ID string check found "
				"unknown card: expected 'Global', got '%.*s\'. "
				"Please report\n", CARD_EEPROM_ID_SIZE, buffer);
	}
	FN_EXIT0;
}
#endif /* NONESSENTIAL_FEATURES */

/*
 * BOM CMDs (Control Path)
 * ==================================================
 */

/*
 * acxmem_s_issue_cmd_timeo
 *
 * Sends command to fw, extract result
 *
 * OW, 20100630:
 *
 * The mem device is quite sensible to data access operations, therefore
 * we may not sleep during the command handling.
 *
 * This has manifested as problem during sw-scan while if up. The acx got
 * stuck - most probably due to concurrent data access collision.
 *
 * By not sleeping anymore and doing the entire operation completely under
 * spinlock (thus with irqs disabled), the sw scan problem was solved.
 *
 * We can now run repeating sw scans, under load, without that the acx
 * device gets stuck.
 *
 * Also ifup/down works more reliable on the mem device.
 *
 */
int acxmem_issue_cmd_timeo_debug(acx_device_t *adev, unsigned cmd,
		void *buffer, unsigned buflen, unsigned cmd_timeout, const char* cmdstr) {

	unsigned long start = jiffies;
	const char *devname;
	unsigned counter;
	u16 irqtype;
	u16 cmd_status=-1;
	int i, j;
	u8 *p;

	acxmem_lock_flags;

	FN_ENTER;
	acxmem_lock();

	devname = wiphy_name(adev->ieee->wiphy);
	if (!devname || !devname[0] || devname[4] == '%')
		devname = "acx";

	log(L_CTL, "acxmem: %s: cmd:%s, cmd:0x%04X, buflen:%u, timeout:%ums, type:0x%04X)\n",
			__func__, cmdstr, cmd, buflen, cmd_timeout,
			buffer ? le16_to_cpu(((acx_ie_generic_t *)buffer)->type) : -1);

	if (!(adev->dev_state_mask & ACX_STATE_FW_LOADED)) {
		printk("acxmem: %s: %s: firmware is not loaded yet, cannot execute commands!\n",
				__func__, devname);
		goto bad;
	}

	if ((acx_debug & L_DEBUG) && (cmd != ACX1xx_CMD_INTERROGATE)) {
		printk("acxmem: input buffer (len=%u):\n", buflen);
		acx_dump_bytes(buffer, buflen);
	}

	/* wait for firmware to become idle for our command submission */
	counter = 199; /* in ms */
	do {
		cmd_status = acxmem_read_cmd_type_status(adev);
		/* Test for IDLE state */
		if (!cmd_status)
			break;

		udelay(1000);
	} while (likely(--counter));

	if (counter == 0) {
		/* the card doesn't get idle, we're in trouble */
		printk("acxmem: %s: %s: cmd_status is not IDLE: 0x%04X!=0\n",
				__func__, devname, cmd_status);
		goto bad;
	} else if (counter < 190) { /* if waited >10ms... */
		log(L_CTL|L_DEBUG, "acx: %s: waited for IDLE %dms. Please report\n",
				__func__, 199 - counter);
	}

	/* now write the parameters of the command if needed */
	if (buffer && buflen) {
		/* if it's an INTERROGATE command, just pass the length
		 * of parameters to read, as data */
#if CMD_DISCOVERY
		if (cmd == ACX1xx_CMD_INTERROGATE)
		memset_io(adev->cmd_area + 4, 0xAA, buflen);
#endif
		/*
		 * slave memory version
		 */
		acxmem_copy_to_slavemem(adev, (u32) (adev->cmd_area + 4), buffer, (cmd
				== ACX1xx_CMD_INTERROGATE) ? 4 : buflen);
	}
	/* now write the actual command type */
	acxmem_write_cmd_type_status(adev, cmd, 0);

	/* clear CMD_COMPLETE bit. can be set only by IRQ handler: */
	CLEAR_BIT(adev->irq_status, HOST_INT_CMD_COMPLETE);

	/* execute command */
	write_reg16(adev, IO_ACX_INT_TRIG, INT_TRIG_CMD);
	write_flush(adev);

	/* wait for firmware to process command */

	/* Ensure nonzero and not too large timeout.
	 ** Also converts e.g. 100->99, 200->199
	 ** which is nice but not essential */
	cmd_timeout = (cmd_timeout - 1) | 1;
	if (unlikely(cmd_timeout> 1199))
		cmd_timeout = 1199;

	/* we schedule away sometimes (timeout can be large) */
	counter = cmd_timeout;
	do {
		irqtype = read_reg16(adev, IO_ACX_IRQ_STATUS_NON_DES);
		if (irqtype & HOST_INT_CMD_COMPLETE) {
			write_reg16(adev, IO_ACX_IRQ_ACK, HOST_INT_CMD_COMPLETE);
			break;
		}

		if (adev->irq_status & HOST_INT_CMD_COMPLETE)
			break;

		udelay(1000);

	} while (likely(--counter));

	/* save state for debugging */
	cmd_status = acxmem_read_cmd_type_status(adev);

	/* put the card in IDLE state */
	acxmem_write_cmd_type_status(adev, ACX1xx_CMD_RESET, 0);

	/* Timed out! */
	if (counter == 0) {

		log(L_ANY, "acxmem: %s: %s: Timed out %s for CMD_COMPLETE. "
				"irq bits:0x%04X irq_status:0x%04X timeout:%dms "
				"cmd_status:%d (%s)\n",
		       __func__, devname,
		       (adev->irqs_active) ? "waiting" : "polling",
		       irqtype, adev->irq_status, cmd_timeout,
		       cmd_status, acx_cmd_status_str(cmd_status));
		log(L_ANY, "acxmem: %s: "
				"timeout: counter:%d cmd_timeout:%d cmd_timeout-counter:%d\n",
				__func__,
				counter, cmd_timeout, cmd_timeout - counter);

		if (read_reg16(adev, IO_ACX_IRQ_MASK) == 0xffff) {
			log(L_ANY,"acxmem: firmware probably hosed - reloading: FIXME: Not implmemented\n");
			FIXME();
		}

	} else if (cmd_timeout - counter > 30) { /* if waited >30ms... */
		log(L_CTL|L_DEBUG, "acxmem: %s: "
				"%s for CMD_COMPLETE %dms. count:%d. Please report\n",
				__func__,
				(adev->irqs_active) ? "waited" : "polled",
				cmd_timeout - counter, counter);
	}

	logf1(L_CTL, "%s: cmd=%s, buflen=%u, timeout=%ums, type=0x%04X: %s\n",
			devname,
			cmdstr, buflen, cmd_timeout,
			buffer ? le16_to_cpu(((acx_ie_generic_t *) buffer)->type) : -1,
			acx_cmd_status_str(cmd_status)
	);

	if (cmd_status != 1) { /* it is not a 'Success' */

		/* zero out result buffer
		 * WARNING: this will trash stack in case of illegally large input
		 * length! */

		if (buflen > 388) {
			/*
			 * 388 is maximum command length
			 */
			log(L_ANY, "acx: %s: invalid length 0x%08x\n",
					__func__, buflen);
			buflen = 388;
		}
		p = (u8 *) buffer;
		for (i = 0; i < buflen; i += 16) {
			printk("%04x:", i);
			for (j = 0; (j < 16) && (i + j < buflen); j++) {
				printk(" %02x", *p++);
			}
			printk("\n");
		}

		if (buffer && buflen)
			memset(buffer, 0, buflen);
		goto bad;
	}

	/* read in result parameters if needed */
	if (buffer && buflen && (cmd == ACX1xx_CMD_INTERROGATE)) {
		acxmem_copy_from_slavemem(adev, buffer, (u32) (adev->cmd_area + 4), buflen);
		if (acx_debug & L_DEBUG) {
			log(L_ANY, "acxmem: %s: output buffer (len=%u): ", __func__, buflen);
			acx_dump_bytes(buffer, buflen);
		}
	}

	/* ok: */
	log(L_DEBUG, "acx: %s: %s: took %ld jiffies to complete\n",
	    __func__, cmdstr, jiffies - start);

	acxmem_unlock();
	FN_EXIT1(OK);
	return OK;

	bad:
	/* Give enough info so that callers can avoid
	 ** printing their own diagnostic messages */
	logf1(L_ANY, "%s: cmd=%s, buflen=%u, timeout=%ums, type=0x%04X, status=%s: FAILED\n",
			devname,
			cmdstr, buflen, cmd_timeout,
			buffer ? le16_to_cpu(((acx_ie_generic_t *) buffer)->type) : -1,
			acx_cmd_status_str(cmd_status)
	);

	acxmem_unlock();
	FN_EXIT1(NOT_OK);
	return NOT_OK;
}

static inline void acxmem_write_cmd_type_status(acx_device_t *adev, u16 type,
		u16 status) {
	FN_ENTER;
	write_slavemem32(adev, (u32) adev->cmd_area, type | (status << 16));
	write_flush(adev);
	FN_EXIT0;
}

static u32 acxmem_read_cmd_type_status(acx_device_t *adev) {
	u32 cmd_type, cmd_status;

	FN_ENTER;

	cmd_type = read_slavemem32(adev, (u32) adev->cmd_area);

	cmd_status = (cmd_type >> 16);
	cmd_type = (u16) cmd_type;

	log(L_DEBUG, "acxmem: %s: "
			"cmd_type:%04X cmd_status:%04X [%s]\n",
			__func__,
			cmd_type, cmd_status,
			acx_cmd_status_str(cmd_status));

	FN_EXIT1(cmd_status);
	return cmd_status;
}

static inline void acxmem_init_mboxes(acx_device_t *adev) {
	u32 cmd_offs, info_offs;

	FN_ENTER;

	cmd_offs = read_reg32(adev, IO_ACX_CMD_MAILBOX_OFFS);
	info_offs = read_reg32(adev, IO_ACX_INFO_MAILBOX_OFFS);
	adev->cmd_area = (u8*) cmd_offs;
	adev->info_area = (u8*) info_offs;

	// OW iobase2 not used in mem.c, in pci.c it is
	/*
	 log(L_DEBUG, "iobase2=%p\n"
	 */
	log(L_DEBUG, "acx: cmd_mbox_offset=%X cmd_area=%p\n"
			"acx: info_mbox_offset=%X info_area=%p\n",
			cmd_offs, adev->cmd_area,
			info_offs, adev->info_area);

	FN_EXIT0;
}


/*
 * BOM Init, Configure (Control Path)
 * ==================================================
 */

/*
 * acxmem_s_reset_dev
 *
 * Arguments:
 *	netdevice that contains the adev variable
 * Returns:
 *	NOT_OK on fail
 *	OK on success
 * Side effects:
 *	device is hard reset
 * Call context:
 *	acxmem_e_probe
 * Comment:
 *	This resets the device using low level hardware calls
 *	as well as uploads and verifies the firmware to the card
 */
int acxmem_reset_dev(acx_device_t *adev) {
	const char* msg = "";
	int result = NOT_OK;
	u16 hardware_info;
	u16 ecpu_ctrl;
	int count;
	u32 tmp;
	acxmem_lock_flags;

	FN_ENTER;
	acxmem_lock();

	/*
	 write_reg32 (adev, IO_ACX_SLV_MEM_CP, 0);
	 */
	/* reset the device to make sure the eCPU is stopped
	 * to upload the firmware correctly */

	/* Windows driver does some funny things here */
	/*
	 * clear bit 0x200 in register 0x2A0
	 */
	clear_regbits(adev, 0x2A0, 0x200);

	/*
	 * Set bit 0x200 in ACX_GPIO_OUT
	 */
	set_regbits(adev, IO_ACX_GPIO_OUT, 0x200);

	/*
	 * read register 0x900 until its value is 0x8400104C, sleeping
	 * in between reads if it's not immediate
	 */
	tmp = read_reg32(adev, REG_ACX_VENDOR_ID);
	count = 500;
	while (count-- && (tmp != ACX_VENDOR_ID)) {
		mdelay (10);
		tmp = read_reg32(adev, REG_ACX_VENDOR_ID);
	}

	/* end what Windows driver does */

	acxmem_reset_mac(adev);

	ecpu_ctrl = read_reg32(adev, IO_ACX_ECPU_CTRL) & 1;
	if (!ecpu_ctrl) {
		msg = "acx: eCPU is already running. ";
		goto end_fail;
	}

#if 0
	if (read_reg16(adev, IO_ACX_SOR_CFG) & 2) {
		/* eCPU most likely means "embedded CPU" */
		msg = "acx: eCPU did not start after boot from flash. ";
		goto end_unlock;
	}

	/* check sense on reset flags */
	if (read_reg16(adev, IO_ACX_SOR_CFG) & 0x10) {
		printk("acx: %s: eCPU did not start after boot (SOR), "
				"is this fatal?\n", adev->ndev->name);
	}
#endif

	/* scan, if any, is stopped now, setting corresponding IRQ bit */
	adev->irq_status |= HOST_INT_SCAN_COMPLETE;

	/* need to know radio type before fw load */
	/* Need to wait for arrival of this information in a loop,
	 * most probably since eCPU runs some init code from EEPROM
	 * (started burst read in reset_mac()) which also
	 * sets the radio type ID */

	count = 0xffff;
	do {
		hardware_info = read_reg16(adev, IO_ACX_EEPROM_INFORMATION);
		if (!--count) {
			msg = "acx: eCPU didn't indicate radio type";
			goto end_fail;
		}
		cpu_relax();
	} while (!(hardware_info & 0xff00)); /* radio type still zero? */

	printk("acx: ACX radio type 0x%02x\n", (hardware_info >> 8) & 0xff);
	/* printk("DEBUG: count %d\n", count); */
	adev->form_factor = hardware_info & 0xff;
	adev->radio_type = hardware_info >> 8;

	acxmem_unlock();
	/* load the firmware */
	if (OK != acxmem_upload_fw(adev))
		goto end_fail;
	acxmem_lock();

	/* acx_s_mwait(10);	this one really shouldn't be required */

	/* now start eCPU by clearing bit */
	clear_regbits(adev, IO_ACX_ECPU_CTRL, 0x1);
	log(L_DEBUG, "acx: booted eCPU up and waiting for completion...\n");

	/* Windows driver clears bit 0x200 in register 0x2A0 here */
	clear_regbits(adev, 0x2A0, 0x200);

	/* Windows driver sets bit 0x200 in ACX_GPIO_OUT here */
	set_regbits(adev, IO_ACX_GPIO_OUT, 0x200);

	acxmem_unlock();
	/* wait for eCPU bootup */
	if (OK != acxmem_verify_init(adev)) {
		msg = "acx: timeout waiting for eCPU. ";
		goto end_fail;
	}
	acxmem_lock();

	log(L_DEBUG, "eCPU has woken up, card is ready to be configured\n");
	acxmem_init_mboxes(adev);
	acxmem_write_cmd_type_status(adev, ACX1xx_CMD_RESET, 0);

	/* test that EEPROM is readable */
	acxmem_read_eeprom_area(adev);

	result = OK;
	goto end;

	/* Finish error message. Indicate which function failed */
	end_fail:

	printk("acx: %sreset_dev() FAILED\n", msg);

	end:

	acxmem_unlock();
	FN_EXIT1(result);
	return result;
}

static int acxmem_verify_init(acx_device_t *adev) {
	int result = NOT_OK;
	unsigned long timeout;
	u32 irqstat;

	acxmem_lock_flags;

	FN_ENTER;

	timeout = jiffies + 2 * HZ;
	for (;;) {
		acxmem_lock();
		irqstat = read_reg32(adev, IO_ACX_IRQ_STATUS_NON_DES);
		if ((irqstat != 0xFFFFFFFF) && (irqstat & HOST_INT_FCS_THRESHOLD)) {
			result = OK;
			write_reg32(adev, IO_ACX_IRQ_ACK, HOST_INT_FCS_THRESHOLD);
			acxmem_unlock();
			break;
		}
		acxmem_unlock();

		if (time_after(jiffies, timeout))
			break;
		/* Init may take up to ~0.5 sec total */
		acx_s_mwait(50);
	}

	FN_EXIT1(result);
	return result;
}

/*
 * Most of the acx specific pieces of hardware reset.
 */
static int acxmem_complete_hw_reset(acx_device_t *adev) {
	acx111_ie_configoption_t co;
	acxmem_lock_flags;

	/* NB: read_reg() reads may return bogus data before reset_dev(),
	 * since the firmware which directly controls large parts of the I/O
	 * registers isn't initialized yet.
	 * acx100 seems to be more affected than acx111 */
	if (OK != acxmem_reset_dev(adev))
		return -1;

	acxmem_lock();
	if (IS_ACX100(adev)) {
		/* ACX100: configopt struct in cmd mailbox - directly after reset */
		acxmem_copy_from_slavemem(adev, (u8*) &co, (u32) adev->cmd_area, sizeof(co));
	}
	acxmem_unlock();

	if (OK != acx_s_init_mac(adev))
		return -3;

	if (IS_ACX111(adev)) {
		/* ACX111: configopt struct needs to be queried after full init */
		acx_s_interrogate(adev, &co, ACX111_IE_CONFIG_OPTIONS);
	}

	/*
	 * Set up transmit buffer administration
	 */
	acxmem_init_acx_txbuf(adev);

	acxmem_lock();
	/*
	 * Windows driver writes 0x01000000 to register 0x288, RADIO_CTL, if the form factor
	 * is 3.  It also write protects the EEPROM by writing 1<<9 to GPIO_OUT
	 */
	if (adev->form_factor == 3) {
		set_regbits(adev, 0x288, 0x01000000);
		set_regbits(adev, 0x298, 1 << 9);
	}

	/* TODO: merge them into one function, they are called just once and are the same for pci & usb */
	if (OK != acxmem_read_eeprom_byte(adev, 0x05, &adev->eeprom_version))
		return -2;

	acxmem_unlock();

	acx_s_parse_configoption(adev, &co);
	acx_s_get_firmware_version(adev); /* needs to be after acx_s_init_mac() */
	acx_display_hardware_details(adev);

	return 0;
}

/*
 * acxmem_l_reset_mac
 *
 * MAC will be reset
 * Call context: reset_dev
 */
static void acxmem_reset_mac(acx_device_t *adev) {
	int count;
	FN_ENTER;

	// OW Bit setting done differently in pci.c
	/* halt eCPU */
	set_regbits(adev, IO_ACX_ECPU_CTRL, 0x1);

	/* now do soft reset of eCPU, set bit */
	set_regbits(adev, IO_ACX_SOFT_RESET, 0x1);
	log(L_DEBUG, "acx: %s: enable soft reset...\n", __func__);

	/* Windows driver sleeps here for a while with this sequence */
	for (count = 0; count < 200; count++) {
		udelay (50);
	}

	/* now clear bit again: deassert eCPU reset */
	log(L_DEBUG, "acx: %s: disable soft reset and go to init mode...\n", __func__);
	clear_regbits(adev, IO_ACX_SOFT_RESET, 0x1);

	/* now start a burst read from initial EEPROM */
	set_regbits(adev, IO_ACX_EE_START, 0x1);

	/*
	 * Windows driver sleeps here for a while with this sequence
	 */
	for (count = 0; count < 200; count++) {
		udelay (50);
	}

	/* Windows driver writes 0x10000 to register 0x808 here */

	write_reg32(adev, 0x808, 0x10000);

	FN_EXIT0;
}

static void acxmem_up(struct ieee80211_hw *hw) {
	acx_device_t *adev = ieee2adev(hw);
	acxmem_lock_flags;

	FN_ENTER;

	acxmem_lock();
	acxmem_irq_enable(adev);
	acxmem_unlock();

	/* acx fw < 1.9.3.e has a hardware timer, and older drivers
	 ** used to use it. But we don't do that anymore, our OS
	 ** has reliable software timers */
	init_timer(&adev->mgmt_timer);
	adev->mgmt_timer.function = acx_i_timer;
	adev->mgmt_timer.data = (unsigned long) adev;

	/* Need to set ACX_STATE_IFACE_UP first, or else
	 ** timer won't be started by acx_set_status() */
	SET_BIT(adev->dev_state_mask, ACX_STATE_IFACE_UP);

	acx_s_start(adev);

	FN_EXIT0;
}


#if 0
/***********************************************************************
 ** acxmem_i_set_multicast_list
 ** FIXME: most likely needs refinement
 */
static void acxmem_i_set_multicast_list(struct net_device *ndev) {
	acx_device_t *adev = ndev2adev(ndev);
	unsigned long flags;

	FN_ENTER;

	acx_lock(adev, flags);

	/* firmwares don't have allmulti capability,
	 * so just use promiscuous mode instead in this case. */
	if (ndev->flags & (IFF_PROMISC|IFF_ALLMULTI)) {
		SET_BIT(adev->rx_config_1, RX_CFG1_RCV_PROMISCUOUS);
		CLEAR_BIT(adev->rx_config_1, RX_CFG1_FILTER_ALL_MULTI);
		SET_BIT(adev->set_mask, SET_RXCONFIG);
		/* let kernel know in case *we* needed to set promiscuous */
		ndev->flags |= (IFF_PROMISC|IFF_ALLMULTI);
	} else {
		CLEAR_BIT(adev->rx_config_1, RX_CFG1_RCV_PROMISCUOUS);
		SET_BIT(adev->rx_config_1, RX_CFG1_FILTER_ALL_MULTI);
		SET_BIT(adev->set_mask, SET_RXCONFIG);
		ndev->flags &= ~(IFF_PROMISC|IFF_ALLMULTI);
	}

	/* cannot update card settings directly here, atomic context */
	acx_schedule_task(adev, ACX_AFTER_IRQ_UPDATE_CARD_CFG);

	acx_unlock(adev, flags);

	FN_EXIT0;
}
#endif


/*
 * BOM Other (Control Path)
 * ==================================================
 */

/*
 * BOM Proc, Debug
 * ==================================================
 */
int acxmem_proc_diag_output(struct seq_file *file, acx_device_t *adev) {
	const char *rtl, *thd, *ttl;
	txdesc_t *txdesc;
	u8 Ctl_8;
	rxdesc_t *rxdesc;
	int i;
	u32 tmp, tmp2;
	txdesc_t txd;
	rxdesc_t rxd;

	acxmem_lock_flags;

	FN_ENTER;
	acxmem_lock();

#if DUMP_MEM_DURING_DIAG > 0
	acxmem_dump_mem (adev, 0, 0x10000);
	panic ("dump finished");
#endif

	seq_printf(file, "** Rx buf **\n");
	rxdesc = adev->rxdesc_start;
	if (rxdesc)
		for (i = 0; i < RX_CNT; i++) {
			rtl = (i == adev->rx_tail) ? " [tail]" : "";
			Ctl_8 = read_slavemem8(adev, (u32) &(rxdesc->Ctl_8));
			if (Ctl_8 & DESC_CTL_HOSTOWN)
				seq_printf(file, "%02u (%02x) FULL %-10s", i, Ctl_8, rtl);
			else
				seq_printf(file, "%02u (%02x) empty%-10s", i, Ctl_8, rtl);

			//seq_printf(file, "\n");

			acxmem_copy_from_slavemem(adev, (u8 *) &rxd, (u32) rxdesc, sizeof(rxd));
			seq_printf(file,
						 "%04x: %04x %04x %04x %04x %04x %04x %04x Ctl_8=%04x %04x %04x %04x %04x %04x %04x %04x\n",
							(u32) rxdesc,
							rxd.pNextDesc.v,
							rxd.HostMemPtr.v,
							rxd.ACXMemPtr.v,
							rxd.rx_time,
							rxd.total_length,
							rxd.WEP_length,
							rxd.WEP_ofs,
							rxd.Ctl_8,
							rxd.rate,
							rxd.error,
							rxd.SNR,
							rxd.RxLevel,
							rxd.queue_ctrl,
							rxd.unknown,
							rxd.unknown2);
			rxdesc++;
		}

	seq_printf(file, "** Tx buf (free %d, Ieee80211 queue: %s) **\n",
			adev->acx_txbuf_free, acx_queue_stopped(adev->ieee) ? "STOPPED"
					: "Running");

	seq_printf(file,
			"** Tx buf %d blocks total, %d available, free list head %04x\n",
			adev->acx_txbuf_numblocks, adev->acx_txbuf_blocks_free,
			adev->acx_txbuf_free);

	txdesc = adev->txdesc_start;
	if (txdesc) {
		for (i = 0; i < TX_CNT; i++) {
			thd = (i == adev->tx_head) ? " [head]" : "";
			ttl = (i == adev->tx_tail) ? " [tail]" : "";
			acxmem_copy_from_slavemem(adev, (u8 *) &txd, (u32) txdesc, sizeof(txd));

			Ctl_8 = read_slavemem8(adev, (u32) &(txdesc->Ctl_8));
			if (Ctl_8 & DESC_CTL_ACXDONE)
				seq_printf(file, "%02u ready to free (%02X)%-7s%-7s", i, Ctl_8, thd, ttl);
			else if (Ctl_8 & DESC_CTL_HOSTOWN)
				seq_printf(file, "%02u available     (%02X)%-7s%-7s", i, Ctl_8, thd, ttl);
			else
				seq_printf(file, "%02u busy          (%02X)%-7s%-7s", i, Ctl_8, thd, ttl);

			seq_printf(file,
						 "%04x: %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %02x %02x %02x %02x "
						 "%02x %02x %02x %02x %04x: ", (u32) txdesc,
						 txd.pNextDesc.v, txd.HostMemPtr.v, txd.AcxMemPtr.v,
						 txd.tx_time, txd.total_length, txd.Reserved,
						 txd.dummy[0], txd.dummy[1], txd.dummy[2],
						 txd.dummy[3], txd.Ctl_8, txd.Ctl2_8, txd.error,
						 txd.ack_failures, txd.rts_failures,
						 txd.rts_ok, txd.u.r1.rate,
						 txd.u.r1.queue_ctrl, txd.queue_info);

			tmp = read_slavemem32(adev, (u32) & (txdesc->AcxMemPtr));
			seq_printf(file, " %04x: ", tmp);

			// Output allocated tx-buffer chain
#if 1
			if (tmp) {
				while ((tmp2 = read_slavemem32(adev, (u32) tmp)) != 0x02000000) {
					tmp2 = tmp2 << 5;
					seq_printf(file, "%04x=%04x,", tmp, tmp2);
					tmp = tmp2;
				}
				seq_printf(file, " %04x=%04x", tmp, tmp2);
			}
#endif
			seq_printf(file, "\n");

#if 0
			u8 buf[0x200];
			int j, k;

			if (txd.AcxMemPtr.v) {
				acxmem_copy_from_slavemem(adev, buf, txd.AcxMemPtr.v, sizeof(buf));
				for (j = 0; (j < txd.total_length) && (j < (sizeof(buf) - 4)); j
						+= 16) {
					seq_printf(file, "    ");
					for (k = 0; (k < 16) && (j + k < txd.total_length); k++) {
						seq_printf(file, " %02x", buf[j + k + 4]);
					}
					seq_printf(file, "\n");
				}
			}
#endif

			txdesc = acxmem_advance_txdesc(adev, txdesc, 1);
		}
	}

#if 1
	// Tx-buffer list dump
	seq_printf(file, "\n");
	seq_printf(file, "* Tx-buffer list dump\n");
	seq_printf(file, "acx_txbuf_numblocks=%d, acx_txbuf_blocks_free=%d, \n"
		"acx_txbuf_start==%04x, acx_txbuf_free=%04x, memblocksize=%d\n",
			adev->acx_txbuf_numblocks, adev->acx_txbuf_blocks_free,
			adev->acx_txbuf_start, adev->acx_txbuf_free, adev->memblocksize);

	tmp = adev->acx_txbuf_start;
	for (i = 0; i < adev->acx_txbuf_numblocks; i++) {
		tmp2 = read_slavemem32(adev, (u32) tmp);
		seq_printf(file, "%02d: %04x=%04x,%04x\n", i, tmp, tmp2, tmp2 << 5);

		tmp += adev->memblocksize;
	}
	seq_printf(file, "\n");
	// ---
#endif

	seq_printf(file, "\n"
		"** Generic slave data **\n"
		"irq_mask 0x%04x irq_status 0x%04x irq on acx 0x%04x\n"

		"txbuf_start 0x%p, txbuf_area_size %u\n"
		// OW TODO Add also the acx tx_buf size available
		"txdesc_size %u, txdesc_start 0x%p\n"
		"txhostdesc_start 0x%p, txhostdesc_area_size %u\n"
		"txbuf start 0x%04x, txbuf size %d\n"

		"rxdesc_start 0x%p\n"
		"rxhostdesc_start 0x%p, rxhostdesc_area_size %u\n"
		"rxbuf_start 0x%p, rxbuf_area_size %u\n",

		adev->irq_mask,	adev->irq_status, read_reg32(adev, IO_ACX_IRQ_STATUS_NON_DES),

		adev->txbuf_start, adev->txbuf_area_size, adev->txdesc_size,
		adev->txdesc_start, adev->txhostdesc_start,
		adev->txhostdesc_area_size, adev->acx_txbuf_start,
		adev->acx_txbuf_numblocks * adev->memblocksize,

		adev->rxdesc_start,
		adev->rxhostdesc_start, adev->rxhostdesc_area_size,
		adev->rxbuf_start, adev->rxbuf_area_size);

	acxmem_unlock();
	FN_EXIT0;
	return 0;
}

int acxmem_proc_eeprom_output(char *buf, acx_device_t *adev) {
	char *p = buf;
	int i;
	acxmem_lock_flags;

	FN_ENTER;
	acxmem_lock();

	for (i = 0; i < 0x400; i++) {
		acxmem_read_eeprom_byte(adev, i, p++);
	}

	acxmem_unlock();
	FN_EXIT1(p - buf);
	return p - buf;
}

/*
 * BOM Rx Path
 * ==================================================
 */

/*
 * acxmem_l_process_rxdesc
 *
 * Called directly and only from the IRQ handler
 */
static void acxmem_process_rxdesc(acx_device_t *adev) {
	register rxhostdesc_t *hostdesc;
	register rxdesc_t *rxdesc;
	unsigned count, tail;
	u32 addr;
	u8 Ctl_8;

	FN_ENTER;

	if (unlikely(acx_debug & L_BUFR))
		acxmem_log_rxbuffer(adev);

	/* First, have a loop to determine the first descriptor that's
	 * full, just in case there's a mismatch between our current
	 * rx_tail and the full descriptor we're supposed to handle. */
	tail = adev->rx_tail;
	count = RX_CNT;
	while (1) {
		hostdesc = &adev->rxhostdesc_start[tail];
		rxdesc = &adev->rxdesc_start[tail];
		/* advance tail regardless of outcome of the below test */
		tail = (tail + 1) % RX_CNT;

		/*
		 * Unlike the PCI interface, where the ACX can write directly to
		 * the host descriptors, on the slave memory interface we have to
		 * pull these.  All we really need to do is check the Ctl_8 field
		 * in the rx descriptor on the ACX, which should be 0x11000000 if
		 * we should process it.
		 */
		Ctl_8 = hostdesc->Ctl_16 = read_slavemem8(adev, (u32) &(rxdesc->Ctl_8));
		if ((Ctl_8 & DESC_CTL_HOSTOWN) && (Ctl_8 & DESC_CTL_ACXDONE))
			break; /* found it! */

		if (unlikely(!--count)) /* hmm, no luck: all descs empty, bail out */
			goto end;
	}

	/* now process descriptors, starting with the first we figured out */
	while (1) {
		log(L_BUFR, "acxmem: %s: rx: tail=%u Ctl_8=%02X\n", __func__, tail, Ctl_8);
		/*
		 * If the ACX has CTL_RECLAIM set on this descriptor there
		 * is no buffer associated; it just wants us to tell it to
		 * reclaim the memory.
		 */
		if (!(Ctl_8 & DESC_CTL_RECLAIM)) {

			/*
			 * slave interface - pull data now
			 */
			hostdesc->length = read_slavemem16(adev,
					(u32) &(rxdesc->total_length));

			/*
			 * hostdesc->data is an rxbuffer_t, which includes header information,
			 * but the length in the data packet doesn't.  The header information
			 * takes up an additional 12 bytes, so add that to the length we copy.
			 */
			addr = read_slavemem32(adev, (u32) &(rxdesc->ACXMemPtr));
			if (addr) {
				/*
				 * How can &(rxdesc->ACXMemPtr) above ever be zero?  Looks like we
				 * get that now and then - try to trap it for debug.
				 */
				if (addr & 0xffff0000) {
					log(L_ANY, "acxmem: %s: rxdesc 0x%08x\n", __func__, (u32) rxdesc);
					acxmem_dump_mem(adev, 0, 0x10000);
					panic("Bad access!");
				}
				acxmem_chaincopy_from_slavemem(adev, (u8 *) hostdesc->data, addr,
						hostdesc->length + (u32) &((rxbuffer_t *) 0)->hdr_a3);

				acx_l_process_rxbuf(adev, hostdesc->data);
			}
		} else {
			log(L_ANY, "acxmem: %s: rx reclaim only!\n", __func__);
		}

		hostdesc->Status = 0;

		/*
		 * Let the ACX know we're done.
		 */
		CLEAR_BIT (Ctl_8, DESC_CTL_HOSTOWN);
		SET_BIT (Ctl_8, DESC_CTL_HOSTDONE);
		SET_BIT (Ctl_8, DESC_CTL_RECLAIM);
		write_slavemem8(adev, (u32) &rxdesc->Ctl_8, Ctl_8);

		/*
		 * Now tell the ACX we've finished with the receive buffer so
		 * it can finish the reclaim.
		 */
		write_reg16(adev, IO_ACX_INT_TRIG, INT_TRIG_RXPRC);

		/* ok, descriptor is handled, now check the next descriptor */
		hostdesc = &adev->rxhostdesc_start[tail];
		rxdesc = &adev->rxdesc_start[tail];

		Ctl_8 = hostdesc->Ctl_16 = read_slavemem8(adev, (u32) &(rxdesc->Ctl_8));

		/* if next descriptor is empty, then bail out */
		if (!(Ctl_8 & DESC_CTL_HOSTOWN) || !(Ctl_8 & DESC_CTL_ACXDONE))
			break;

		tail = (tail + 1) % RX_CNT;
	}
	end:
		adev->rx_tail = tail;
		FN_EXIT0;
}

/*
 * BOM Tx Path
 * ==================================================
 */

/*
 * acxmem_l_alloc_tx
 * Actually returns a txdesc_t* ptr
 *
 * FIXME: in case of fragments, should allocate multiple descrs
 * after figuring out how many we need and whether we still have
 * sufficiently many.
 */
 // OW TODO Align with pci.c
tx_t *acxmem_alloc_tx(acx_device_t *adev, unsigned int len) {
	struct txdesc *txdesc;
	unsigned head;
	u8 ctl8;
	static int txattempts = 0;
	int blocks_needed;
	acxmem_lock_flags;

	FN_ENTER;
	acxmem_lock();

	if (unlikely(!adev->tx_free)) {
		log(L_ANY, "acxmem: %s: BUG: no free txdesc left\n", __func__);
		/*
		 * Probably the ACX ignored a transmit attempt and now there's a packet
		 * sitting in the queue we think should be transmitting but the ACX doesn't
		 * know about.
		 * On the first pass, send the ACX a TxProc interrupt to try moving
		 * things along, and if that doesn't work (ie, we get called again) completely
		 * flush the transmit queue.
		 */
		if (txattempts < 10) {
			txattempts++;
			log(L_ANY, "acxmem: %s: trying to wake up ACX\n", __func__);
			write_reg16(adev, IO_ACX_INT_TRIG, INT_TRIG_TXPRC);
			write_flush(adev);
		} else {
			txattempts = 0;
			log(L_ANY, "acxmem: %s: flushing transmit queue.\n", __func__);
			acxmem_clean_txdesc_emergency(adev);
		}
		txdesc = NULL;
		goto end;
	}

	/*
	 * Make a quick check to see if there is transmit buffer space on
	 * the ACX.  This can't guarantee there is enough space for the packet
	 * since we don't yet know how big it is, but it will prevent at least some
	 * annoyances.
	 */

	/* OW 20090815
	 * Make a detailed check of required tx_buf blocks, to avoid 'out of tx_buf' situation.
	 *
	 * The empty tx_buf and reuse trick in acxmem_l_tx_data doen't wotk well.
	 * I think it confused mac80211, that was supposing the packet was send,
	 * but actually it was dropped.	According to mac80211 dropping should not happen,
	 * altough a bit of dropping seemed to be ok.
	 *
	 * What we do now is simpler and clean I think:
	 * - first check the number of required blocks
	 * - and if there are not enough: Stop the queue and report NOT_OK.
	 *
	 * Reporting NOT_OK here shouldn't be done neither according to mac80211,
	 * but it seems to work better here.
   	*/

	blocks_needed=acxmem_get_txbuf_space_needed(adev, len);
	if (!(blocks_needed <= adev->acx_txbuf_blocks_free)) {
		txdesc = NULL;
		log(L_BUFT, "acxmem: %s: !(blocks_needed <= adev->acx_txbuf_blocks_free), "
				"len=%i, blocks_needed=%i, acx_txbuf_blocks_free=%i: "
				"Stopping queue.\n",
				__func__,
				len, blocks_needed, adev->acx_txbuf_blocks_free);
		acx_stop_queue(adev->ieee, NULL);
		goto end;
	}

	head = adev->tx_head;
	/*
	 * txdesc points to ACX memory
	 */
	txdesc = acxmem_get_txdesc(adev, head);
	ctl8 = read_slavemem8(adev, (u32) &(txdesc->Ctl_8));

	/*
	 * If we don't own the buffer (HOSTOWN) it is certainly not free; however,
	 * we may have previously thought we had enough memory to send
	 * a packet, allocated the buffer then gave up when we found not enough
	 * transmit buffer space on the ACX. In that case, HOSTOWN and
	 * ACXDONE will both be set.
	 */

	// TODO OW Check if this is correct
	// TODO 20100115 Changed to DESC_CTL_ACXDONE_HOSTOWN like in pci.c
	if (unlikely(DESC_CTL_HOSTOWN != (ctl8 & DESC_CTL_ACXDONE_HOSTOWN))) {
		/* whoops, descr at current index is not free, so probably
		 * ring buffer already full */
		log(L_ANY, "acxmem: %s: BUG: tx_head:%d Ctl8:0x%02X - failed to find free txdesc\n",
			__func__,
			head, ctl8);
		txdesc = NULL;
		goto end;
	}

	/* Needed in case txdesc won't be eventually submitted for tx */
	write_slavemem8(adev, (u32) &(txdesc->Ctl_8), DESC_CTL_ACXDONE_HOSTOWN);

	adev->tx_free--;
	log(L_BUFT, "acxmem: %s: tx: got desc %u, %u remain\n",
			__func__, head, adev->tx_free);

	/* returning current descriptor, so advance to next free one */
	adev->tx_head = (head + 1) % TX_CNT;

	end:

	acxmem_unlock();
	FN_EXIT0;

	return (tx_t*) txdesc;
}

/*
 * acxmem_l_dealloc_tx
 *
 * Clears out a previously allocatedvoid acxmem_l_dealloc_tx(tx_t *tx_opaque);
 * transmit descriptor.
 * The ACX can get confused if we skip transmit descriptors in the queue,
 * so when we don't need a descriptor return it to its original
 * state and move the queue head pointer back.
 *
 */
void acxmem_dealloc_tx(acx_device_t *adev, tx_t *tx_opaque) {
	/*
	 * txdesc is the address of the descriptor on the ACX.
	 */
	txdesc_t *txdesc = (txdesc_t*) tx_opaque;
	txdesc_t tmptxdesc;
	int index;

	acxmem_lock_flags;
	acxmem_lock();

	memset (&tmptxdesc, 0, sizeof(tmptxdesc));
	tmptxdesc.Ctl_8 = DESC_CTL_HOSTOWN | DESC_CTL_FIRSTFRAG;
	tmptxdesc.u.r1.rate = 0x0a;

	/*
	 * Clear out all of the transmit descriptor except for the next pointer
	 */
	acxmem_copy_to_slavemem(adev, (u32) &(txdesc->HostMemPtr),
			(u8 *) &(tmptxdesc.HostMemPtr), sizeof(tmptxdesc)
					- sizeof(tmptxdesc.pNextDesc));

	/*
	 * This is only called immediately after we've allocated, so we should
	 * be able to set the head back to this descriptor.
	 */
	index = ((u8*) txdesc - (u8*) adev->txdesc_start) / adev->txdesc_size;
	printk("acx_dealloc: moving head from %d to %d\n", adev->tx_head, index);
	adev->tx_head = index;

	acxmem_unlock();

}

void *acxmem_get_txbuf(acx_device_t *adev, tx_t *tx_opaque) {
	return acxmem_get_txhostdesc(adev, (txdesc_t*) tx_opaque)->data;
}

static int acxmem_get_txbuf_space_needed(acx_device_t *adev, unsigned int len) {
	int blocks_needed;

	blocks_needed = len / (adev->memblocksize - 4);
	if (len % (adev->memblocksize - 4))
		blocks_needed++;

	return (blocks_needed);
}

/*
 * Return an acx pointer to the next transmit data block.
 */
static u32 acxmem_allocate_acx_txbuf_space(acx_device_t *adev, int count) {
	u32 block, next, last_block;
	int blocks_needed;

	/*
	 * Take 4 off the memory block size to account for the reserved word at the start of
	 * the block.
	 */
	blocks_needed=acxmem_get_txbuf_space_needed(adev, count);

	if (blocks_needed <= adev->acx_txbuf_blocks_free) {
		/*
		 * Take blocks at the head of the free list.
		 */
		last_block = block = adev->acx_txbuf_free;

		/*
		 * Follow block pointers through the requested number of blocks both to
		 * find the new head of the free list and to set the flags for the blocks
		 * appropriately.
		 */
		while (blocks_needed--) {
			/*
			 * Keep track of the last block of the allocation
			 */
			last_block = adev->acx_txbuf_free;

			/*
			 * Make sure the end control flag is not set.
			 */
			next = read_slavemem32(adev, adev->acx_txbuf_free) & 0x7ffff;
			write_slavemem32(adev, adev->acx_txbuf_free, next);

			/*
			 * Update the new head of the free list
			 */
			adev->acx_txbuf_free = next << 5;
			adev->acx_txbuf_blocks_free--;

		}

		/*
		 * Flag the last block both by clearing out the next pointer
		 * and marking the control field.
		 */
		write_slavemem32(adev, last_block, 0x02000000);

		/*
		 * If we're out of buffers make sure the free list pointer is NULL
		 */
		if (!adev->acx_txbuf_blocks_free) {
			adev->acx_txbuf_free = 0;
		}
	} else {
		block = 0;
	}

	return block;
}

/*
 * Return buffer space back to the pool by following the next pointers until we find
 * the block marked as the end.  Point the last block to the head of the free list,
 * then update the head of the free list to point to the newly freed memory.
 * This routine gets called in interrupt context, so it shouldn't block to protect
 * the integrity of the linked list.  The ISR already holds the lock.
 */
static void acxmem_reclaim_acx_txbuf_space(acx_device_t *adev, u32 blockptr) {
	u32 cur, last, next;

	if ((blockptr >= adev->acx_txbuf_start) &&
		(blockptr <= adev->acx_txbuf_start +
		(adev->acx_txbuf_numblocks - 1)	* adev->memblocksize)) {
		cur = blockptr;
		do {
			last = cur;
			next = read_slavemem32(adev, cur);

			/*
			 * Advance to the next block in this allocation
			 */
			cur = (next & 0x7ffff) << 5;

			/*
			 * This block now counts as free.
			 */
			adev->acx_txbuf_blocks_free++;
		} while (!(next & 0x02000000));

		/*
		 * last now points to the last block of that allocation.  Update the pointer
		 * in that block to point to the free list and reset the free list to the
		 * first block of the free call.  If there were no free blocks, make sure
		 * the new end of the list marks itself as truly the end.
		 */
		if (adev->acx_txbuf_free) {
			write_slavemem32(adev, last, adev->acx_txbuf_free >> 5);
		} else {
			write_slavemem32(adev, last, 0x02000000);
		}
		adev->acx_txbuf_free = blockptr;
	}

}

/*
 * Initialize the pieces managing the transmit buffer pool on the ACX.  The transmit
 * buffer is a circular queue with one 32 bit word reserved at the beginning of each
 * block.  The upper 13 bits are a control field, of which only 0x02000000 has any
 * meaning.  The lower 19 bits are the address of the next block divided by 32.
 */
static void acxmem_init_acx_txbuf(acx_device_t *adev) {

	/*
	 * acx100_s_init_memory_pools set up txbuf_start and txbuf_numblocks for us.
	 * All we need to do is reset the rest of the bookeeping.
	 */

	adev->acx_txbuf_free = adev->acx_txbuf_start;
	adev->acx_txbuf_blocks_free = adev->acx_txbuf_numblocks;

	/*
	 * Initialization leaves the last transmit pool block without a pointer back to
	 * the head of the list, but marked as the end of the list.  That's how we want
	 * to see it, too, so leave it alone.  This is only ever called after a firmware
	 * reset, so the ACX memory is in the state we want.
	 */

}

/* Re-initialize tx-buffer list
 */
void acxmem_init_acx_txbuf2(acx_device_t *adev) {

	int i;
	u32 adr, next_adr;

	adr = adev->acx_txbuf_start;
	for (i = 0; i < adev->acx_txbuf_numblocks; i++) {
		next_adr = adr + adev->memblocksize;

		// Last block is marked with 0x02000000
		if (i == adev->acx_txbuf_numblocks - 1) {
			write_slavemem32(adev, adr, 0x02000000);
		}
		// Else write pointer to next block
		else {
			write_slavemem32(adev, adr, (next_adr >> 5));
		}
		adr = next_adr;
	}

	adev->acx_txbuf_free = adev->acx_txbuf_start;
	adev->acx_txbuf_blocks_free = adev->acx_txbuf_numblocks;

}

static inline txdesc_t*
acxmem_get_txdesc(acx_device_t *adev, int index) {
	return (txdesc_t*) (((u8*) adev->txdesc_start) + index * adev->txdesc_size);
}

static inline txdesc_t*
acxmem_advance_txdesc(acx_device_t *adev, txdesc_t* txdesc, int inc) {
	return (txdesc_t*) (((u8*) txdesc) + inc * adev->txdesc_size);
}

static txhostdesc_t*
acxmem_get_txhostdesc(acx_device_t *adev, txdesc_t* txdesc) {
	int index = (u8*) txdesc - (u8*) adev->txdesc_start;
	if (unlikely(ACX_DEBUG && (index % adev->txdesc_size))) {
		printk("bad txdesc ptr %p\n", txdesc);
		return NULL;
	}
	index /= adev->txdesc_size;
	if (unlikely(ACX_DEBUG && (index >= TX_CNT))) {
		printk("bad txdesc ptr %p\n", txdesc);
		return NULL;
	}
	return &adev->txhostdesc_start[index * 2];
}

/*
 * acxmem_l_tx_data
 *
 * Can be called from IRQ (rx -> (AP bridging or mgmt response) -> tx).
 * Can be called from acx_i_start_xmit (data frames from net core).
 *
 * FIXME: in case of fragments, should loop over the number of
 * pre-allocated tx descrs, properly setting up transfer data and
 * CTL_xxx flags according to fragment number.
 */
void acxmem_tx_data(acx_device_t *adev, tx_t *tx_opaque, int len,
			struct ieee80211_tx_info *ieeectl, struct sk_buff *skb) {
	/*
	 * txdesc is the address on the ACX
	 */
	txdesc_t *txdesc = (txdesc_t*) tx_opaque;
	struct ieee80211_hdr *wireless_header;
	txhostdesc_t *hostdesc1, *hostdesc2;
	int rate;
	u8 Ctl_8, Ctl2_8;
	int wlhdr_len;
	u32 addr;

	acxmem_lock_flags;

	FN_ENTER;
	acxmem_lock();

	/* fw doesn't tx such packets anyhow */
	/* if (unlikely(len < WLAN_HDR_A3_LEN))
		goto end;
	 */

	hostdesc1 = acxmem_get_txhostdesc(adev, txdesc);
	wireless_header = (struct ieee80211_hdr *) hostdesc1->data;

	// wlhdr_len = ieee80211_hdrlen(le16_to_cpu(wireless_header->frame_control));
	wlhdr_len = WLAN_HDR_A3_LEN;

	/* modify flag status in separate variable to be able to write it back
	 * in one big swoop later (also in order to have less device memory
	 * accesses) */
	Ctl_8 = read_slavemem8(adev, (u32) &(txdesc->Ctl_8));
	Ctl2_8 = 0; /* really need to init it to 0, not txdesc->Ctl2_8, it seems */

	hostdesc2 = hostdesc1 + 1;

	/* DON'T simply set Ctl field to 0 here globally,
	 * it needs to maintain a consistent flag status (those are state flags!!),
	 * otherwise it may lead to severe disruption. Only set or reset particular
	 * flags at the exact moment this is needed... */

	/* let chip do RTS/CTS handshaking before sending
	 * in case packet size exceeds threshold */

	// if (len > adev->rts_threshold)
	if (ieeectl->flags & IEEE80211_TX_RC_USE_RTS_CTS)
		SET_BIT(Ctl2_8, DESC_CTL2_RTS);
	else
		CLEAR_BIT(Ctl2_8, DESC_CTL2_RTS);

	rate = ieee80211_get_tx_rate(adev->ieee, ieeectl)->bitrate;

	write_slavemem16(adev, (u32) &(txdesc->total_length), cpu_to_le16(len));
	hostdesc2->length = cpu_to_le16(len - wlhdr_len);

	/* ACX111 */
	if (IS_ACX111(adev)) {
		/* note that if !txdesc->do_auto, txrate->cur
		 ** has only one nonzero bit */
		txdesc->u.r2.rate111 = cpu_to_le16(rate);

		/* WARNING: I was never able to make it work with prism54 AP.
		 ** It was falling down to 1Mbit where shortpre is not applicable,
		 ** and not working at all at "5,11 basic rates only" setting.
		 ** I even didn't see tx packets in radio packet capture.
		 ** Disabled for now --vda */
		/*| ((clt->shortpre && clt->cur!=RATE111_1) ? RATE111_SHORTPRE : 0) */

#ifdef TODO_FIGURE_OUT_WHEN_TO_SET_THIS
		/* should add this to rate111 above as necessary */
		| (clt->pbcc511 ? RATE111_PBCC511 : 0)
#endif
		hostdesc1->length = cpu_to_le16(len);
	}
	/* ACX100 */
	else {
		u8 rate_100 = rate;
		write_slavemem8(adev, (u32) &(txdesc->u.r1.rate), rate_100);

#ifdef TODO_FIGURE_OUT_WHEN_TO_SET_THIS
		if (clt->pbcc511) {
			if (n == RATE100_5 || n == RATE100_11)
			n |= RATE100_PBCC511;
		}

		if (clt->shortpre && (clt->cur != RATE111_1))
		SET_BIT(Ctl_8, DESC_CTL_SHORT_PREAMBLE); /* set Short Preamble */
#endif

		/* set autodma and reclaim and 1st mpdu */
		SET_BIT(Ctl_8, DESC_CTL_FIRSTFRAG);

#if ACX_FRAGMENTATION
		/* SET_BIT(Ctl2_8, DESC_CTL2_MORE_FRAG); cannot set it unconditionally, needs to be set for all non-last fragments */
#endif

		hostdesc1->length = cpu_to_le16(wlhdr_len);

		/*
		 * Since we're not using autodma copy the packet data to the acx now.
		 * Even host descriptors point to the packet header, and the odd indexed
		 * descriptor following points to the packet data.
		 *
		 * The first step is to find free memory in the ACX transmit buffers.
		 * They don't necessarily map one to one with the transmit queue entries,
		 * so search through them starting just after the last one used.
		 */
		addr = acxmem_allocate_acx_txbuf_space(adev, len);
		if (addr) {
			acxmem_chaincopy_to_slavemem(adev, addr, hostdesc1->data, len);
		} else {
			/*
			 * Bummer.  We thought we might have enough room in the transmit
			 * buffers to send this packet, but it turns out we don't.  alloc_tx
			 * has already marked this transmit descriptor as HOSTOWN and ACXDONE,
			 * which means the ACX will hang when it gets to this descriptor unless
			 * we do something about it.  Having a bubble in the transmit queue just
			 * doesn't seem to work, so we have to reset this transmit queue entry's
			 * state to its original value and back up our head pointer to point
			 * back to this entry.
			 */
			 // OW FIXME Logging
			printk("acxmem_l_tx_data: Bummer. Not enough room in the txbuf_space.\n");
			hostdesc1->length = 0;
			hostdesc2->length = 0;
			write_slavemem16(adev, (u32) &(txdesc->total_length), 0);
			write_slavemem8(adev, (u32) &(txdesc->Ctl_8), DESC_CTL_HOSTOWN
					| DESC_CTL_FIRSTFRAG);
			adev->tx_head = ((u8*) txdesc - (u8*) adev->txdesc_start)
					/ adev->txdesc_size;
			adev->tx_free++;
			goto end;
		}
		/*
		 * Tell the ACX where the packet is.
		 */
		write_slavemem32(adev, (u32) &(txdesc->AcxMemPtr), addr);

	}
	/* don't need to clean ack/rts statistics here, already
	 * done on descr cleanup */

	/* clears HOSTOWN and ACXDONE bits, thus telling that the descriptors
	 * are now owned by the acx100; do this as LAST operation */
	CLEAR_BIT(Ctl_8, DESC_CTL_ACXDONE_HOSTOWN);
	/* flush writes before we release hostdesc to the adapter here */
	//wmb();


	/* write back modified flags */
	//At this point Ctl_8 should just be FIRSTFRAG
	CLEAR_BIT(Ctl2_8, DESC_CTL2_WEP);
	write_slavemem8(adev, (u32) &(txdesc->Ctl2_8), Ctl2_8);
	write_slavemem8(adev, (u32) &(txdesc->Ctl_8), Ctl_8);
	/* unused: txdesc->tx_time = cpu_to_le32(jiffies); */

	/*
	 * Update the queue indicator to say there's data on the first queue.
	 */
	acxmem_update_queue_indicator(adev, 0);

	/* flush writes before we tell the adapter that it's its turn now */
	mmiowb();
	write_reg16(adev, IO_ACX_INT_TRIG, INT_TRIG_TXPRC);
	write_flush(adev);

	hostdesc1->skb = skb;

	/* log the packet content AFTER sending it,
	 * in order to not delay sending any further than absolutely needed
	 * Do separate logs for acx100/111 to have human-readable rates */

	end:

	// Debugging
	if (unlikely(acx_debug & (L_XFER|L_DATA))) {
		u16 fc = ((struct ieee80211_hdr *) hostdesc1->data)->frame_control;
		if (IS_ACX111(adev))
			printk("acx: tx: pkt (%s): len %d "
				"rate %04X%s status %u\n", acx_get_packet_type_string(
					le16_to_cpu(fc)), len, le16_to_cpu(txdesc->u.r2.rate111), (le16_to_cpu(txdesc->u.r2.rate111)& RATE111_SHORTPRE) ? "(SPr)" : "",
					adev->status);
		else
			printk("acx: tx: pkt (%s): len %d rate %03u%s status %u\n",
					acx_get_packet_type_string(fc), len, read_slavemem8(adev,
							(u32) &(txdesc->u.r1.rate)), (Ctl_8
							& DESC_CTL_SHORT_PREAMBLE) ? "(SPr)" : "",
					adev->status);

		if (0 && acx_debug & L_DATA) {
			printk("acx: tx: 802.11 [%d]: ", len);
			acx_dump_bytes(hostdesc1->data, len);
		}
	}

	acxmem_unlock();

	FN_EXIT0;
}


/*
 * acxmem_l_clean_txdesc
 *
 * This function resets the txdescs' status when the ACX100
 * signals the TX done IRQ (txdescs have been processed), starting with
 * the pool index of the descriptor which we would use next,
 * in order to make sure that we can be as fast as possible
 * in filling new txdescs.
 * Everytime we get called we know where the next packet to be cleaned is.
 */
// OW TODO Very similar with pci: possible merging.
unsigned int acxmem_clean_txdesc(acx_device_t *adev) {
	txdesc_t *txdesc;
	txhostdesc_t *hostdesc;
	unsigned finger;
	int num_cleaned;
	u16 r111;
	u8 error, ack_failures, rts_failures, rts_ok, r100, Ctl_8;
	u32 acxmem;
	txdesc_t tmptxdesc;

	struct ieee80211_tx_info *txstatus;

	FN_ENTER;

	/*
	 * Set up a template descriptor for re-initialization.  The only
	 * things that get set are Ctl_8 and the rate, and the rate defaults
	 * to 1Mbps.
	 */
	memset (&tmptxdesc, 0, sizeof (tmptxdesc));
	tmptxdesc.Ctl_8 = DESC_CTL_HOSTOWN | DESC_CTL_FIRSTFRAG;
	tmptxdesc.u.r1.rate = 0x0a;

	if (unlikely(acx_debug & L_DEBUG))
		acxmem_log_txbuffer(adev);

	log(L_BUFT, "acx: tx: cleaning up bufs from %u\n", adev->tx_tail);

	/* We know first descr which is not free yet. We advance it as far
	 ** as we see correct bits set in following descs (if next desc
	 ** is NOT free, we shouldn't advance at all). We know that in
	 ** front of tx_tail may be "holes" with isolated free descs.
	 ** We will catch up when all intermediate descs will be freed also */

	finger = adev->tx_tail;
	num_cleaned = 0;
	while (likely(finger != adev->tx_head)) {
		txdesc = acxmem_get_txdesc(adev, finger);

		/* If we allocated txdesc on tx path but then decided
		 ** to NOT use it, then it will be left as a free "bubble"
		 ** in the "allocated for tx" part of the ring.
		 ** We may meet it on the next ring pass here. */

		/* stop if not marked as "tx finished" and "host owned" */
		Ctl_8 = read_slavemem8(adev, (u32) &(txdesc->Ctl_8));

		// OW FIXME Check against pci.c
		if ((Ctl_8 & DESC_CTL_ACXDONE_HOSTOWN) != DESC_CTL_ACXDONE_HOSTOWN) {
			//if (unlikely(!num_cleaned)) { /* maybe remove completely */
			log(L_BUFT, "acx: clean_txdesc: tail isn't free. "
				"finger=%d, tail=%d, head=%d\n", finger, adev->tx_tail,
					adev->tx_head);
			//}
			break;
		}

		/* remember desc values... */
		error = read_slavemem8(adev, (u32) &(txdesc->error));
		ack_failures = read_slavemem8(adev, (u32) &(txdesc->ack_failures));
		rts_failures = read_slavemem8(adev, (u32) &(txdesc->rts_failures));
		rts_ok = read_slavemem8(adev, (u32) &(txdesc->rts_ok));
		// OW FIXME does this also require le16_to_cpu()?
		r100 = read_slavemem8(adev, (u32) &(txdesc->u.r1.rate));
		r111 = le16_to_cpu(read_slavemem16 (adev, (u32) &(txdesc->u.r2.rate111)));

		/* need to check for certain error conditions before we
		 * clean the descriptor: we still need valid descr data here */

		hostdesc = acxmem_get_txhostdesc(adev, txdesc);
		txstatus = IEEE80211_SKB_CB(hostdesc->skb);

        if (!(txstatus->flags & IEEE80211_TX_CTL_NO_ACK) && !(error & 0x30))
			txstatus->flags |= IEEE80211_TX_STAT_ACK;

    	txstatus->status.rates[0].count = ack_failures + 1;

		/*
		 * Free up the transmit data buffers
		 */
		acxmem = read_slavemem32(adev, (u32) &(txdesc->AcxMemPtr));
		if (acxmem) {
			acxmem_reclaim_acx_txbuf_space(adev, acxmem);
		}

		/* ...and free the desc by clearing all the fields
		 except the next pointer */
		acxmem_copy_to_slavemem(adev, (u32) &(txdesc->HostMemPtr),
				(u8 *) &(tmptxdesc.HostMemPtr), sizeof(tmptxdesc)
						- sizeof(tmptxdesc.pNextDesc));

		adev->tx_free++;
		num_cleaned++;

		/* do error checking, rate handling and logging
		 * AFTER having done the work, it's faster */
		if (unlikely(error))
			acxpcimem_handle_tx_error(adev, error, finger, txstatus);

		if (IS_ACX111(adev)) {
			log(L_BUFT,
					"acx: tx: cleaned %u: !ACK=%u !RTS=%u RTS=%u r111=%04X tx_free=%u\n",
					finger, ack_failures, rts_failures, rts_ok, r111, adev->tx_free);
		} else {
			log(L_BUFT,
					"acx: tx: cleaned %u: !ACK=%u !RTS=%u RTS=%u rate=%u tx_free=%u\n",
					finger, ack_failures, rts_failures, rts_ok, r100, adev->tx_free);
		}

		/* And finally report upstream */
		ieee80211_tx_status_irqsafe(adev->ieee, hostdesc->skb);

		/* update pointer for descr to be cleaned next */
		finger = (finger + 1) % TX_CNT;
	}
	/* remember last position */
	adev->tx_tail = finger;

	FN_EXIT1(num_cleaned);
	return num_cleaned;
}

/* clean *all* Tx descriptors, and regardless of their previous state.
 * Used for brute-force reset handling. */
void acxmem_clean_txdesc_emergency(acx_device_t *adev) {
	txdesc_t *txdesc;
	int i;

	FN_ENTER;

	for (i = 0; i < TX_CNT; i++) {
		txdesc = acxmem_get_txdesc(adev, i);

		/* free it */
		write_slavemem8(adev, (u32) &(txdesc->ack_failures), 0);
		write_slavemem8(adev, (u32) &(txdesc->rts_failures), 0);
		write_slavemem8(adev, (u32) &(txdesc->rts_ok), 0);
		write_slavemem8(adev, (u32) &(txdesc->error), 0);
		write_slavemem8(adev, (u32) &(txdesc->Ctl_8), DESC_CTL_HOSTOWN);

#if 0
		u32 acxmem;
		/*
		 * Clean up the memory allocated on the ACX for this transmit descriptor.
		 */
		acxmem = read_slavemem32(adev, (u32) &(txdesc->AcxMemPtr));

		if (acxmem) {
			acxmem_reclaim_acx_txbuf_space(adev, acxmem);
		}
#endif

		write_slavemem32(adev, (u32) &(txdesc->AcxMemPtr), 0);
	}

	adev->tx_free = TX_CNT;

	acxmem_init_acx_txbuf2(adev);

	FN_EXIT0;
}

void acxmem_update_queue_indicator(acx_device_t *adev, int txqueue) {
#ifdef USING_MORE_THAN_ONE_TRANSMIT_QUEUE
	u32 indicator;
	unsigned long flags;
	int count;

	/*
	 * Can't handle an interrupt while we're fiddling with the ACX's lock,
	 * according to TI.  The ACX is supposed to hold fw_lock for at most
	 * 500ns.
	 */
	local_irq_save(flags);

	/*
	 * Wait for ACX to release the lock (at most 500ns).
	 */
	count = 0;
	while (read_slavemem16 (adev, (u32) &(adev->acx_queue_indicator->fw_lock))
			&& (count++ < 50)) {
		ndelay (10);
	}
	if (count < 50) {

		/*
		 * Take out the host lock - anything non-zero will work, so don't worry about
		 * be/le
		 */
		write_slavemem16 (adev, (u32) &(adev->acx_queue_indicator->host_lock), 1);

		/*
		 * Avoid a race condition
		 */
		count = 0;
		while (read_slavemem16 (adev, (u32) &(adev->acx_queue_indicator->fw_lock))
				&& (count++ < 50)) {
			ndelay (10);
		}

		if (count < 50) {
			/*
			 * Mark the queue active
			 */
			indicator = read_slavemem32 (adev, (u32) &(adev->acx_queue_indicator->indicator));
			indicator |= cpu_to_le32 (1 << txqueue);
			write_slavemem32 (adev, (u32) &(adev->acx_queue_indicator->indicator), indicator);
		}

		/*
		 * Release the host lock
		 */
		write_slavemem16 (adev, (u32) &(adev->acx_queue_indicator->host_lock), 0);

	}

	/*
	 * Restore interrupts
	 */
	local_irq_restore (flags);
#endif
}

int acx100mem_set_tx_level(acx_device_t *adev, u8 level_dbm) {
	struct acx111_ie_tx_level tx_level;

	/* since it can be assumed that at least the Maxim radio has a
	 * maximum power output of 20dBm and since it also can be
	 * assumed that these values drive the DAC responsible for
	 * setting the linear Tx level, I'd guess that these values
	 * should be the corresponding linear values for a dBm value,
	 * in other words: calculate the values from that formula:
	 * Y [dBm] = 10 * log (X [mW])
	 * then scale the 0..63 value range onto the 1..100mW range (0..20 dBm)
	 * and you're done...
	 * Hopefully that's ok, but you never know if we're actually
	 * right... (especially since Windows XP doesn't seem to show
	 * actual Tx dBm values :-P) */

	/* NOTE: on Maxim, value 30 IS 30mW, and value 10 IS 10mW - so the
	 * values are EXACTLY mW!!! Not sure about RFMD and others,
	 * though... */
	static const u8 dbm2val_maxim[21] = { 63, 63, 63, 62, 61, 61, 60, 60, 59,
			58, 57, 55, 53, 50, 47, 43, 38, 31, 23, 13, 0 };
	static const u8 dbm2val_rfmd[21] = { 0, 0, 0, 1, 2, 2, 3, 3, 4, 5, 6, 8,
			10, 13, 16, 20, 25, 32, 41, 50, 63 };
	const u8 *table;

	switch (adev->radio_type) {
	case RADIO_0D_MAXIM_MAX2820:
		table = &dbm2val_maxim[0];
		break;
	case RADIO_11_RFMD:
	case RADIO_15_RALINK:
		table = &dbm2val_rfmd[0];
		break;
	default:
		printk("acx: %s: unknown/unsupported radio type, "
			"cannot modify tx power level yet!\n", wiphy_name(adev->ieee->wiphy));
		return NOT_OK;
	}
	/*
	 * The hx4700 EEPROM, at least, only supports 1 power setting.  The configure
	 * routine matches the PA bias with the gain, so just use its default value.
	 * The values are: 0x2b for the gain and 0x03 for the PA bias.  The firmware
	 * writes the gain level to the Tx gain control DAC and the PA bias to the Maxim
	 * radio's PA bias register.  The firmware limits itself to 0 - 64 when writing to the
	 * gain control DAC.
	 *
	 * Physically between the ACX and the radio, higher Tx gain control DAC values result
	 * in less power output; 0 volts to the Maxim radio results in the highest output power
	 * level, which I'm assuming matches up with 0 in the Tx Gain DAC register.
	 *
	 * Although there is only the 1 power setting, one of the radio firmware functions adjusts
	 * the transmit power level up and down.  That function is called by the ACX FIQ handler
	 * under certain conditions.
	 */
	tx_level.level = 1;
	//return acx_s_configure(adev, &tx_level, ACX1xx_IE_DOT11_TX_POWER_LEVEL);

	printk("acx: %s: changing radio power level to %u dBm (%u)\n", wiphy_name(adev->ieee->wiphy),
			level_dbm, table[level_dbm]);
	acxmem_write_phy_reg(adev, 0x11, table[level_dbm]);

	return 0;
}

// OW TODO See if this is usable with mac80211
#if 0
/***********************************************************************
 ** acxmem_i_tx_timeout
 **
 ** Called from network core. Must not sleep!
 */
static void acxmem_i_tx_timeout(struct net_device *ndev) {
	acx_device_t *adev = ndev2adev(ndev);
	unsigned long flags;
	unsigned int tx_num_cleaned;

	FN_ENTER;

	acx_lock(adev, flags);

	/* clean processed tx descs, they may have been completely full */
	tx_num_cleaned = acxmem_clean_txdesc(adev);

	/* nothing cleaned, yet (almost) no free buffers available?
	 * --> clean all tx descs, no matter which status!!
	 * Note that I strongly suspect that doing emergency cleaning
	 * may confuse the firmware. This is a last ditch effort to get
	 * ANYTHING to work again...
	 *
	 * TODO: it's best to simply reset & reinit hw from scratch...
	 */
	if ((adev->tx_free <= TX_EMERG_CLEAN) && (tx_num_cleaned == 0)) {
		printk("%s: FAILED to free any of the many full tx buffers. "
			"Switching to emergency freeing. "
			"Please report!\n", ndev->name);
		acxmem_clean_txdesc_emergency(adev);
	}

	if (acx_queue_stopped(ndev) && (ACX_STATUS_4_ASSOCIATED == adev->status))
		acx_wake_queue(ndev, "after tx timeout");

	/* stall may have happened due to radio drift, so recalib radio */
	acx_schedule_task(adev, ACX_AFTER_IRQ_CMD_RADIO_RECALIB);

	/* do unimportant work last */
	printk("%s: tx timeout!\n", ndev->name);
	adev->stats.tx_errors++;

	acx_unlock(adev, flags);

	FN_EXIT0;
}
#endif


/*
 * BOM Irq Handling, Timer
 * ==================================================
 */

static void acxmem_irq_enable(acx_device_t *adev) {
	FN_ENTER;
	write_reg16(adev, IO_ACX_IRQ_MASK, adev->irq_mask);
	write_reg16(adev, IO_ACX_FEMR, 0x8000);
	adev->irqs_active = 1;
	FN_EXIT0;
}

static void acxmem_irq_disable(acx_device_t *adev) {
	FN_ENTER;

	write_reg16(adev, IO_ACX_IRQ_MASK, HOST_INT_MASK_ALL);
	write_reg16(adev, IO_ACX_FEMR, 0x0);
	adev->irqs_active = 0;
	FN_EXIT0;
}

/* Interrupt handler bottom-half */
// OW TODO Copy of pci: possible merging.
void acxmem_irq_work(struct work_struct *work)
{
	acx_device_t *adev = container_of(work, struct acx_device, irq_work);
	int irqreason;
	int irqmasked;
	acxmem_lock_flags;

	FN_ENTER;

	acx_sem_lock(adev);
	acxmem_lock();

	/* We only get an irq-signal for IO_ACX_IRQ_MASK unmasked irq reasons.
	 * However masked irq reasons we still read with IO_ACX_IRQ_REASON or
	 * IO_ACX_IRQ_STATUS_NON_DES
	 */
	irqreason = read_reg16(adev, IO_ACX_IRQ_REASON);
	irqmasked = irqreason & ~adev->irq_mask;
	log(L_IRQ, "acxpci: irqstatus=%04X, irqmasked==%04X\n", irqreason, irqmasked);

		/* HOST_INT_CMD_COMPLETE handling */
		if (irqmasked & HOST_INT_CMD_COMPLETE) {
			log(L_IRQ, "acxmem: got Command_Complete IRQ\n");
			/* save the state for the running issue_cmd() */
			SET_BIT(adev->irq_status, HOST_INT_CMD_COMPLETE);
		}

		/* Tx reporting */
		if (irqmasked & HOST_INT_TX_COMPLETE) {
			log(L_IRQ, "acxmem: got Tx_Complete IRQ\n");
				acxmem_clean_txdesc(adev);

				// Restart queue if stopped and enough tx-descr free
				if ((adev->tx_free >= TX_START_QUEUE) && acx_queue_stopped(adev->ieee)) {
					log(L_BUF, "acxmem: tx: wake queue (avail. Tx desc %u)\n",
							adev->tx_free);
					acx_wake_queue(adev->ieee, NULL);
					ieee80211_queue_work(adev->ieee, &adev->tx_work);
				}

		}

		/* Rx processing */
		if (irqmasked & HOST_INT_RX_DATA) {
			log(L_IRQ, "acxmem: got Rx_Complete IRQ\n");
			acxmem_process_rxdesc(adev);
		}

		/* HOST_INT_INFO */
		if (irqmasked & HOST_INT_INFO) {
			acxmem_handle_info_irq(adev);
		}

		/* HOST_INT_SCAN_COMPLETE */
		if (irqmasked & HOST_INT_SCAN_COMPLETE) {
			log(L_IRQ, "acxmem: got Scan_Complete IRQ\n");
			/* need to do that in process context */
			/* remember that fw is not scanning anymore */
			SET_BIT(adev->irq_status,
					HOST_INT_SCAN_COMPLETE);
		}

		/* These we just log, but either they happen rarely
		 * or we keep them masked out */
		if (acx_debug & L_IRQ)
		{
			acx_log_irq(irqreason);
		}

	/* Routine to perform blink with range
	 * FIXME: update_link_quality_led is a stub - add proper code and enable this again:
	if (unlikely(adev->led_power == 2))
		update_link_quality_led(adev);
	*/

	// Renable irq-signal again for irqs we are interested in
	write_reg16(adev, IO_ACX_IRQ_MASK, adev->irq_mask);
	write_flush(adev);

	acxmem_unlock();

	// after_interrupt_jobs: need to be done outside acx_lock (Sleeping required. None atomic)
	if (adev->after_interrupt_jobs){
		acx_e_after_interrupt_task(adev);
	}

	acx_sem_unlock(adev);

	FN_EXIT0;
	return;

}

// OW TODO Copy of pci: possible merging.
static irqreturn_t acxmem_i_interrupt(int irq, void *dev_id)
{
	acx_device_t *adev = dev_id;
	unsigned long flags;
	u16 irqreason;
	u16 irqmasked;

	if (!adev)
		return IRQ_NONE;

	// On a shared irq line, irqs should only be for us, when enabled them
	if (!adev->irqs_active)
		return IRQ_NONE;

	FN_ENTER;

	spin_lock_irqsave(&adev->spinlock, flags);

	/* We only get an irq-signal for IO_ACX_IRQ_MASK unmasked irq reasons.
	 * However masked irq reasons we still read with IO_ACX_IRQ_REASON or
	 * IO_ACX_IRQ_STATUS_NON_DES
	 */
	irqreason = read_reg16(adev, IO_ACX_IRQ_STATUS_NON_DES);
	irqmasked = irqreason & ~adev->irq_mask;
	log(L_IRQ, "acxmem: irqstatus=%04X, irqmasked=%04X,\n", irqreason, irqmasked);

	if (unlikely(irqreason == 0xffff)) {
		/* 0xffff value hints at missing hardware,
		 * so don't do anything.
		 * Not very clean, but other drivers do the same... */
		log(L_IRQ, "acxmem: irqstatus=FFFF: Device removed?: IRQ_NONE\n");
		goto none;
	}

	/* Our irq-signals are the ones, that were triggered by the IO_ACX_IRQ_MASK
	 * unmasked irqs.
	 */
	if (!irqmasked) {
		/* We are on a shared IRQ line and it wasn't our IRQ */
		log(L_IRQ, "acxmem: irqmasked zero: IRQ_NONE\n");
		goto none;
	}

	// Mask all irqs, until we handle them. We will unmask them later in the tasklet.
	write_reg16(adev, IO_ACX_IRQ_MASK, HOST_INT_MASK_ALL);
	write_flush(adev);
	acx_schedule_task(adev, 0);

	spin_unlock_irqrestore(&adev->spinlock, flags);
	FN_EXIT0;
	return IRQ_HANDLED;

	none:

	spin_unlock_irqrestore(&adev->spinlock, flags);
	FN_EXIT0;
	return IRQ_NONE;

}

/*
 * acxmem_handle_info_irq
 */

/* scan is complete. all frames now on the receive queue are valid */
#define INFO_SCAN_COMPLETE      0x0001
#define INFO_WEP_KEY_NOT_FOUND  0x0002
/* hw has been reset as the result of a watchdog timer timeout */
#define INFO_WATCH_DOG_RESET    0x0003
/* failed to send out NULL frame from PS mode notification to AP */
/* recommended action: try entering 802.11 PS mode again */
#define INFO_PS_FAIL            0x0004
/* encryption/decryption process on a packet failed */
#define INFO_IV_ICV_FAILURE     0x0005

/* Info mailbox format:
 2 bytes: type
 2 bytes: status
 more bytes may follow
 rumors say about status:
 0x0000 info available (set by hw)
 0x0001 information received (must be set by host)
 0x1000 info available, mailbox overflowed (messages lost) (set by hw)
 but in practice we've seen:
 0x9000 when we did not set status to 0x0001 on prev message
 0x1001 when we did set it
 0x0000 was never seen
 conclusion: this is really a bitfield:
 0x1000 is 'info available' bit
 'mailbox overflowed' bit is 0x8000, not 0x1000
 value of 0x0000 probably means that there are no messages at all
 P.S. I dunno how in hell hw is supposed to notice that messages are lost -
 it does NOT clear bit 0x0001, and this bit will probably stay forever set
 after we set it once. Let's hope this will be fixed in firmware someday
 */

static void acxmem_handle_info_irq(acx_device_t *adev) {
#if ACX_DEBUG
	static const char * const info_type_msg[] = {
			"(unknown)",
			"scan complete",
			"WEP key not found",
			"internal watchdog reset was done",
			"failed to send powersave (NULL frame) notification to AP",
			"encrypt/decrypt on a packet has failed",
			"TKIP tx keys disabled",
			"TKIP rx keys disabled",
			"TKIP rx: key ID not found",
			"???",
			"???",
			"???",
			"???",
			"???",
			"???",
			"???",
			"TKIP IV value exceeds thresh"
			};
#endif
	u32 info_type, info_status;

	info_type = read_slavemem32(adev, (u32) adev->info_area);

	info_status = (info_type >> 16);
	info_type = (u16) info_type;

	/* inform fw that we have read this info message */
	write_slavemem32(adev, (u32) adev->info_area, info_type | 0x00010000);
	write_reg16(adev, IO_ACX_INT_TRIG, INT_TRIG_INFOACK);
	write_flush(adev);

	log(L_IRQ|L_CTL, "acx: got Info IRQ: status %04X type %04X: %s\n",
			info_status, info_type,
			info_type_msg[(info_type >= ARRAY_SIZE(info_type_msg)) ?
			0 : info_type]
	);
}

void acxmem_set_interrupt_mask(acx_device_t *adev) {

	FN_ENTER;

	if (IS_ACX111(adev)) {
		adev->irq_mask = (u16) ~(0
		| HOST_INT_RX_DATA
		| HOST_INT_TX_COMPLETE
		/* | HOST_INT_TX_XFER        */
		/* | HOST_INT_RX_COMPLETE    */
		/* | HOST_INT_DTIM           */
		/* | HOST_INT_BEACON         */
		/* | HOST_INT_TIMER          */
		/* | HOST_INT_KEY_NOT_FOUND  */
		| HOST_INT_IV_ICV_FAILURE
		| HOST_INT_CMD_COMPLETE
		| HOST_INT_INFO
		| HOST_INT_OVERFLOW
		/* | HOST_INT_PROCESS_ERROR  */
		| HOST_INT_SCAN_COMPLETE
		| HOST_INT_FCS_THRESHOLD
		| HOST_INT_UNKNOWN);

	} else {
		adev->irq_mask = (u16) ~(0
		| HOST_INT_RX_DATA
		| HOST_INT_TX_COMPLETE
		/* | HOST_INT_TX_XFER        */
		/* | HOST_INT_RX_COMPLETE    */
		/* | HOST_INT_DTIM           */
		/* | HOST_INT_BEACON         */
		/* | HOST_INT_TIMER          */
		/* | HOST_INT_KEY_NOT_FOUND  */
		/* | HOST_INT_IV_ICV_FAILURE */
		| HOST_INT_CMD_COMPLETE
		| HOST_INT_INFO
		| HOST_INT_OVERFLOW
		| HOST_INT_PROCESS_ERROR
		| HOST_INT_SCAN_COMPLETE
		/* | HOST_INT_FCS_THRESHOLD  */
		/* | HOST_INT_BEACON_MISSED        */
		);

	}

	FN_EXIT0;

}

// OW FIXME Old interrupt handler
// ---
#if 0
static irqreturn_t acxmem_i_interrupt(int irq, void *dev_id)
{
	acx_device_t *adev = dev_id;
	unsigned long flags;
	unsigned int irqcount = MAX_IRQLOOPS_PER_JIFFY;
	register u16 irqtype;
	u16 unmasked;

	FN_ENTER;

	if (!adev)
		return IRQ_NONE;

	/* LOCKING: can just spin_lock() since IRQs are disabled anyway.
	 * I am paranoid */
	acx_lock(adev, flags);

	unmasked = read_reg16(adev, IO_ACX_IRQ_REASON);
	if (unlikely(0xffff == unmasked)) {
		/* 0xffff value hints at missing hardware,
		 * so don't do anything.
		 * Not very clean, but other drivers do the same... */
		log(L_IRQ, "IRQ type:FFFF - device removed? IRQ_NONE\n");
		goto none;
	}

	/* We will check only "interesting" IRQ types */
	irqtype = unmasked & ~adev->irq_mask;
	if (!irqtype) {
		/* We are on a shared IRQ line and it wasn't our IRQ */
		log(L_IRQ, "IRQ type:%04X, mask:%04X - all are masked, IRQ_NONE\n",
				unmasked, adev->irq_mask);
		goto none;
	}

	/* Done here because IRQ_NONEs taking three lines of log
	 ** drive me crazy */
	FN_ENTER;

#define IRQ_ITERATE 1
#if IRQ_ITERATE
	if (jiffies != adev->irq_last_jiffies) {
		adev->irq_loops_this_jiffy = 0;
		adev->irq_last_jiffies = jiffies;
	}

	/* safety condition; we'll normally abort loop below
	 * in case no IRQ type occurred */
	while (likely(--irqcount)) {
#endif
		/* ACK all IRQs ASAP */
		write_reg16(adev, IO_ACX_IRQ_ACK, 0xffff);

		log(L_IRQ, "IRQ type:%04X, mask:%04X, type & ~mask:%04X\n",
				unmasked, adev->irq_mask, irqtype);

		/* Handle most important IRQ types first */

		// OW 20091123 FIXME Rx path stops under load problem:
		// Maybe the RX rings fills up to fast, we are missing an irq and
		// then we are then not getting rx irqs anymore
		if (irqtype & HOST_INT_RX_DATA) {
			log(L_IRQ, "got Rx_Data IRQ\n");
			acxmem_process_rxdesc(adev);
		}

		if (irqtype & HOST_INT_TX_COMPLETE) {
			log(L_IRQ, "got Tx_Complete IRQ\n");
			/* don't clean up on each Tx complete, wait a bit
			 * unless we're going towards full, in which case
			 * we do it immediately, too (otherwise we might lockup
			 * with a full Tx buffer if we go into
			 * acxmem_l_clean_txdesc() at a time when we won't wakeup
			 * the net queue in there for some reason...) */
			if (adev->tx_free <= TX_START_CLEAN) {
#if TX_CLEANUP_IN_SOFTIRQ
				acx_schedule_task(adev, ACX_AFTER_IRQ_TX_CLEANUP);
#else
				acxmem_clean_txdesc(adev);
#endif
			}
		}

		/* Less frequent ones */
		if (irqtype & (0 | HOST_INT_CMD_COMPLETE | HOST_INT_INFO
				| HOST_INT_SCAN_COMPLETE)) {
			if (irqtype & HOST_INT_CMD_COMPLETE) {
				log(L_IRQ, "got Command_Complete IRQ\n");
				/* save the state for the running issue_cmd() */
				SET_BIT(adev->irq_status, HOST_INT_CMD_COMPLETE);
			}
			if (irqtype & HOST_INT_INFO) {
				acxmem_handle_info_irq(adev);
			}
			if (irqtype & HOST_INT_SCAN_COMPLETE) {
				log(L_IRQ, "got Scan_Complete IRQ\n");
				/* need to do that in process context */
				acx_schedule_task(adev, ACX_AFTER_IRQ_COMPLETE_SCAN);
				/* remember that fw is not scanning anymore */
				SET_BIT(adev->irq_status, HOST_INT_SCAN_COMPLETE);
			}
		}

		/* These we just log, but either they happen rarely
		 * or we keep them masked out */
		if (irqtype & (0
		/* | HOST_INT_RX_DATA */
		/* | HOST_INT_TX_COMPLETE   */
		| HOST_INT_TX_XFER
		| HOST_INT_RX_COMPLETE
		| HOST_INT_DTIM
		| HOST_INT_BEACON
		| HOST_INT_TIMER
		| HOST_INT_KEY_NOT_FOUND
		| HOST_INT_IV_ICV_FAILURE
		/* | HOST_INT_CMD_COMPLETE  */
		/* | HOST_INT_INFO          */
		| HOST_INT_OVERFLOW
		| HOST_INT_PROCESS_ERROR
		/* | HOST_INT_SCAN_COMPLETE */
		| HOST_INT_FCS_THRESHOLD
		| HOST_INT_UNKNOWN))
		{
			acxmem_log_unusual_irq(irqtype);
		}

#if IRQ_ITERATE
		unmasked = read_reg16(adev, IO_ACX_IRQ_REASON);
		irqtype = unmasked & ~adev->irq_mask;
		/* Bail out if no new IRQ bits or if all are masked out */
		if (!irqtype)
			break;

		if (unlikely(++adev->irq_loops_this_jiffy> MAX_IRQLOOPS_PER_JIFFY)) {
			printk(KERN_ERR "acx: too many interrupts per jiffy!\n");
			/* Looks like card floods us with IRQs! Try to stop that */
			write_reg16(adev, IO_ACX_IRQ_MASK, 0xffff);
			/* This will short-circuit all future attempts to handle IRQ.
			 * We cant do much more... */
			adev->irq_mask = 0;
			break;
		}
	}
#endif

	// OW 20091129 TODO Currently breaks mem.c ...
	// If sleeping is required like for update card settings, this is usefull
	// For now I replaced sleeping for command handling by mdelays.
//	if (adev->after_interrupt_jobs){
//		acx_e_after_interrupt_task(adev);
//	}


// OW TODO
#if 0
	/* Routine to perform blink with range */
	if (unlikely(adev->led_power == 2))
		update_link_quality_led(adev);
#endif

	/* handled: */
	/* write_flush(adev); - not needed, last op was read anyway */
	acx_unlock(adev, flags);
	FN_EXIT0;
	return IRQ_HANDLED;

	none:
	acx_unlock(adev, flags);
	FN_EXIT0;
	return IRQ_NONE;
}
#endif
// ---


/*
 * BOM Mac80211 Ops
 * ==================================================
 */

static const struct ieee80211_ops acxmem_hw_ops = {
		.tx = acx_op_tx,
		.conf_tx = acx_e_conf_tx,
		.add_interface = acx_e_op_add_interface,
		.remove_interface = acx_e_op_remove_interface,
		.start = acxmem_e_op_start,
		.configure_filter = acx_i_op_configure_filter,
		.stop = acxmem_e_op_stop,
		.config = acx_e_op_config,
		.bss_info_changed = acx_e_op_bss_info_changed,
		.set_key = acx_e_op_set_key,
		.get_stats = acx_e_op_get_stats,
#if CONFIG_ACX_MAC80211_VERSION < KERNEL_VERSION(2, 6, 34)
		.get_tx_stats = acx_e_op_get_tx_stats,
#endif
		};

static int acxmem_e_op_start(struct ieee80211_hw *hw) {
	acx_device_t *adev = ieee2adev(hw);
	int result = OK;

	FN_ENTER;
	acx_sem_lock(adev);

	adev->initialized = 0;

	/* TODO: pci_set_power_state(pdev, PCI_D0); ? */

	/* ifup device */
	acxmem_up(hw);

	/* We don't currently have to do anything else.
	 * The setup of the MAC should be subsequently completed via
	 * the mlme commands.
	 * Higher layers know we're ready from dev->start==1 and
	 * dev->tbusy==0.  Our rx path knows to pass up received/
	 * frames because of dev->flags&IFF_UP is true.
	 */

	ieee80211_wake_queues(adev->ieee);

	adev->initialized = 1;

	acx_sem_unlock(adev);
	FN_EXIT1(result);

	return result;
}


static void acxmem_e_op_stop(struct ieee80211_hw *hw)
{
	acx_device_t *adev = ieee2adev(hw);
	acxmem_lock_flags;

	FN_ENTER;
	acx_sem_lock(adev);

	acx_stop_queue(adev->ieee, "on ifdown");

	/* disable all IRQs, release shared IRQ handler */
	acxmem_lock();
	acxmem_irq_disable(adev);
	acxmem_unlock();
	synchronize_irq(adev->irq);

	acx_sem_unlock(adev);
	cancel_work_sync(&adev->irq_work);
	cancel_work_sync(&adev->tx_work);
	acx_sem_lock(adev);

	acx_tx_queue_flush(adev);

	del_timer_sync(&adev->mgmt_timer);

	CLEAR_BIT(adev->dev_state_mask, ACX_STATE_IFACE_UP);

	/* TODO: pci_set_power_state(pdev, PCI_D3hot); ? */

	adev->initialized = 0;

	log(L_INIT, "acxpci: closed device\n");

	acx_sem_unlock(adev);
	FN_EXIT0;
}

/*
 * BOM Helpers
 * ==================================================
 */

void acxmem_l_power_led(acx_device_t *adev, int enable) {
	u16 gpio_pled = IS_ACX111(adev) ? 0x0040 : 0x0800;

	/* A hack. Not moving message rate limiting to adev->xxx
	 * (it's only a debug message after all) */
	static int rate_limit = 0;

	if (rate_limit++ < 3)
		log(L_IOCTL, "acx: Please report in case toggling the power "
				"LED doesn't work for your card!\n");
	if (enable)
		write_reg16(adev, IO_ACX_GPIO_OUT, read_reg16(adev, IO_ACX_GPIO_OUT)
				& ~gpio_pled);
	else
		write_reg16(adev, IO_ACX_GPIO_OUT, read_reg16(adev, IO_ACX_GPIO_OUT)
				| gpio_pled);
}

INLINE_IO int acxmem_adev_present(acx_device_t *adev) {
	/* fast version (accesses the first register, IO_ACX_SOFT_RESET,
	 * which should be safe): */
	return acx_readl(adev->iobase) != 0xffffffff;
}

static char acxmem_printable(char c) {
	return ((c >= 20) && (c < 127)) ? c : '.';
}

// OW TODO
#if 0
static void update_link_quality_led(acx_device_t *adev) {
	int qual;

	qual = acx_signal_determine_quality(adev->wstats.qual.level,
			adev->wstats.qual.noise);
	if (qual > adev->brange_max_quality)
		qual = adev->brange_max_quality;

	if (time_after(jiffies, adev->brange_time_last_state_change +
			(HZ/2 - HZ/2 * (unsigned long)qual / adev->brange_max_quality ) )) {
		acxmem_l_power_led(adev, (adev->brange_last_state == 0));
		adev->brange_last_state ^= 1; /* toggle */
		adev->brange_time_last_state_change = jiffies;
	}
}
#endif


/*
 * BOM Ioctls
 * ==================================================
 */

// OW TODO Not used in pci either !?
#if 0
int acx111pci_ioctl_info(struct ieee80211_hw *hw, struct iw_request_info *info,
		struct iw_param *vwrq, char *extra) {
#if ACX_DEBUG > 1

	acx_device_t *adev = ieee2adev(hw);
	rxdesc_t *rxdesc;
	txdesc_t *txdesc;
	rxhostdesc_t *rxhostdesc;
	txhostdesc_t *txhostdesc;
	struct acx111_ie_memoryconfig memconf;
	struct acx111_ie_queueconfig queueconf;
	unsigned long flags;
	int i;
	char memmap[0x34];
	char rxconfig[0x8];
	char fcserror[0x8];
	char ratefallback[0x5];

	if (!(acx_debug & (L_IOCTL | L_DEBUG)))
		return OK;
	/* using printk() since we checked debug flag already */

	acx_sem_lock(adev);

	if (!IS_ACX111(adev)) {
		printk("acx: acx111-specific function called "
			"with non-acx111 chip, aborting\n");
		goto end_ok;
	}

	/* get Acx111 Memory Configuration */
	memset(&memconf, 0, sizeof(memconf));
	/* BTW, fails with 12 (Write only) error code.
	 ** Retained for easy testing of issue_cmd error handling :) */
	printk("Interrogating queue config\n");
	acx_s_interrogate(adev, &memconf, ACX1xx_IE_QUEUE_CONFIG);
	printk("done with queue config\n");

	/* get Acx111 Queue Configuration */
	memset(&queueconf, 0, sizeof(queueconf));
	printk("Interrogating mem config options\n");
	acx_s_interrogate(adev, &queueconf, ACX1xx_IE_MEMORY_CONFIG_OPTIONS);
	printk("done with mem config options\n");

	/* get Acx111 Memory Map */
	memset(memmap, 0, sizeof(memmap));
	printk("Interrogating mem map\n");
	acx_s_interrogate(adev, &memmap, ACX1xx_IE_MEMORY_MAP);
	printk("done with mem map\n");

	/* get Acx111 Rx Config */
	memset(rxconfig, 0, sizeof(rxconfig));
	printk("Interrogating rxconfig\n");
	acx_s_interrogate(adev, &rxconfig, ACX1xx_IE_RXCONFIG);
	printk("done with queue rxconfig\n");

	/* get Acx111 fcs error count */
	memset(fcserror, 0, sizeof(fcserror));
	printk("Interrogating fcs err count\n");
	acx_s_interrogate(adev, &fcserror, ACX1xx_IE_FCS_ERROR_COUNT);
	printk("done with err count\n");

	/* get Acx111 rate fallback */
	memset(ratefallback, 0, sizeof(ratefallback));
	printk("Interrogating rate fallback\n");
	acx_s_interrogate(adev, &ratefallback, ACX1xx_IE_RATE_FALLBACK);
	printk("done with rate fallback\n");

	/* force occurrence of a beacon interrupt */
	/* TODO: comment why is this necessary */
	write_reg16(adev, IO_ACX_HINT_TRIG, HOST_INT_BEACON);

	/* dump Acx111 Mem Configuration */
	printk("acx: dump mem config:\n"
		"data read: %d, struct size: %d\n"
		"Number of stations: %1X\n"
		"Memory block size: %1X\n"
		"tx/rx memory block allocation: %1X\n"
		"count rx: %X / tx: %X queues\n"
		"options %1X\n"
		"fragmentation %1X\n"
		"Rx Queue 1 Count Descriptors: %X\n"
		"Rx Queue 1 Host Memory Start: %X\n"
		"Tx Queue 1 Count Descriptors: %X\n"
		"Tx Queue 1 Attributes: %X\n",
		  memconf.len, (int) sizeof(memconf),
			memconf.no_of_stations,
			memconf.memory_block_size,
			memconf.tx_rx_memory_block_allocation,
			memconf.count_rx_queues, memconf.count_tx_queues,
			memconf.options,
			memconf.fragmentation,
			memconf.rx_queue1_count_descs,
			acx2cpu(memconf.rx_queue1_host_rx_start),
			memconf.tx_queue1_count_descs, memconf.tx_queue1_attributes);

	/* dump Acx111 Queue Configuration */
	printk("acx: dump queue head:\n"
		"data read: %d, struct size: %d\n"
		"tx_memory_block_address (from card): %X\n"
		"rx_memory_block_address (from card): %X\n"
		"rx1_queue address (from card): %X\n"
		"tx1_queue address (from card): %X\n"
		"tx1_queue attributes (from card): %X\n",
			queueconf.len, (int) sizeof(queueconf),
			queueconf.tx_memory_block_address,
			queueconf.rx_memory_block_address,
			queueconf.rx1_queue_address,
			queueconf.tx1_queue_address, queueconf.tx1_attributes);

	/* dump Acx111 Mem Map */
	printk("acx: dump mem map:\n"
		"data read: %d, struct size: %d\n"
		"Code start: %X\n"
		"Code end: %X\n"
		"WEP default key start: %X\n"
		"WEP default key end: %X\n"
		"STA table start: %X\n"
		"STA table end: %X\n"
		"Packet template start: %X\n"
		"Packet template end: %X\n"
		"Queue memory start: %X\n"
		"Queue memory end: %X\n"
		"Packet memory pool start: %X\n"
		"Packet memory pool end: %X\n"
		"iobase: %p\n"
		"iobase2: %p\n",
			*((u16 *) &memmap[0x02]), (int) sizeof(memmap),
			*((u32 *) &memmap[0x04]),
			*((u32 *) &memmap[0x08]),
			*((u32 *) &memmap[0x0C]),
			*((u32 *) &memmap[0x10]),
			*((u32 *) &memmap[0x14]),
			*((u32 *) &memmap[0x18]),
			*((u32 *) &memmap[0x1C]),
			*((u32 *) &memmap[0x20]),
			*((u32 *) &memmap[0x24]),
			*((u32 *) &memmap[0x28]),
			*((u32 *) &memmap[0x2C]),
			*((u32 *) &memmap[0x30]), adev->iobase,
			adev->iobase2);

	/* dump Acx111 Rx Config */
	printk("acx: dump rx config:\n"
		"data read: %d, struct size: %d\n"
		"rx config: %X\n"
		"rx filter config: %X\n",
			*((u16 *) &rxconfig[0x02]),	(int) sizeof(rxconfig),
			*((u16 *) &rxconfig[0x04]),	*((u16 *) &rxconfig[0x06]));

	/* dump Acx111 fcs error */
	printk("acx: dump fcserror:\n"
		"data read: %d, struct size: %d\n"
		"fcserrors: %X\n",
			*((u16 *) &fcserror[0x02]), (int) sizeof(fcserror),
			*((u32 *) &fcserror[0x04]));

	/* dump Acx111 rate fallback */
	printk("acx: dump rate fallback:\n"
		"data read: %d, struct size: %d\n"
		"ratefallback: %X\n",
			*((u16 *) &ratefallback[0x02]),
			(int) sizeof(ratefallback),
			*((u8 *) &ratefallback[0x04]));

	/* protect against IRQ */
	acx_lock(adev, flags);

	/* dump acx111 internal rx descriptor ring buffer */
	rxdesc = adev->rxdesc_start;

	/* loop over complete receive pool */
	if (rxdesc)
		for (i = 0; i < RX_CNT; i++) {
			printk("acx: \ndump internal rxdesc %d:\n"
				"mem pos %p\n"
				"next 0x%X\n"
				"acx mem pointer (dynamic) 0x%X\n"
				"CTL (dynamic) 0x%X\n"
				"Rate (dynamic) 0x%X\n"
				"RxStatus (dynamic) 0x%X\n"
				"Mod/Pre (dynamic) 0x%X\n",
				i,
				rxdesc,
				acx2cpu(rxdesc->pNextDesc),
				acx2cpu(rxdesc->ACXMemPtr),
				rxdesc->Ctl_8, rxdesc->rate,	rxdesc->error, rxdesc->SNR);
			rxdesc++;
		}

		/* dump host rx descriptor ring buffer */

		rxhostdesc = adev->rxhostdesc_start;

		/* loop over complete receive pool */
		if (rxhostdesc)
		for (i = 0; i < RX_CNT; i++) {
			printk("acx: \ndump host rxdesc %d:\n"
					"mem pos %p\n"
					"buffer mem pos 0x%X\n"
					"buffer mem offset 0x%X\n"
					"CTL 0x%X\n"
					"Length 0x%X\n"
					"next 0x%X\n"
					"Status 0x%X\n",
					i,
					rxhostdesc,
					acx2cpu(rxhostdesc->data_phy),
					rxhostdesc->data_offset,
					le16_to_cpu(rxhostdesc->Ctl_16),
					le16_to_cpu(rxhostdesc->length),
					acx2cpu(rxhostdesc->desc_phy_next),
					rxhostdesc->Status);
			rxhostdesc++;
		}

		/* dump acx111 internal tx descriptor ring buffer */
		txdesc = adev->txdesc_start;

		/* loop over complete transmit pool */
		if (txdesc)
		for (i = 0; i < TX_CNT; i++) {
			printk("acx: \ndump internal txdesc %d:\n"
					"size 0x%X\n"
					"mem pos %p\n"
					"next 0x%X\n"
					"acx mem pointer (dynamic) 0x%X\n"
					"host mem pointer (dynamic) 0x%X\n"
					"length (dynamic) 0x%X\n"
					"CTL (dynamic) 0x%X\n"
					"CTL2 (dynamic) 0x%X\n"
					"Status (dynamic) 0x%X\n"
					"Rate (dynamic) 0x%X\n",
					i,
					(int) sizeof(struct txdesc),
					txdesc,
					acx2cpu(txdesc->pNextDesc),
					acx2cpu(txdesc->AcxMemPtr),
					acx2cpu(txdesc->HostMemPtr),
					le16_to_cpu(txdesc->total_length),
					txdesc->Ctl_8,
					txdesc->Ctl2_8,
					txdesc->error,
					txdesc->u.r1.rate);
			txdesc = acxmem_advance_txdesc(adev, txdesc, 1);
		}

		/* dump host tx descriptor ring buffer */

		txhostdesc = adev->txhostdesc_start;

		/* loop over complete host send pool */
		if (txhostdesc)
		for (i = 0; i < TX_CNT * 2; i++) {
			printk("acx: \ndump host txdesc %d:\n"
					"mem pos %p\n"
					"buffer mem pos 0x%X\n"
					"buffer mem offset 0x%X\n"
					"CTL 0x%X\n"
					"Length 0x%X\n"
					"next 0x%X\n"
					"Status 0x%X\n",
					i,
					txhostdesc,
					acx2cpu(txhostdesc->data_phy),
					txhostdesc->data_offset,
					le16_to_cpu(txhostdesc->Ctl_16),
					le16_to_cpu(txhostdesc->length),
					acx2cpu(txhostdesc->desc_phy_next),
					le32_to_cpu(txhostdesc->Status));
			txhostdesc++;
		}

		/* write_reg16(adev, 0xb4, 0x4); */

		acx_unlock(adev, flags);
		end_ok:

		acx_sem_unlock(adev);
#endif /* ACX_DEBUG */
	return OK;
}

/***********************************************************************
 */
int acx100mem_ioctl_set_phy_amp_bias(struct ieee80211_hw *hw,
		struct iw_request_info *info,
		struct iw_param *vwrq, char *extra) {
	// OW
	acx_device_t *adev = ieee2adev(hw);
	unsigned long flags;
	u16 gpio_old;

	if (!IS_ACX100(adev)) {
		/* WARNING!!!
		 * Removing this check *might* damage
		 * hardware, since we're tweaking GPIOs here after all!!!
		 * You've been warned...
		 * WARNING!!! */
		printk("acx: sorry, setting bias level for non-acx100 "
			"is not supported yet\n");
		return OK;
	}

	if (*extra > 7) {
		printk("acx: invalid bias parameter, range is 0-7\n");
		return -EINVAL;
	}

	acx_sem_lock(adev);

	/* Need to lock accesses to [IO_ACX_GPIO_OUT]:
	 * IRQ handler uses it to update LED */
	acx_lock(adev, flags);
	gpio_old = read_reg16(adev, IO_ACX_GPIO_OUT);
	write_reg16(adev, IO_ACX_GPIO_OUT,
			(gpio_old & 0xf8ff)	| ((u16) *extra << 8));
	acx_unlock(adev, flags);

	log(L_DEBUG, "acx: gpio_old: 0x%04X\n", gpio_old);
	printk("acx: %s: PHY power amplifier bias: old:%d, new:%d\n",
			wiphy_name(adev->ieee->wiphy), (gpio_old & 0x0700) >> 8, (unsigned char) *extra);

	acx_sem_unlock(adev);

	return OK;
}
#endif


/*
 * BOM Driver, Module
 * ==================================================
 */

/*
 * acxmem_e_probe
 *
 * Probe routine called when a PCI device w/ matching ID is found.
 * Here's the sequence:
 *   - Allocate the PCI resources.
 *   - Read the PCMCIA attribute memory to make sure we have a WLAN card
 *   - Reset the MAC
 *   - Initialize the dev and wlan data
 *   - Initialize the MAC
 *
 * pdev	- ptr to pci device structure containing info about pci configuration
 * id	- ptr to the device id entry that matched this device
 */
static int __devinit acxmem_e_probe(struct platform_device *pdev) {

	acx_device_t *adev = NULL;
	const char *chip_name;
	int result = -EIO;
	int err;
	int i;

	struct resource *iomem;
	unsigned long addr_size = 0;
	u8 chip_type;

	acxmem_lock_flags;

	struct ieee80211_hw *ieee;

	FN_ENTER;

	ieee = ieee80211_alloc_hw(sizeof(struct acx_device), &acxmem_hw_ops);
	if (!ieee) {
		printk("acx: could not allocate ieee80211 structure %s\n", pdev->name);
		goto fail_ieee80211_alloc_hw;
	}
	SET_IEEE80211_DEV(ieee, &pdev->dev);
	ieee->flags &= ~IEEE80211_HW_RX_INCLUDES_FCS;
	/* TODO: mainline doesn't support the following flags yet */
	/*
	 ~IEEE80211_HW_MONITOR_DURING_OPER &
	 ~IEEE80211_HW_WEP_INCLUDE_IV;
	 */
	ieee->wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION)
					| BIT(NL80211_IFTYPE_ADHOC);
	ieee->queues = 1;
	// OW TODO Check if RTS/CTS threshold can be included here

	/* TODO: although in the original driver the maximum value was 100,
	 * the OpenBSD driver assigns maximum values depending on the type of
	 * radio transceiver (i.e. Radia, Maxim, etc.). This value is always a
	 * positive integer which most probably indicates the gain of the AGC
	 * in the rx path of the chip, in dB steps (0.625 dB, for example?).
	 * The mapping of this rssi value to dBm is still unknown, but it can
	 * nevertheless be used as a measure of relative signal strength. The
	 * other two values, i.e. max_signal and max_noise, do not seem to be
	 * supported on my acx111 card (they are always 0), although iwconfig
	 * reports them (in dBm) when using ndiswrapper with the Windows XP
	 * driver. The GPL-licensed part of the AVM FRITZ!WLAN USB Stick
	 * driver sources (for the TNETW1450, though) seems to also indicate
	 * that only the RSSI is supported. In conclusion, the max_signal and
	 * max_noise values will not be initialised by now, as they do not
	 * seem to be supported or how to acquire them is still unknown. */

	// We base signal quality on winlevel approach of previous driver
	// TODO OW 20100615 This should into a common init code
	ieee->flags |= IEEE80211_HW_SIGNAL_UNSPEC;
	ieee->max_signal = 100;

	adev = ieee2adev(ieee);

	memset(adev, 0, sizeof(*adev));
	/** Set up our private interface **/
	spin_lock_init(&adev->spinlock); /* initial state: unlocked */
	/* We do not start with downed sem: we want PARANOID_LOCKING to work */
	mutex_init(&adev->mutex);
	/* since nobody can see new netdev yet, we can as well
	 ** just _presume_ that we're under sem (instead of actually taking it): */
	/* acx_sem_lock(adev); */
	adev->ieee = ieee;
	adev->pdev = pdev;
	adev->bus_dev = &pdev->dev;
	adev->dev_type = DEVTYPE_MEM;

	/** Finished with private interface **/


	/** begin board specific inits **/
	platform_set_drvdata(pdev, ieee);

	/* chiptype is u8 but id->driver_data is ulong
	 ** Works for now (possible values are 1 and 2) */
	chip_type = CHIPTYPE_ACX100;
	/* acx100 and acx111 have different PCI memory regions */
	if (chip_type == CHIPTYPE_ACX100) {
		chip_name = "ACX100";
	} else if (chip_type == CHIPTYPE_ACX111) {
		chip_name = "ACX111";
	} else {
		printk("acx: unknown chip type 0x%04X\n", chip_type);
		goto fail_unknown_chiptype;
	}

	printk("acx: found %s-based wireless network card\n", chip_name);
	log(L_ANY, "initial debug setting is 0x%04X\n", acx_debug);

	adev->dev_type = DEVTYPE_MEM;
	adev->chip_type = chip_type;
	adev->chip_name = chip_name;
	adev->io = (CHIPTYPE_ACX100 == chip_type) ? IO_ACX100 : IO_ACX111;

	iomem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	addr_size = iomem->end - iomem->start + 1;
	adev->membase = (volatile u32 *) iomem->start;
	adev->iobase = (volatile u32 *) ioremap_nocache(iomem->start, addr_size);
	if (!adev->iobase) {
		result = -ENOMEM;
		dev_err(adev->bus_dev, "Couldn't ioremap\n");
		goto fail_ioremap;
	}

	i = platform_get_irq(pdev, 0);
	if (i < 0)
		return i;
	adev->irq = i;

	log(L_ANY, "acx: found an %s-based wireless network card, "
			"irq:%d, "
			"membase:0x%p, mem_size:%ld, "
			"iobase:0x%p",
			chip_name,
			adev->irq,
			adev->membase, addr_size,
			adev->iobase);
	log(L_ANY, "acx: the initial debug setting is 0x%04X\n", acx_debug);

	if (adev->irq == 0) {
		printk("acx: can't use IRQ 0\n");
		goto fail_request_irq;
	}

	log(L_IRQ | L_INIT, "acx: using IRQ %d\n", adev->irq);
	/* request shared IRQ handler */
	if (request_irq(adev->irq, acxmem_i_interrupt,
			IRQF_SHARED,
			KBUILD_MODNAME,
			adev)) {
		printk("acx: %s: request_irq FAILED\n", wiphy_name(adev->ieee->wiphy));
		result = -EAGAIN;
		goto fail_request_irq;
	}
	set_irq_type(adev->irq, IRQF_TRIGGER_FALLING);
	log(L_ANY, "acx: request_irq %d successful\n", adev->irq);
	// Acx irqs shall be off and are enabled later in acxpci_s_up
	acxmem_lock();
	acxmem_irq_disable(adev);
	acxmem_unlock();

	/* to find crashes due to weird driver access
	 * to unconfigured interface (ifup) */
	adev->mgmt_timer.function = (void(*)(unsigned long)) 0x0000dead;

#if defined(NONESSENTIAL_FEATURES)
	acx_show_card_eeprom_id(adev);
#endif /* NONESSENTIAL_FEATURES */

	/* Device setup is finished, now start initializing the card */
	// ---

	acx_init_task_scheduler(adev);

	// Mac80211 Tx_queue
	INIT_WORK(&adev->tx_work, acx_tx_work);
	skb_queue_head_init(&adev->tx_queue);

	// OK init parts from pci.c are done in acxmem_complete_hw_reset(adev)
	if (OK != acxmem_complete_hw_reset(adev))
		goto fail_complete_hw_reset;

	/*
	 * Set up default things for most of the card settings.
	 */
	acx_s_set_defaults(adev);

	/* Register the card, AFTER everything else has been set up,
	 * since otherwise an ioctl could step on our feet due to
	 * firmware operations happening in parallel or uninitialized data */

	if (acx_proc_register_entries(ieee, 0) != OK)
		goto fail_proc_register_entries;

	/* Now we have our device, so make sure the kernel doesn't try
	 * to send packets even though we're not associated to a network yet */

// OW FIXME Check if acx_stop_queue, acx_carrier_off should be included
// OW Rest can be cleaned up
#if 0
	acx_stop_queue(ndev, "on probe");
	acx_carrier_off(ndev, "on probe");
#endif

	printk("acx: net device %s, driver compiled "
        "against wireless extensions %d and Linux %s\n",
        wiphy_name(adev->ieee->wiphy), WIRELESS_EXT, UTS_RELEASE);

	MAC_COPY(adev->ieee->wiphy->perm_addr, adev->dev_addr);

	/** done with board specific setup **/

	/* need to be able to restore PCI state after a suspend */
#ifdef CONFIG_PM
			// pci_save_state(pdev);
#endif

	err = acx_setup_modes(adev);
	if (err) {
		printk("acx: can't setup hwmode\n");
		goto fail_acx_setup_modes;
	}

	err = ieee80211_register_hw(ieee);
	if (OK != err) {
		printk("acx: ieee80211_register_hw() FAILED: %d\n", err);
		goto fail_ieee80211_register_hw;
	}

#if CMD_DISCOVERY
	great_inquisitor(adev);
#endif

	result = OK;
	goto done;


	/* error paths: undo everything in reverse order... */
	fail_ieee80211_register_hw:

	fail_acx_setup_modes:

	fail_proc_register_entries:
	acx_proc_unregister_entries(ieee, 0);

	fail_complete_hw_reset:

	fail_request_irq:
	free_irq(adev->irq, adev);

	fail_ioremap:
	if (adev->iobase)
		iounmap((void *)adev->iobase);

	fail_unknown_chiptype:

	fail_ieee80211_alloc_hw:
	acxmem_delete_dma_regions(adev);
	platform_set_drvdata(pdev, NULL);
	ieee80211_free_hw(ieee);

	done:

	FN_EXIT1(result);
	return result;
}

/*
 * acxmem_e_remove
 *
 * Shut device down (if not hot unplugged)
 * and deallocate PCI resources for the acx chip.
 *
 * pdev - ptr to PCI device structure containing info about pci configuration
 */
static int __devexit acxmem_e_remove(struct platform_device *pdev) {

	struct ieee80211_hw *hw = (struct ieee80211_hw *) platform_get_drvdata(pdev);
	acx_device_t *adev = ieee2adev(hw);
	acxmem_lock_flags;

	FN_ENTER;

	if (!hw) {
		log(L_DEBUG, "acx: %s: card is unused. Skipping any release code\n",
		    __func__);
		goto end_no_lock;
	}

	// Unregister ieee80211 device
	log(L_INIT, "acxmem: removing device %s\n", wiphy_name(adev->ieee->wiphy));
	ieee80211_unregister_hw(adev->ieee);
	CLEAR_BIT(adev->dev_state_mask, ACX_STATE_IFACE_UP);

	/* If device wasn't hot unplugged... */
	if (acxmem_adev_present(adev)) {

		/* disable both Tx and Rx to shut radio down properly */
		if (adev->initialized) {
			acx_s_issue_cmd(adev, ACX1xx_CMD_DISABLE_TX, NULL, 0);
			acx_s_issue_cmd(adev, ACX1xx_CMD_DISABLE_RX, NULL, 0);
			adev->initialized = 0;
		}

#ifdef REDUNDANT
		/* put the eCPU to sleep to save power
		 * Halting is not possible currently,
		 * since not supported by all firmware versions */
		acx_s_issue_cmd(adev, ACX100_CMD_SLEEP, NULL, 0);
#endif

		acxmem_lock();

		/* disable power LED to save power :-) */
		log(L_INIT, "acx: switching off power LED to save power\n");
		acxmem_l_power_led(adev, 0);

		/* stop our eCPU */
		if (IS_ACX111(adev)) {
			/* FIXME: does this actually keep halting the eCPU?
			 * I don't think so...
			 */
			acxmem_reset_mac(adev);
		} else {
			u16 temp;

			/* halt eCPU */
			temp = read_reg16(adev, IO_ACX_ECPU_CTRL) | 0x1;
			write_reg16(adev, IO_ACX_ECPU_CTRL, temp);
			write_flush(adev);
		}

		acxmem_unlock();
	}

	// Proc
	acx_proc_unregister_entries(adev->ieee, 0);

	// IRQs
	acxmem_lock();
	acxmem_irq_disable(adev);
	acxmem_unlock();

	synchronize_irq(adev->irq);
	free_irq(adev->irq, adev);

	/* finally, clean up PCI bus state */
	acxmem_delete_dma_regions(adev);
	if (adev->iobase)
		iounmap(adev->iobase);

	/* remove dev registration */
	platform_set_drvdata(pdev, NULL);

	/* Free netdev (quite late,
	 * since otherwise we might get caught off-guard
	 * by a netdev timeout handler execution
	 * expecting to see a working dev...) */
	ieee80211_free_hw(adev->ieee);

	printk("acxmem: %s done\n", __func__);

	end_no_lock:
	FN_EXIT0;

	return(0);
}

/*
 * TODO: PM code needs to be fixed / debugged / tested.
 */
#ifdef CONFIG_PM
static int
acxmem_e_suspend(struct platform_device *pdev, pm_message_t state) {

	struct ieee80211_hw *hw = (struct ieee80211_hw *) platform_get_drvdata(pdev);
	acx_device_t *adev;

	FN_ENTER;
	printk("acx: suspend handler is experimental!\n");
	printk("acx: sus: dev %p\n", hw);

	/*	if (!netif_running(ndev))
	 goto end;
	 */

	adev = ieee2adev(hw);
	printk("sus: adev %p\n", adev);

	acx_sem_lock(adev);

	ieee80211_unregister_hw(hw); /* this one cannot sleep */
	acxmem_s_down(hw);
	/* down() does not set it to 0xffff, but here we really want that */
	write_reg16(adev, IO_ACX_IRQ_MASK, 0xffff);
	write_reg16(adev, IO_ACX_FEMR, 0x0);
	acxmem_delete_dma_regions(adev);

	/*
	 * Turn the ACX chip off.
	 */
	// This should be done by the corresponding platform module, e.g. hx4700_acx.c
	// hwdata->stop_hw();

	acx_sem_unlock(adev);

	FN_EXIT0;
	return OK;
}

static int acxmem_e_resume(struct platform_device *pdev) {

	struct ieee80211_hw *hw = (struct ieee80211_hw *) platform_get_drvdata(pdev);
	acx_device_t *adev;

	FN_ENTER;

	printk("acx: resume handler is experimental!\n");
	printk("acx: rsm: got dev %p\n", hw);

	/*	if (!netif_running(ndev))
	 return;
	 */

	adev = ieee2adev(hw);
	printk("acx: rsm: got adev %p\n", adev);

	acx_sem_lock(adev);

	/*
	 * Turn on the ACX.
	 */
	// This should be done by the corresponding platform module, e.g. hx4700_acx.c
	// hwdata->start_hw();

	acxmem_complete_hw_reset(adev);

	/*
	 * done by acx_s_set_defaults for initial startup
	 */
	acxmem_set_interrupt_mask(adev);

	printk("acx: rsm: bringing up interface\n");
	SET_BIT (adev->set_mask, GETSET_ALL);
	acxmem_up(hw);
	printk("acx: rsm: acx up done\n");

	/* now even reload all card parameters as they were before suspend,
	 * and possibly be back in the network again already :-)
	 */
	/* - most settings updated in acxmem_s_up() */
	if (ACX_STATE_IFACE_UP & adev->dev_state_mask) {
		adev->set_mask = GETSET_ALL;
		acx_s_update_card_settings(adev);
		printk("acx: rsm: settings updated\n");
	}

	ieee80211_register_hw(hw);
	printk("acx: rsm: device attached\n");

	acx_sem_unlock(adev);

	FN_EXIT0;
	return OK;
}
#endif /* CONFIG_PM */


static struct platform_driver acxmem_drv_id = {
		.driver = {
				.name = "acx-mem",
		},
		.probe = acxmem_e_probe,
		.remove = __devexit_p(acxmem_e_remove),

		#ifdef CONFIG_PM
		.suspend = acxmem_e_suspend,
		.resume = acxmem_e_resume
		#endif /* CONFIG_PM */

};

/*
 * acxmem_e_init_module
 *
 * Module initialization routine, called once at module load time
 */
int __init acxmem_e_init_module(void) {
	int res;

	FN_ENTER;

#if (ACX_IO_WIDTH==32)
	printk("acx: compiled to use 32bit I/O access. "
		"I/O timing issues might occur, such as "
		"non-working firmware upload. Report them\n");
#else
	printk("acx: compiled to use 16bit I/O access only "
			"(compatibility mode)\n");
#endif

#ifdef __LITTLE_ENDIAN
#define ENDIANNESS_STRING "acx: running on a little-endian CPU\n"
#else
#define ENDIANNESS_STRING "acx: running on a BIG-ENDIAN CPU\n"
#endif
	log(L_INIT,
			ENDIANNESS_STRING
			"acx: Slave-memory module initialized, "
			"waiting for cards to probe...\n"
	);

	res = platform_driver_register(&acxmem_drv_id);
	FN_EXIT1(res);
	return res;
}

/*
 * acxmem_e_cleanup_module
 *
 * Called at module unload time. This is our last chance to
 * clean up after ourselves.
 */
void __exit acxmem_e_cleanup_module(void) {
	FN_ENTER;

	printk("acxmem: cleanup_module\n");
	platform_driver_unregister(&acxmem_drv_id);

	FN_EXIT0;
}

MODULE_AUTHOR( "Todd Blumer <todd@sdgsystems.com>" );
MODULE_DESCRIPTION( "ACX Slave Memory Driver" );
MODULE_LICENSE( "GPL" );
