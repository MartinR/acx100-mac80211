/*
 * Copyright (C) 2003, 2004, 2005, 2006, 2007, 2008
 * The ACX100 Open Source Project <acx100-devel@lists.sourceforge.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#define ACX_MAC80211_PCI 1

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
#include <linux/wireless.h>
#include <linux/netdevice.h>
#include <linux/ioport.h>
#include <linux/pci.h>
#include <linux/pm.h>
#include <linux/vmalloc.h>
#include <linux/ethtool.h>
#include <linux/dma-mapping.h>
#include <linux/workqueue.h>
#include <linux/nl80211.h>
#ifdef CONFIG_VLYNQ
#include <linux/vlynq.h>
#endif

#include <net/iw_handler.h>
#include <net/mac80211.h>

#include "acx.h"

/*
 * BOM Config
 * ==================================================
 */

/* Pick one */
/* #define INLINE_IO static */
#define INLINE_IO static inline

#define FW_NO_AUTO_INCREMENT	1

/*
 * BOM Prototypes
 * ... static and also none-static for overview reasons (maybe not best practice ...)
 * ==================================================
 */

// Logging
static void acxpci_log_rxbuffer(const acx_device_t * adev);
static void acxpci_log_txbuffer(acx_device_t * adev);

// Data Access
INLINE_IO u32 read_reg32(acx_device_t * adev, unsigned int offset);
INLINE_IO u16 read_reg16(acx_device_t * adev, unsigned int offset);
INLINE_IO u8 read_reg8(acx_device_t * adev, unsigned int offset);
INLINE_IO void write_reg32(acx_device_t * adev, unsigned int offset, u32 val);
INLINE_IO void write_reg16(acx_device_t * adev, unsigned int offset, u16 val);
INLINE_IO void write_reg8(acx_device_t * adev, unsigned int offset, u8 val);
INLINE_IO void write_flush(acx_device_t * adev);

int acxpci_create_hostdesc_queues(acx_device_t * adev);
static int acxpci_create_rx_host_desc_queue(acx_device_t * adev);
static int acxpci_create_tx_host_desc_queue(acx_device_t * adev);

void acxpci_create_desc_queues(acx_device_t * adev, u32 tx_queue_start, u32 rx_queue_start);
static void acxpci_create_rx_desc_queue(acx_device_t * adev, u32 rx_queue_start);
static void acxpci_create_tx_desc_queue(acx_device_t * adev, u32 tx_queue_start);

void acxpci_free_desc_queues(acx_device_t * adev);
static void acxpci_delete_dma_regions(acx_device_t * adev);
static inline void acxpci_free_coherent(struct pci_dev *hwdev, size_t size, void *vaddr, dma_addr_t dma_handle);
static void *acxpci_allocate(acx_device_t * adev, size_t size, dma_addr_t * phy, const char *msg);

// Firmware, EEPROM, Phy
int acxpci_upload_radio(acx_device_t * adev);
int acxpci_read_eeprom_byte(acx_device_t * adev, u32 addr, u8 * charbuf);
// int acxpci_s_write_eeprom(acx_device_t * adev, u32 addr, u32 len, const u8 * charbuf);
static inline void acxpci_read_eeprom_area(acx_device_t * adev);
int acxpci_read_phy_reg(acx_device_t * adev, u32 reg, u8 * charbuf);
int acxpci_write_phy_reg(acx_device_t * adev, u32 reg, u8 value);
static int acxpci_write_fw(acx_device_t * adev, const firmware_image_t *fw_image, u32 offset);
static int acxpci_validate_fw(acx_device_t * adev, const firmware_image_t *fw_image, u32 offset);
static int acxpci_upload_fw(acx_device_t * adev);
// static void acx_show_card_eeprom_id(acx_device_t * adev);

// CMDs (Control Path)
int acxpci_issue_cmd_timeo_debug(acx_device_t * adev, unsigned cmd, void *buffer, unsigned buflen, unsigned cmd_timeout, const char *cmdstr);
static inline void acxpci_write_cmd_type_status(acx_device_t * adev, u16 type, u16 status);
static u32 acxpci_read_cmd_type_status(acx_device_t *adev);
static inline void acxpci_init_mboxes(acx_device_t * adev);

// Init, Configuration (Control Path)
int acxpci_reset_dev(acx_device_t * adev);
static int acxpci_verify_init(acx_device_t * adev);
static void acxpci_reset_mac(acx_device_t * adev);
static void acxpci_up(struct ieee80211_hw *hw);

// Other (Control Path)

// Proc, Debug
int acxpci_proc_diag_output(struct seq_file *file, acx_device_t *adev);
int acxpci_proc_eeprom_output(char *buf, acx_device_t * adev);

// Rx Path
static void acxpci_process_rxdesc(acx_device_t * adev);

// Tx Path
tx_t *acxpci_alloc_tx(acx_device_t * adev);
void *acxpci_get_txbuf(acx_device_t * adev, tx_t * tx_opaque);
void acxpci_tx_data(acx_device_t *adev, tx_t *tx_opaque, int len, struct ieee80211_tx_info *ieeectl, struct sk_buff *skb);
unsigned int acxpci_clean_txdesc(acx_device_t * adev);
void acxpci_clean_txdesc_emergency(acx_device_t * adev);
static inline txdesc_t *acxpci_get_txdesc(acx_device_t * adev, int index);
static inline txdesc_t *acxpci_advance_txdesc(acx_device_t * adev, txdesc_t * txdesc, int inc);
static txhostdesc_t *acxpci_get_txhostdesc(acx_device_t * adev, txdesc_t * txdesc);
int acx100pci_set_tx_level(acx_device_t * adev, u8 level_dbm);

// Irq Handling, Timer
static void acxpci_irq_enable(acx_device_t * adev);
static void acxpci_irq_disable(acx_device_t * adev);
void acxpci_irq_work(struct work_struct *work);
static irqreturn_t acxpci_interrupt(int irq, void *dev_id);
static void acxpci_handle_info_irq(acx_device_t * adev);
void acxpci_set_interrupt_mask(acx_device_t * adev);

// Mac80211 Ops
static int acxpci_op_start(struct ieee80211_hw *hw);
static void acxpci_op_stop(struct ieee80211_hw *hw);

// Helpers
void acxpci_power_led(acx_device_t * adev, int enable);
INLINE_IO int acxpci_adev_present(acx_device_t *adev);

// Ioctls
int acx111pci_ioctl_info(struct net_device *ndev, struct iw_request_info *info, struct iw_param *vwrq, char *extra);
int acx100pci_ioctl_set_phy_amp_bias(struct net_device *ndev, struct iw_request_info *info, struct iw_param *vwrq, char *extra);

// Driver, Module
static int __devinit acxpci_probe(struct pci_dev *pdev, const struct pci_device_id *id);
static void __devexit acxpci_remove(struct pci_dev *pdev);
#ifdef CONFIG_PM
static int acxpci_e_suspend(struct pci_dev *pdev, pm_message_t state);
static int acxpci_e_resume(struct pci_dev *pdev);
#endif

#ifdef CONFIG_VLYNQ
static int vlynq_probe(struct vlynq_device *vdev, struct vlynq_device_id *id);
static void vlynq_remove(struct vlynq_device *vdev);
#endif

int __init acxpci_init_module(void);
void __exit acxpci_cleanup_module(void);


/*
 * BOM Defines, static vars, etc.
 * ==================================================
 */

// PCI
// -----
#ifdef CONFIG_PCI
#define PCI_TYPE		(PCI_USES_MEM | PCI_ADDR0 | PCI_NO_ACPI_WAKE)
#define PCI_ACX100_REGION1		0x01
#define PCI_ACX100_REGION1_SIZE		0x1000	/* Memory size - 4K bytes */
#define PCI_ACX100_REGION2		0x02
#define PCI_ACX100_REGION2_SIZE		0x10000	/* Memory size - 64K bytes */

#define PCI_ACX111_REGION1		0x00
#define PCI_ACX111_REGION1_SIZE		0x2000	/* Memory size - 8K bytes */
#define PCI_ACX111_REGION2		0x01
#define PCI_ACX111_REGION2_SIZE		0x20000	/* Memory size - 128K bytes */

/* Texas Instruments Vendor ID */
#define PCI_VENDOR_ID_TI		0x104c

/* ACX100 22Mb/s WLAN controller */
#define PCI_DEVICE_ID_TI_TNETW1100A	0x8400
#define PCI_DEVICE_ID_TI_TNETW1100B	0x8401

/* ACX111 54Mb/s WLAN controller */
#define PCI_DEVICE_ID_TI_TNETW1130	0x9066

/* PCI Class & Sub-Class code, Network-'Other controller' */
#define PCI_CLASS_NETWORK_OTHERS	0x0280

#define CARD_EEPROM_ID_SIZE 6

#ifndef PCI_D0
/* From include/linux/pci.h */
#define PCI_D0		0
#define PCI_D1		1
#define PCI_D2		2
#define PCI_D3hot	3
#define PCI_D3cold	4
#define PCI_UNKNOWN	5
#define PCI_POWER_ERROR	-1
#endif
#endif /* CONFIG_PCI */

#define RX_BUFFER_SIZE (sizeof(rxbuffer_t) + 32)

#define MAX_IRQLOOPS_PER_JIFFY  (20000/HZ)

/*
 * BOM Logging
 * ==================================================
 */

static void acxpci_log_rxbuffer(const acx_device_t * adev)
{
	register const struct rxhostdesc *rxhostdesc;
	int i;

	/* no FN_ENTER here, we don't want that */

	rxhostdesc = adev->rxhostdesc_start;
	if (unlikely(!rxhostdesc))
		return;
	for (i = 0; i < RX_CNT; i++) {
		if ((rxhostdesc->Ctl_16 & cpu_to_le16(DESC_CTL_HOSTOWN))
		    && (rxhostdesc->Status & cpu_to_le32(DESC_STATUS_FULL)))
			printk("acx: rx: buf %d full\n", i);
		rxhostdesc++;
	}
}

static void acxpci_log_txbuffer(acx_device_t * adev)
{
	txdesc_t *txdesc;
	int i;

	/* no FN_ENTER here, we don't want that */
	/* no locks here, since it's entirely non-critical code */
	txdesc = adev->txdesc_start;
	if (unlikely(!txdesc))
		return;
	printk("acx: tx: desc->Ctl8's:");
	for (i = 0; i < TX_CNT; i++) {
		printk("acx:  %02X", txdesc->Ctl_8);
		txdesc = acxpci_advance_txdesc(adev, txdesc, 1);
	}
	printk("acx: \n");
}

/*
 * BOM Data Access
 * ==================================================
 */

/* OS I/O routines *always* be endianness-clean but having them doesn't hurt */
#define acx_readl(v)	le32_to_cpu(readl((v)))
#define acx_readw(v)	le16_to_cpu(readw((v)))
#define acx_writew(v,r)	writew(le16_to_cpu((v)), r)
#define acx_writel(v,r)	writel(le32_to_cpu((v)), r)

INLINE_IO u32 read_reg32(acx_device_t * adev, unsigned int offset)
{
#if ACX_IO_WIDTH == 32
	return acx_readl((u8 *) adev->iobase + adev->io[offset]);
#else
	return acx_readw((u8 *) adev->iobase + adev->io[offset])
	    + (acx_readw((u8 *) adev->iobase + adev->io[offset] + 2) << 16);
#endif
}

INLINE_IO u16 read_reg16(acx_device_t * adev, unsigned int offset)
{
	return acx_readw((u8 *) adev->iobase + adev->io[offset]);
}

INLINE_IO u8 read_reg8(acx_device_t * adev, unsigned int offset)
{
	return readb((u8 *) adev->iobase + adev->io[offset]);
}

INLINE_IO void write_reg32(acx_device_t * adev, unsigned int offset, u32 val)
{
#if ACX_IO_WIDTH == 32
	acx_writel(val, (u8 *) adev->iobase + adev->io[offset]);
#else
	acx_writew(val & 0xffff, (u8 *) adev->iobase + adev->io[offset]);
	acx_writew(val >> 16, (u8 *) adev->iobase + adev->io[offset] + 2);
#endif
}

INLINE_IO void write_reg16(acx_device_t * adev, unsigned int offset, u16 val)
{
	acx_writew(val, (u8 *) adev->iobase + adev->io[offset]);
}

INLINE_IO void write_reg8(acx_device_t * adev, unsigned int offset, u8 val)
{
	writeb(val, (u8 *) adev->iobase + adev->io[offset]);
}

/* Handle PCI posting properly:
 * Make sure that writes reach the adapter in case they require to be executed
 * *before* the next write, by reading a random (and safely accessible) register.
 * This call has to be made if there is no read following (which would flush the data
 * to the adapter), yet the written data has to reach the adapter immediately. */
INLINE_IO void write_flush(acx_device_t * adev)
{
	/* readb(adev->iobase + adev->io[IO_ACX_INFO_MAILBOX_OFFS]); */
	/* faster version (accesses the first register, IO_ACX_SOFT_RESET,
	 * which should also be safe): */
	readb(adev->iobase);
}

// -----

int acxpci_create_hostdesc_queues(acx_device_t * adev)
{
	int result;
	result = acxpci_create_tx_host_desc_queue(adev);
	if (OK != result)
		return result;
	result = acxpci_create_rx_host_desc_queue(adev);
	return result;
}

/*
 * acxpci_s_create_rx_host_desc_queue
 *
 * the whole size of a data buffer (header plus data body)
 * plus 32 bytes safety offset at the end
 */
static int acxpci_create_rx_host_desc_queue(acx_device_t * adev)
{
	rxhostdesc_t *hostdesc;
	rxbuffer_t *rxbuf;
	dma_addr_t hostdesc_phy;
	dma_addr_t rxbuf_phy;
	int i;

	FN_ENTER;

	/* allocate the RX host descriptor queue pool */
	adev->rxhostdesc_area_size = RX_CNT * sizeof(*hostdesc);
	adev->rxhostdesc_start = acxpci_allocate(adev, adev->rxhostdesc_area_size,
					  &adev->rxhostdesc_startphy,
					  "rxhostdesc_start");
	if (!adev->rxhostdesc_start)
		goto fail;
	/* check for proper alignment of RX host descriptor pool */
	if ((long)adev->rxhostdesc_start & 3) {
		printk
		    ("acx: driver bug: dma alloc returns unaligned address\n");
		goto fail;
	}

	/* allocate Rx buffer pool which will be used by the acx
	 * to store the whole content of the received frames in it */
	adev->rxbuf_area_size = RX_CNT * RX_BUFFER_SIZE;
	adev->rxbuf_start = acxpci_allocate(adev, adev->rxbuf_area_size,
				     &adev->rxbuf_startphy, "rxbuf_start");
	if (!adev->rxbuf_start)
		goto fail;

	rxbuf = adev->rxbuf_start;
	rxbuf_phy = adev->rxbuf_startphy;
	hostdesc = adev->rxhostdesc_start;
	hostdesc_phy = adev->rxhostdesc_startphy;

	/* don't make any popular C programming pointer arithmetic mistakes
	 * here, otherwise I'll kill you...
	 * (and don't dare asking me why I'm warning you about that...) */
	for (i = 0; i < RX_CNT; i++) {
		hostdesc->data = rxbuf;
		hostdesc->data_phy = cpu2acx(rxbuf_phy);
		hostdesc->length = cpu_to_le16(RX_BUFFER_SIZE);
		CLEAR_BIT(hostdesc->Ctl_16, cpu_to_le16(DESC_CTL_HOSTOWN));
		rxbuf++;
		rxbuf_phy += sizeof(*rxbuf);
		hostdesc_phy += sizeof(*hostdesc);
		hostdesc->desc_phy_next = cpu2acx(hostdesc_phy);
		hostdesc++;
	}
	hostdesc--;
	hostdesc->desc_phy_next = cpu2acx(adev->rxhostdesc_startphy);
	FN_EXIT1(OK);
	return OK;
      fail:
	printk("acx: create_rx_host_desc_queue FAILED\n");
	/* dealloc will be done by free function on error case */
	FN_EXIT1(NOT_OK);
	return NOT_OK;
}

static int acxpci_create_tx_host_desc_queue(acx_device_t * adev)
{
	txhostdesc_t *hostdesc;
	u8 *txbuf;
	dma_addr_t hostdesc_phy;
	dma_addr_t txbuf_phy;
	int i;

	FN_ENTER;

	/* allocate TX buffer */
	// OW 20100513 adev->txbuf_area_size = TX_CNT * /*WLAN_A4FR_MAXLEN_WEP_FCS*/ (30 + 2312 + 4);
	adev->txbuf_area_size = TX_CNT * WLAN_A4FR_MAXLEN_WEP_FCS;
	adev->txbuf_start = acxpci_allocate(adev, adev->txbuf_area_size,
				     &adev->txbuf_startphy, "txbuf_start");
	if (!adev->txbuf_start)
		goto fail;

	/* allocate the TX host descriptor queue pool */
	adev->txhostdesc_area_size = TX_CNT * 2 * sizeof(*hostdesc);
	adev->txhostdesc_start = acxpci_allocate(adev, adev->txhostdesc_area_size,
					  &adev->txhostdesc_startphy,
					  "txhostdesc_start");
	if (!adev->txhostdesc_start)
		goto fail;
	/* check for proper alignment of TX host descriptor pool */
	if ((long)adev->txhostdesc_start & 3) {
		printk
		    ("acx: driver bug: dma alloc returns unaligned address\n");
		goto fail;
	}

	hostdesc = adev->txhostdesc_start;
	hostdesc_phy = adev->txhostdesc_startphy;
	txbuf = adev->txbuf_start;
	txbuf_phy = adev->txbuf_startphy;

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
	for (i = 0; i < TX_CNT * 2; i++) {
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
			hostdesc->length = cpu_to_le16(3);	/* bug workaround */
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
		hostdesc_phy += sizeof(*hostdesc);

		hostdesc->data_phy = cpu2acx(txbuf_phy);
		/* done by memset(0): hostdesc->data_offset = 0; */
		/* hostdesc->reserved = ... */
		hostdesc->Ctl_16 = cpu_to_le16(DESC_CTL_HOSTOWN);
		/* hostdesc->length = ... */
		hostdesc->desc_phy_next = cpu2acx(hostdesc_phy);
		/* done by memset(0): hostdesc->pNext = ptr2acx(NULL); */
		/* hostdesc->Status = ... */
		/* ->data is a non-hardware field: */
		hostdesc->data = txbuf;

		if (!(i & 1)) {
			// OW 20100513 txbuf += 24 /*WLAN_HDR_A3_LEN*/;
			// OW 20100513 txbuf_phy += 24 /*WLAN_HDR_A3_LEN*/;
			txbuf += WLAN_HDR_A3_LEN;
			txbuf_phy += WLAN_HDR_A3_LEN;
		} else {
			// OW 20100513 txbuf +=  30 + 2132 + 4 - 24/*WLAN_A4FR_MAXLEN_WEP_FCS - WLAN_HDR_A3_LEN*/;
			// OW 20100513 txbuf_phy += 30 + 2132 +4  - 24/*WLAN_A4FR_MAXLEN_WEP_FCS - WLAN_HDR_A3_LEN*/;
			txbuf +=  WLAN_A4FR_MAXLEN_WEP_FCS - WLAN_HDR_A3_LEN;
			txbuf_phy += WLAN_A4FR_MAXLEN_WEP_FCS - WLAN_HDR_A3_LEN;
		}
		hostdesc++;
	}
	hostdesc--;
	hostdesc->desc_phy_next = cpu2acx(adev->txhostdesc_startphy);

	FN_EXIT1(OK);
	return OK;
      fail:
	printk("acx: create_tx_host_desc_queue FAILED\n");
	/* dealloc will be done by free function on error case */
	FN_EXIT1(NOT_OK);
	return NOT_OK;
}



void
acxpci_create_desc_queues(acx_device_t * adev, u32 tx_queue_start,
			  u32 rx_queue_start)
{
	acxpci_create_tx_desc_queue(adev, tx_queue_start);
	acxpci_create_rx_desc_queue(adev, rx_queue_start);
}

static void acxpci_create_rx_desc_queue(acx_device_t * adev, u32 rx_queue_start)
{
	rxdesc_t *rxdesc;
	u32 mem_offs;
	int i;

	FN_ENTER;

	/* done by memset: adev->rx_tail = 0; */

	/* ACX111 doesn't need any further config: preconfigures itself.
	 * Simply print ring buffer for debugging */
	if (IS_ACX111(adev)) {
		/* rxdesc_start already set here */

		adev->rxdesc_start =
		    (rxdesc_t *) ((u8 *) adev->iobase2 + rx_queue_start);

		rxdesc = adev->rxdesc_start;
		for (i = 0; i < RX_CNT; i++) {
			log(L_DEBUG, "acx: rx descriptor %d @ 0x%p\n", i, rxdesc);
			rxdesc = adev->rxdesc_start = (rxdesc_t *)
			    (adev->iobase2 + acx2cpu(rxdesc->pNextDesc));
		}
	} else {
		/* we didn't pre-calculate rxdesc_start in case of ACX100 */
		/* rxdesc_start should be right AFTER Tx pool */
		adev->rxdesc_start = (rxdesc_t *)
		    ((u8 *) adev->txdesc_start + (TX_CNT * sizeof(txdesc_t)));
		/* NB: sizeof(txdesc_t) above is valid because we know
		 ** we are in if (acx100) block. Beware of cut-n-pasting elsewhere!
		 ** acx111's txdesc is larger! */

		memset(adev->rxdesc_start, 0, RX_CNT * sizeof(*rxdesc));

		/* loop over whole receive pool */
		rxdesc = adev->rxdesc_start;
		mem_offs = rx_queue_start;
		for (i = 0; i < RX_CNT; i++) {
			log(L_DEBUG, "acx: rx descriptor @ 0x%p\n", rxdesc);
			rxdesc->Ctl_8 = DESC_CTL_RECLAIM | DESC_CTL_AUTODMA;
			/* point to next rxdesc */
			rxdesc->pNextDesc = cpu2acx(mem_offs + sizeof(*rxdesc));
			/* go to the next one */
			mem_offs += sizeof(*rxdesc);
			rxdesc++;
		}
		/* go to the last one */
		rxdesc--;

		/* and point to the first making it a ring buffer */
		rxdesc->pNextDesc = cpu2acx(rx_queue_start);
	}
	FN_EXIT0;
}

static void acxpci_create_tx_desc_queue(acx_device_t * adev, u32 tx_queue_start)
{
	txdesc_t *txdesc;
	txhostdesc_t *hostdesc;
	dma_addr_t hostmemptr;
	u32 mem_offs;
	int i;

	FN_ENTER;

	if (IS_ACX100(adev))
		adev->txdesc_size = sizeof(*txdesc);
	else
		/* the acx111 txdesc is 4 bytes larger */
		adev->txdesc_size = sizeof(*txdesc) + 4;

	adev->txdesc_start = (txdesc_t *) (adev->iobase2 + tx_queue_start);

	log(L_DEBUG, "acx: adev->iobase2=%p\n"
	    "acx: tx_queue_start=%08X\n"
	    "acx: adev->txdesc_start=%p\n",
	    adev->iobase2, tx_queue_start, adev->txdesc_start);

	adev->tx_free = TX_CNT;
	/* done by memset: adev->tx_head = 0; */
	/* done by memset: adev->tx_tail = 0; */
	txdesc = adev->txdesc_start;
	mem_offs = tx_queue_start;
	hostmemptr = adev->txhostdesc_startphy;
	hostdesc = adev->txhostdesc_start;

	if (IS_ACX111(adev)) {
		/* ACX111 has a preinitialized Tx buffer! */
		/* loop over whole send pool */
		/* FIXME: do we have to do the hostmemptr stuff here?? */
		for (i = 0; i < TX_CNT; i++) {
			txdesc->HostMemPtr = ptr2acx(hostmemptr);
			txdesc->Ctl_8 = DESC_CTL_HOSTOWN;
			/* reserve two (hdr desc and payload desc) */
			hostdesc += 2;
			hostmemptr += 2 * sizeof(*hostdesc);
			txdesc = acxpci_advance_txdesc(adev, txdesc, 1);
		}
	} else {
		/* ACX100 Tx buffer needs to be initialized by us */
		/* clear whole send pool. sizeof is safe here (we are acx100) */
		memset(adev->txdesc_start, 0, TX_CNT * sizeof(*txdesc));

		/* loop over whole send pool */
		for (i = 0; i < TX_CNT; i++) {
			log(L_DEBUG, "acx: configure card tx descriptor: 0x%p, "
			    "size: 0x%X\n", txdesc, adev->txdesc_size);

			/* pointer to hostdesc memory */
			txdesc->HostMemPtr = ptr2acx(hostmemptr);
			/* initialise ctl */
			txdesc->Ctl_8 = (DESC_CTL_HOSTOWN | DESC_CTL_RECLAIM
					 | DESC_CTL_AUTODMA |
					 DESC_CTL_FIRSTFRAG);
			/* done by memset(0): txdesc->Ctl2_8 = 0; */
			/* point to next txdesc */
			txdesc->pNextDesc =
			    cpu2acx(mem_offs + adev->txdesc_size);
			/* reserve two (hdr desc and payload desc) */
			hostdesc += 2;
			hostmemptr += 2 * sizeof(*hostdesc);
			/* go to the next one */
			mem_offs += adev->txdesc_size;
			/* ++ is safe here (we are acx100) */
			txdesc++;
		}
		/* go back to the last one */
		txdesc--;
		/* and point to the first making it a ring buffer */
		txdesc->pNextDesc = cpu2acx(tx_queue_start);
	}
	FN_EXIT0;
}


/*
 * acxpci_free_desc_queues
 *
 * Releases the queues that have been allocated, the
 * others have been initialised to NULL so this
 * function can be used if only part of the queues were allocated.
 */
void acxpci_free_desc_queues(acx_device_t * adev)
{

#define ACX_FREE_QUEUE(size, ptr, phyaddr) \
	if (ptr) { \
		acxpci_free_coherent(NULL, size, ptr, phyaddr); \
		ptr = NULL; \
		size = 0; \
	}

	FN_ENTER;

	ACX_FREE_QUEUE(adev->txhostdesc_area_size, adev->txhostdesc_start,
		       adev->txhostdesc_startphy);
	ACX_FREE_QUEUE(adev->txbuf_area_size, adev->txbuf_start,
		       adev->txbuf_startphy);

	adev->txdesc_start = NULL;

	ACX_FREE_QUEUE(adev->rxhostdesc_area_size, adev->rxhostdesc_start,
		       adev->rxhostdesc_startphy);
	ACX_FREE_QUEUE(adev->rxbuf_area_size, adev->rxbuf_start,
		       adev->rxbuf_startphy);

	adev->rxdesc_start = NULL;

	FN_EXIT0;
}

static void acxpci_delete_dma_regions(acx_device_t * adev)
{
	FN_ENTER;
	/* disable radio Tx/Rx. Shouldn't we use the firmware commands
	 * here instead? Or are we that much down the road that it's no
	 * longer possible here? */
	write_reg16(adev, IO_ACX_ENABLE, 0);

	acx_mwait(100);

	/* NO locking for all parts of acxpci_free_desc_queues because:
	 * while calling dma_free_coherent() interrupts need to be 'free'
	 * but if you spinlock the whole function (acxpci_free_desc_queues)
	 * you'll get an error */
	acxpci_free_desc_queues(adev);

	FN_EXIT0;
}

static inline void
acxpci_free_coherent(struct pci_dev *hwdev, size_t size,
	      void *vaddr, dma_addr_t dma_handle)
{
	dma_free_coherent(hwdev == NULL ? NULL : &hwdev->dev,
			  size, vaddr, dma_handle);
}

static void *acxpci_allocate(acx_device_t * adev, size_t size, dma_addr_t * phy,
		      const char *msg)
{
	void *ptr;

	ptr = dma_alloc_coherent(adev->bus_dev, size, phy, GFP_KERNEL);

	if (ptr) {
		log(L_DEBUG, "acx: %s sz=%d adr=0x%p phy=0x%08llx\n",
		    msg, (int)size, ptr, (unsigned long long)*phy);
		memset(ptr, 0, size);
		return ptr;
	}
	printk(KERN_ERR "acx: %s allocation FAILED (%d bytes)\n",
	       msg, (int)size);
	return NULL;
}

/*
 * BOM Firmware, EEPROM, Phy
 * ==================================================
 */

/*
 * acxpci_s_upload_radio
 *
 * Uploads the appropriate radio module firmware into the card.
 *
 * Origin: Standard Read/Write to IO
 */
int acxpci_upload_radio(acx_device_t * adev)
{
	acx_ie_memmap_t mm;
	firmware_image_t *radio_image;
	acx_cmd_radioinit_t radioinit;
	int res = NOT_OK;
	int try;
	u32 offset;
	u32 size;
	char filename[sizeof("tiacx1NNrNN")];

	if (!adev->need_radio_fw)
		return OK;

	FN_ENTER;

	acx_interrogate(adev, &mm, ACX1xx_IE_MEMORY_MAP);
	offset = le32_to_cpu(mm.CodeEnd);

	snprintf(filename, sizeof(filename), "tiacx1%02dr%02X",
		 IS_ACX111(adev) * 11, adev->radio_type);
	radio_image = acx_read_fw(adev->bus_dev, filename, &size);
	if (!radio_image) {
		printk("acx: can't load radio module '%s'\n", filename);
		goto fail;
	}

	acx_issue_cmd(adev, ACX1xx_CMD_SLEEP, NULL, 0);

	for (try = 1; try <= 5; try++) {
		res = acxpci_write_fw(adev, radio_image, offset);
		log(L_DEBUG | L_INIT, "acx: acx_write_fw (radio): %d\n", res);
		if (OK == res) {
			res = acxpci_validate_fw(adev, radio_image, offset);
			log(L_DEBUG | L_INIT, "acx: acx_validate_fw (radio): %d\n",
			    res);
		}

		if (OK == res)
			break;
		printk("acx: radio firmware upload attempt #%d FAILED, "
		       "retrying...\n", try);
		acx_mwait(1000);	/* better wait for a while... */
	}

	acx_issue_cmd(adev, ACX1xx_CMD_WAKE, NULL, 0);
	radioinit.offset = cpu_to_le32(offset);
	/* no endian conversion needed, remains in card CPU area: */
	radioinit.len = radio_image->size;

	vfree(radio_image);

	if (OK != res)
		goto fail;

	/* will take a moment so let's have a big timeout */
	acx_issue_cmd_timeo(adev, ACX1xx_CMD_RADIOINIT,
			      &radioinit, sizeof(radioinit),
			      CMD_TIMEOUT_MS(1000));

	res = acx_interrogate(adev, &mm, ACX1xx_IE_MEMORY_MAP);
      fail:
	FN_EXIT1(res);
	return res;
}

/*
 * acxpci_read_eeprom_byte
 *
 * Function called to read an octet in the EEPROM.
 *
 * This function is used by acxpci_e_probe to check if the
 * connected card is a legal one or not.
 *
 * Arguments:
 *	adev		ptr to acx_device structure
 *	addr		address to read in the EEPROM
 *	charbuf		ptr to a char. This is where the read octet
 *			will be stored
 */
int acxpci_read_eeprom_byte(acx_device_t * adev, u32 addr, u8 * charbuf)
{
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
			printk("acx: %s: timeout waiting for EEPROM read\n",
			       wiphy_name(adev->ieee->wiphy));
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
acxpci_s_write_eeprom(acx_device_t * adev, u32 addr, u32 len,
		      const u8 * charbuf)
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
		result = OK;	/* read data matches, success */

      end:
	kfree(data_verify);
	FN_EXIT1(result);
	return result;
}
#endif /* UNUSED */

static inline void acxpci_read_eeprom_area(acx_device_t * adev)
{
#if ACX_DEBUG > 1
	int offs;
	u8 tmp;

	FN_ENTER;

	for (offs = 0x8c; offs < 0xb9; offs++)
		acxpci_read_eeprom_byte(adev, offs, &tmp);

	FN_EXIT0;
#endif
}

/*
 * acxpci_s_read_phy_reg
 *
 * Messing with rx/tx disabling and enabling here
 * (write_reg32(adev, IO_ACX_ENABLE, 0b000000xx)) kills traffic
 */
int acxpci_read_phy_reg(acx_device_t * adev, u32 reg, u8 * charbuf)
{
	int result = NOT_OK;
	int count;

	FN_ENTER;

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
			printk("acx: %s: timeout waiting for phy read\n",
			       wiphy_name(adev->ieee->wiphy));
			*charbuf = 0;
			goto fail;
		}
		cpu_relax();
	}

	log(L_DEBUG, "acx: the count was %u\n", count);
	*charbuf = read_reg8(adev, IO_ACX_PHY_DATA);

	log(L_DEBUG, "acx: radio PHY at 0x%04X = 0x%02X\n", *charbuf, reg);
	result = OK;
	goto fail;		/* silence compiler warning */
      fail:
	FN_EXIT1(result);
	return result;
}


int acxpci_write_phy_reg(acx_device_t * adev, u32 reg, u8 value)
{
	FN_ENTER;

	/* mprusko said that 32bit accesses result in distorted sensitivity
	 * on his card. Unconfirmed, looks like it's not true (most likely since we
	 * now properly flush writes). */
	write_reg32(adev, IO_ACX_PHY_DATA, value);
	write_reg32(adev, IO_ACX_PHY_ADDR, reg);
	write_flush(adev);
	write_reg32(adev, IO_ACX_PHY_CTL, 1);
	write_flush(adev);
	log(L_DEBUG, "acx: radio PHY write 0x%02X at 0x%04X\n", value, reg);

	FN_EXIT0;
	return OK;
}



/*
 * acxpci_s_write_fw
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
 *
 * Standard csum implementation + write to IO
 */
static int
acxpci_write_fw(acx_device_t * adev, const firmware_image_t *fw_image,
		  u32 offset)
{
	int len, size;
	u32 sum, v32;
	/* we skip the first four bytes which contain the control sum */

	const u8 *p = (u8 *) fw_image + 4;

	FN_ENTER;

	/* start the image checksum by adding the image size value */
	sum = p[0] + p[1] + p[2] + p[3];
	p += 4;

	write_reg32(adev, IO_ACX_SLV_END_CTL, 0);

#if FW_NO_AUTO_INCREMENT
	write_reg32(adev, IO_ACX_SLV_MEM_CTL, 0);	/* use basic mode */
#else
	write_reg32(adev, IO_ACX_SLV_MEM_CTL, 1);	/* use autoincrement mode */
	write_reg32(adev, IO_ACX_SLV_MEM_ADDR, offset);	/* configure start address */
	write_flush(adev);
#endif

	len = 0;
	size = le32_to_cpu(fw_image->size) & (~3);

	while (likely(len < size)) {
		v32 = be32_to_cpu(*(u32 *) p);
		sum += p[0] + p[1] + p[2] + p[3];
		p += 4;
		len += 4;

#if FW_NO_AUTO_INCREMENT
		write_reg32(adev, IO_ACX_SLV_MEM_ADDR, offset + len - 4);
		write_flush(adev);
#endif
		write_reg32(adev, IO_ACX_SLV_MEM_DATA, v32);
	}

	log(L_DEBUG, "acx: firmware written, size:%d sum1:%x sum2:%x\n",
	    size, sum, le32_to_cpu(fw_image->chksum));

	/* compare our checksum with the stored image checksum */
	FN_EXIT1(sum != le32_to_cpu(fw_image->chksum));
	return (sum != le32_to_cpu(fw_image->chksum));
}


/*
 * acxpci_s_validate_fw
 *
 * Compare the firmware image given with
 * the firmware image written into the card.
 *
 * Arguments:
 *	adev		wlan device structure
 *   fw_image  firmware image.
 *
 * Returns:
 *	NOT_OK	firmware image corrupted or not correctly written
 *	OK	success
 *
 * Origin: Standard csum + Read IO
 */
static int
acxpci_validate_fw(acx_device_t * adev, const firmware_image_t *fw_image,
		     u32 offset)
{
	u32 sum, v32, w32;
	int len, size;
	int result = OK;
	/* we skip the first four bytes which contain the control sum */
	const u8 *p = (u8 *) fw_image + 4;

	FN_ENTER;

	/* start the image checksum by adding the image size value */
	sum = p[0] + p[1] + p[2] + p[3];
	p += 4;

	write_reg32(adev, IO_ACX_SLV_END_CTL, 0);

#if FW_NO_AUTO_INCREMENT
	write_reg32(adev, IO_ACX_SLV_MEM_CTL, 0);	/* use basic mode */
#else
	write_reg32(adev, IO_ACX_SLV_MEM_CTL, 1);	/* use autoincrement mode */
	write_reg32(adev, IO_ACX_SLV_MEM_ADDR, offset);	/* configure start address */
#endif

	len = 0;
	size = le32_to_cpu(fw_image->size) & (~3);

	while (likely(len < size)) {
		v32 = be32_to_cpu(*(u32 *) p);
		p += 4;
		len += 4;

#if FW_NO_AUTO_INCREMENT
		write_reg32(adev, IO_ACX_SLV_MEM_ADDR, offset + len - 4);
#endif
		w32 = read_reg32(adev, IO_ACX_SLV_MEM_DATA);

		if (unlikely(w32 != v32)) {
			printk("acx: FATAL: firmware upload: "
			       "data parts at offset %d don't match (0x%08X vs. 0x%08X)! "
			       "I/O timing issues or defective memory, with DWL-xx0+? "
			       "ACX_IO_WIDTH=16 may help. Please report\n",
			       len, v32, w32);
			result = NOT_OK;
			break;
		}

		sum +=
		    (u8) w32 + (u8) (w32 >> 8) + (u8) (w32 >> 16) +
		    (u8) (w32 >> 24);
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


/*
 * acxpci_s_upload_fw
 *
 * Called from acx_reset_dev
 *
 * Origin: Derived from FW dissection
 */
static int acxpci_upload_fw(acx_device_t * adev)
{
	firmware_image_t *fw_image = NULL;
	int res = NOT_OK;
	int try;
	u32 file_size;
	char filename[sizeof("tiacx1NNcNN")];

	FN_ENTER;

	/* print exact chipset and radio ID to make sure people
	 * really get a clue on which files exactly they need to provide.
	 * Firmware loading is a frequent end-user PITA with these chipsets.
	 */
	printk( "acx: need firmware for acx1%02d chipset with radio ID %02X\n"
		"Please provide via firmware hotplug:\n"
		"either combined firmware (single file named 'tiacx1%02dc%02X')\n"
		"or two files (base firmware file 'tiacx1%02d' "
		"+ radio fw 'tiacx1%02dr%02X')\n",
		IS_ACX111(adev)*11, adev->radio_type,
		IS_ACX111(adev)*11, adev->radio_type,
		IS_ACX111(adev)*11,
		IS_ACX111(adev)*11, adev->radio_type
		);

	/* print exact chipset and radio ID to make sure people really get a clue on which files exactly they are supposed to provide,
	 * since firmware loading is the biggest enduser PITA with these chipsets.
	 * Not printing radio ID in 0xHEX in order to not confuse them into wrong file naming */
	printk(	"acx: need to load firmware for acx1%02d chipset with radio ID %02x, please provide via firmware hotplug:\n"
		"acx: either one file only (<c>ombined firmware image file, radio-specific) or two files (radio-less base image file *plus* separate <r>adio-specific extension file)\n",
		IS_ACX111(adev)*11, adev->radio_type);

	/* Try combined, then main image */
	adev->need_radio_fw = 0;
	snprintf(filename, sizeof(filename), "tiacx1%02dc%02X",
		 IS_ACX111(adev) * 11, adev->radio_type);

	fw_image = acx_read_fw(adev->bus_dev, filename, &file_size);
	if (!fw_image) {
		adev->need_radio_fw = 1;
		filename[sizeof("tiacx1NN") - 1] = '\0';
		fw_image =
		    acx_read_fw(adev->bus_dev, filename, &file_size);
		if (!fw_image) {
			FN_EXIT1(NOT_OK);
			return NOT_OK;
		}
	}

	for (try = 1; try <= 5; try++) {
		res = acxpci_write_fw(adev, fw_image, 0);
		log(L_DEBUG | L_INIT, "acx: acx_write_fw (main/combined): %d\n", res);
		if (OK == res) {
			res = acxpci_validate_fw(adev, fw_image, 0);
			log(L_DEBUG | L_INIT, "acx: acx_validate_fw "
			    		"(main/combined): %d\n", res);
		}

		if (OK == res) {
			SET_BIT(adev->dev_state_mask, ACX_STATE_FW_LOADED);
			break;
		}
		printk("acx: firmware upload attempt #%d FAILED, "
		       "retrying...\n", try);
		acx_mwait(1000);	/* better wait for a while... */
	}

	vfree(fw_image);

	FN_EXIT1(res);
	return res;
}


#ifdef NONESSENTIAL_FEATURES
typedef struct device_id {
	unsigned char id[6];
	char *descr;
	char *type;
} device_id_t;

static const device_id_t device_ids[] = {
	{
	 {'G', 'l', 'o', 'b', 'a', 'l'},
	 NULL,
	 NULL,
	 },
	{
	 {0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
	 "uninitialized",
	 "SpeedStream SS1021 or Gigafast WF721-AEX"},
	{
	 {0x80, 0x81, 0x82, 0x83, 0x84, 0x85},
	 "non-standard",
	 "DrayTek Vigor 520"},
	{
	 {'?', '?', '?', '?', '?', '?'},
	 "non-standard",
	 "Level One WPC-0200"},
	{
	 {0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
	 "empty",
	 "DWL-650+ variant"}
};

static void acx_show_card_eeprom_id(acx_device_t * adev)
{
	unsigned char buffer[CARD_EEPROM_ID_SIZE];
	int i;

	FN_ENTER;

	memset(&buffer, 0, CARD_EEPROM_ID_SIZE);
	/* use direct EEPROM access */
	for (i = 0; i < CARD_EEPROM_ID_SIZE; i++) {
		if (OK != acxpci_read_eeprom_byte(adev,
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
 * acxpci_s_issue_cmd_timeo
 *
 * Sends command to fw, extract result
 *
 * NB: we do _not_ take lock inside, so be sure to not touch anything
 * which may interfere with IRQ handler operation
 *
 * TODO: busy wait is a bit silly, so:
 * 1) stop doing many iters - go to sleep after first
 * 2) go to waitqueue based approach: wait, not poll!
 */
int
acxpci_issue_cmd_timeo_debug(acx_device_t * adev,
			       unsigned cmd,
			       void *buffer,
			       unsigned buflen,
			       unsigned cmd_timeout, const char *cmdstr)
{
	unsigned long start = jiffies;
	const char *devname;
	unsigned counter;
	u16 irqtype;
	u16 cmd_status=-1;
	unsigned long timeout;

	FN_ENTER;

	devname = wiphy_name(adev->ieee->wiphy);
	if (!devname || !devname[0] || devname[4] == '%')
		devname = "acx";

	log(L_CTL, "acx: %s: cmd=%s, buflen=%u, timeout=%ums, type=0x%04X\n",
	    __func__, cmdstr, buflen, cmd_timeout,
	    buffer ? le16_to_cpu(((acx_ie_generic_t *) buffer)->type) : -1);

	if (!(adev->dev_state_mask & ACX_STATE_FW_LOADED)) {
		printk("acx: %s: %s: firmware is not loaded yet, "
		       "cannot execute commands!\n", 
           __func__, devname);
		goto bad;
	}

	if ((acx_debug & L_DEBUG) && (cmd != ACX1xx_CMD_INTERROGATE)) {
		printk("acx: input buffer (len=%u):\n", buflen);
		acx_dump_bytes(buffer, buflen);
	}

	/* wait for firmware to become idle for our command submission */
	timeout = HZ / 5;
	counter = (timeout * 1000 / HZ) - 1;	/* in ms */
	timeout += jiffies;
	do {
		cmd_status = acxpci_read_cmd_type_status(adev);
		/* Test for IDLE state */
		if (!cmd_status)
			break;
		if (counter % 8 == 0) {
			if (time_after(jiffies, timeout)) {
				counter = 0;
				break;
			}
			/* we waited 8 iterations, no luck. Sleep 8 ms */
			acx_mwait(8);
		}
	} while (likely(--counter));

	if (!counter) {
		/* the card doesn't get idle, we're in trouble */
		printk("acx: %s: %s: cmd_status is not IDLE: 0x%04X!=0\n",
		       __func__, devname, cmd_status);
		goto bad;
	} else if (counter < 190) {	/* if waited >10ms... */
		log(L_CTL | L_DEBUG, "acx: %s: waited for IDLE %dms. "
		    "Please report\n", 
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
		/* adev->cmd_area points to PCI device's memory, not to RAM! */
		memcpy_toio(adev->cmd_area + 4, buffer,
			    (cmd == ACX1xx_CMD_INTERROGATE) ? 4 : buflen);
	}
	/* now write the actual command type */
	acxpci_write_cmd_type_status(adev, cmd, 0);

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
	if (unlikely(cmd_timeout > 1199))
		cmd_timeout = 1199;

	/* we schedule away sometimes (timeout can be large) */
	counter = cmd_timeout;
	timeout = jiffies + cmd_timeout * HZ / 1000;


	do {
		irqtype = read_reg16(adev, IO_ACX_IRQ_STATUS_NON_DES);
		if (irqtype & HOST_INT_CMD_COMPLETE) {
			write_reg16(adev, IO_ACX_IRQ_ACK,
				    HOST_INT_CMD_COMPLETE);
			break;
		}

		if (adev->irq_status & HOST_INT_CMD_COMPLETE)
			break;

		if (counter % 8 == 0) {
			// Timeout
			if (time_after(jiffies, timeout)) {
				counter = -1;
				break;
			}
			/* we waited 8 iterations, no luck. Sleep 8 ms */
			acx_mwait(8);
		}
	} while (likely(--counter));

	/* save state for debugging */
	cmd_status = acxpci_read_cmd_type_status(adev);

	/* put the card in IDLE state */
	acxpci_write_cmd_type_status(adev, 0, 0);

	/* Timed out! */
	if (counter == -1) {
		log(L_ANY, "acx: %s: %s: timed out %s for CMD_COMPLETE. "
		       "irq bits:0x%04X irq_status:0x%04X timeout:%dms "
		       "cmd_status:%d (%s)\n",
		       __func__, devname, 
                       (adev->irqs_active) ? "waiting" : "polling",
		       irqtype, adev->irq_status, cmd_timeout,
		       cmd_status, acx_cmd_status_str(cmd_status));
		log(L_ANY, "acx: timeout: counter:%d cmd_timeout:%d cmd_timeout-counter:%d\n",
				counter, cmd_timeout, cmd_timeout - counter);

	} else if ((cmd_timeout - counter) > 30) {	/* if waited >30ms... */
		log(L_CTL | L_DEBUG, "acx: %s: %s for CMD_COMPLETE %dms. "
		    "count:%d. Please report\n",
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

	if (1 != cmd_status) {	/* it is not a 'Success' */
		/* zero out result buffer
		 * WARNING: this will trash stack in case of illegally large input
		 * length! */
		if (buffer && buflen)
			memset(buffer, 0, buflen);
		goto bad;
	}

	/* read in result parameters if needed */
	if (buffer && buflen && (cmd == ACX1xx_CMD_INTERROGATE)) {
		/* adev->cmd_area points to PCI device's memory, not to RAM! */
		memcpy_fromio(buffer, adev->cmd_area + 4, buflen);
		if (acx_debug & L_DEBUG) {
			printk("acx: output buffer (len=%u): ", buflen);
			acx_dump_bytes(buffer, buflen);
		}
	}
	/* ok: */
	log(L_DEBUG, "acx: %s: %s: took %ld jiffies to complete\n",
	    __func__, cmdstr, jiffies - start);
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
	// dump_stack();
	FN_EXIT1(NOT_OK);
	
	return NOT_OK;
}

static inline void
acxpci_write_cmd_type_status(acx_device_t * adev, u16 type, u16 status)
{
	FN_ENTER;
	acx_writel(type | (status << 16), adev->cmd_area);
	write_flush(adev);
	FN_EXIT0;
}


static u32 acxpci_read_cmd_type_status(acx_device_t *adev)
{
	u32 cmd_type, cmd_status;

	FN_ENTER;

	cmd_type = acx_readl(adev->cmd_area);
	cmd_status = (cmd_type >> 16);
	cmd_type = (u16) cmd_type;

	logf1(L_DEBUG, "cmd_type=%04X cmd_status=%04X [%s]\n",
	    cmd_type, cmd_status, acx_cmd_status_str(cmd_status));

	FN_EXIT1(cmd_status);
	return cmd_status;
}

static inline void acxpci_init_mboxes(acx_device_t * adev)
{
	u32 cmd_offs, info_offs;

	FN_ENTER;

	cmd_offs = read_reg32(adev, IO_ACX_CMD_MAILBOX_OFFS);
	info_offs = read_reg32(adev, IO_ACX_INFO_MAILBOX_OFFS);
	adev->cmd_area = (u8 *) adev->iobase2 + cmd_offs;
	adev->info_area = (u8 *) adev->iobase2 + info_offs;
	log(L_DEBUG, "acx: iobase2=%p\n"
	    "acx: cmd_mbox_offset=%X cmd_area=%p\n"
	    "acx: info_mbox_offset=%X info_area=%p\n",
	    adev->iobase2,
	    cmd_offs, adev->cmd_area, info_offs, adev->info_area);
	FN_EXIT0;
}

/*
 * BOM Init, Configuration (Control Path)
 * ==================================================
 */

/*
 * acxpci_s_reset_dev
 *
 * Arguments:
 *	netdevice that contains the adev variable
 * Returns:
 *	NOT_OK on fail
 *	OK on success
 * Side effects:
 *	device is hard reset
 * Call context:
 *	acxpci_e_probe
 * Comment:
 *	This resets the device using low level hardware calls
 *	as well as uploads and verifies the firmware to the card
 */
int acxpci_reset_dev(acx_device_t * adev)
{
	const char *msg = "";
	int result = NOT_OK;
	u16 hardware_info;
	u16 ecpu_ctrl;
	int count;

	FN_ENTER;

	/* reset the device to make sure the eCPU is stopped
	 * to upload the firmware correctly */

#ifdef CONFIG_PCI
	acxpci_reset_mac(adev);
#endif

	ecpu_ctrl = read_reg16(adev, IO_ACX_ECPU_CTRL) & 1;
	if (!ecpu_ctrl) {
		msg = "acx: eCPU is already running. ";
		goto end_unlock;
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
		       "is this fatal?\n", wiphy_name(adev->ieee->wiphy));
	}
#endif
	/* scan, if any, is stopped now, setting corresponding IRQ bit */
	SET_BIT(adev->irq_status, HOST_INT_SCAN_COMPLETE);

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
	} while (!(hardware_info & 0xff00));	/* radio type still zero? */

	/* printk("acx: DEBUG: count %d\n", count); */
	adev->form_factor = hardware_info & 0xff;
	adev->radio_type = hardware_info >> 8;

	/* load the firmware */
	if (OK != acxpci_upload_fw(adev))
		goto end_fail;

	/* acx_s_mwait(10);    this one really shouldn't be required */

	/* now start eCPU by clearing bit */
	write_reg16(adev, IO_ACX_ECPU_CTRL, ecpu_ctrl & ~0x1);
	log(L_DEBUG, "acx: booted eCPU up and waiting for completion...\n");

	/* wait for eCPU bootup */
	if (OK != acxpci_verify_init(adev)) {
		msg = "acx: timeout waiting for eCPU. ";
		goto end_fail;
	}
	log(L_DEBUG, "acx: eCPU has woken up, card is ready to be configured\n");

	acxpci_init_mboxes(adev);
	acxpci_write_cmd_type_status(adev, 0, 0);

	/* test that EEPROM is readable */
	acxpci_read_eeprom_area(adev);

	result = OK;
	goto end;

/* Finish error message. Indicate which function failed */
      end_unlock:

      end_fail:
	printk("acx: %sreset_dev() FAILED\n", msg);
      end:
	FN_EXIT1(result);
	return result;
}

static int acxpci_verify_init(acx_device_t * adev)
{
	int result = NOT_OK;
	unsigned long timeout;

	FN_ENTER;

	timeout = jiffies + 2 * HZ;
	for (;;) {
		u16 irqstat = read_reg16(adev, IO_ACX_IRQ_STATUS_NON_DES);
		if (irqstat & HOST_INT_FCS_THRESHOLD) {
			result = OK;
			write_reg16(adev, IO_ACX_IRQ_ACK,
				    HOST_INT_FCS_THRESHOLD);
			break;
		}
		if (time_after(jiffies, timeout))
			break;
		/* Init may take up to ~0.5 sec total */
		acx_mwait(50);
	}

	FN_EXIT1(result);
	return result;
}

/*
 * acxpci_l_reset_mac
 *
 * MAC will be reset
 * Call context: reset_dev
 *
 * Origin: Standard Read/Write to IO
 */
static void acxpci_reset_mac(acx_device_t * adev)
{
	u16 temp;

	FN_ENTER;

	/* halt eCPU */
	temp = read_reg16(adev, IO_ACX_ECPU_CTRL) | 0x1;
	write_reg16(adev, IO_ACX_ECPU_CTRL, temp);

	/* now do soft reset of eCPU, set bit */
	temp = read_reg16(adev, IO_ACX_SOFT_RESET) | 0x1;
	log(L_DEBUG, "acx: enable soft reset\n");
	write_reg16(adev, IO_ACX_SOFT_RESET, temp);
	write_flush(adev);

	/* now clear bit again: deassert eCPU reset */
	log(L_DEBUG, "acx: disable soft reset and go to init mode\n");
	write_reg16(adev, IO_ACX_SOFT_RESET, temp & ~0x1);

	/* now start a burst read from initial EEPROM */
	temp = read_reg16(adev, IO_ACX_EE_START) | 0x1;
	write_reg16(adev, IO_ACX_EE_START, temp);
	write_flush(adev);

	FN_EXIT0;
}

/*
 * acxpci_s_up
 *
 * This function is called by acxpci_e_open (when ifconfig sets the device as up)
 *
 * Side effects:
 * - Enables on-card interrupt requests
 * - calls acx_s_start
 */
static void acxpci_up(struct ieee80211_hw *hw)
{
	acx_device_t *adev = ieee2adev(hw);

	FN_ENTER;

	acxpci_irq_enable(adev);

	/* acx fw < 1.9.3.e has a hardware timer, and older drivers
	 ** used to use it. But we don't do that anymore, our OS
	 ** has reliable software timers */
	init_timer(&adev->mgmt_timer);
	adev->mgmt_timer.function = acx_timer;
	adev->mgmt_timer.data = (unsigned long)adev;

	/* Need to set ACX_STATE_IFACE_UP first, or else
	 ** timer won't be started by acx_set_status() */
	SET_BIT(adev->dev_state_mask, ACX_STATE_IFACE_UP);

	acx_start(adev);

	FN_EXIT0;
}

/*
 * BOM Other (Control Path)
 * ==================================================
 */

/* FIXME: update_link_quality_led was a stub - let's comment it and avoid
 * compiler warnings */
/*
static void update_link_quality_led(acx_device_t * adev)
{
	int qual;

	qual =
	    acx_signal_determine_quality(adev->wstats.qual.level,
					 adev->wstats.qual.noise);
	if (qual > adev->brange_max_quality)
		qual = adev->brange_max_quality;

	if (time_after(jiffies, adev->brange_time_last_state_change +
		       (HZ / 2 -
			HZ / 2 * (unsigned long)qual /
			adev->brange_max_quality))) {
		acxpci_l_power_led(adev, (adev->brange_last_state == 0));
		adev->brange_last_state ^= 1;	// toggle
		adev->brange_time_last_state_change = jiffies;
	}
}
*/


/*
 * BOM Proc, Debug
 * ==================================================
 */

int acxpci_proc_diag_output(struct seq_file *file, acx_device_t *adev)
{
	const char *rtl, *thd, *ttl;
	rxhostdesc_t *rxhostdesc;
	txdesc_t *txdesc;
	int i;

	FN_ENTER;

	seq_printf(file, "** Rx buf **\n");
	rxhostdesc = adev->rxhostdesc_start;
	if (rxhostdesc)
		for (i = 0; i < RX_CNT; i++) {
			rtl = (i == adev->rx_tail) ? " [tail]" : "";
			if ((rxhostdesc->Ctl_16 & cpu_to_le16(DESC_CTL_HOSTOWN))
			    && (rxhostdesc->
				Status & cpu_to_le32(DESC_STATUS_FULL)))
				seq_printf(file, "%02u FULL%s\n", i, rtl);
			else
				seq_printf(file, "%02u empty%s\n", i, rtl);
			rxhostdesc++;
		}

	seq_printf(file, "** Tx buf (free %d, Ieee80211 queue: %s) **\n",
			adev->tx_free,
			acx_queue_stopped(adev->ieee) ? "STOPPED" : "running");

	txdesc = adev->txdesc_start;
	if (txdesc)
		for (i = 0; i < TX_CNT; i++) {
			thd = (i == adev->tx_head) ? " [head]" : "";
			ttl = (i == adev->tx_tail) ? " [tail]" : "";

			if (txdesc->Ctl_8 & DESC_CTL_ACXDONE)
				seq_printf(file, "%02u Ready to free (%02X)%s%s", i, txdesc->Ctl_8,
						thd, ttl);
			else if (txdesc->Ctl_8 & DESC_CTL_HOSTOWN)
				seq_printf(file, "%02u Available     (%02X)%s%s", i, txdesc->Ctl_8,
						thd, ttl);
			else
				seq_printf(file, "%02u Busy          (%02X)%s%s", i, txdesc->Ctl_8,
						thd, ttl);
			seq_printf(file, "\n");

			txdesc = acxpci_advance_txdesc(adev, txdesc, 1);
		}
	seq_printf(file,
		     "\n"
		     "** PCI data **\n"
		     "txbuf_start %p, txbuf_area_size %u, txbuf_startphy %08llx\n"
		     "txdesc_size %u, txdesc_start %p\n"
		     "txhostdesc_start %p, txhostdesc_area_size %u, txhostdesc_startphy %08llx\n"
		     "rxdesc_start %p\n"
		     "rxhostdesc_start %p, rxhostdesc_area_size %u, rxhostdesc_startphy %08llx\n"
		     "rxbuf_start %p, rxbuf_area_size %u, rxbuf_startphy %08llx\n",
		     adev->txbuf_start, adev->txbuf_area_size,
		     (unsigned long long)adev->txbuf_startphy,
		     adev->txdesc_size, adev->txdesc_start,
		     adev->txhostdesc_start, adev->txhostdesc_area_size,
		     (unsigned long long)adev->txhostdesc_startphy,
		     adev->rxdesc_start,
		     adev->rxhostdesc_start, adev->rxhostdesc_area_size,
		     (unsigned long long)adev->rxhostdesc_startphy,
		     adev->rxbuf_start, adev->rxbuf_area_size,
		     (unsigned long long)adev->rxbuf_startphy);

	FN_EXIT0;
	return 0;
}

int acxpci_proc_eeprom_output(char *buf, acx_device_t * adev)
{
	char *p = buf;
	int i;

	FN_ENTER;

	for (i = 0; i < 0x400; i++) {
		acxpci_read_eeprom_byte(adev, i, p++);
	}

	FN_EXIT1(p - buf);
	return p - buf;
}


/*
 * BOM Rx Path
 * ==================================================
 */



/*
 * acxpci_l_process_rxdesc
 *
 * Called directly and only from the IRQ handler
 */
static void acxpci_process_rxdesc(acx_device_t * adev)
{
	register rxhostdesc_t *hostdesc;
	unsigned count, tail;

	FN_ENTER;

	if (unlikely(acx_debug & L_BUFR))
		acxpci_log_rxbuffer(adev);

	/* First, have a loop to determine the first descriptor that's
	 * full, just in case there's a mismatch between our current
	 * rx_tail and the full descriptor we're supposed to handle. */
	tail = adev->rx_tail;
	count = RX_CNT;
	while (1) {
		hostdesc = &adev->rxhostdesc_start[tail];
		/* advance tail regardless of outcome of the below test */
		tail = (tail + 1) % RX_CNT;

		if ((hostdesc->Ctl_16 & cpu_to_le16(DESC_CTL_HOSTOWN))
		    && (hostdesc->Status & cpu_to_le32(DESC_STATUS_FULL)))
			break;	/* found it! */

		if (unlikely(!--count))	/* hmm, no luck: all descs empty, bail out */
			goto end;
	}

	/* now process descriptors, starting with the first we figured out */
	while (1) {
		log(L_BUFR, "acx: rx: tail=%u Ctl_16=%04X Status=%08X\n",
		    tail, hostdesc->Ctl_16, hostdesc->Status);

		acx_process_rxbuf(adev, hostdesc->data);
		hostdesc->Status = 0;
		/* flush all writes before adapter sees CTL_HOSTOWN change */
		wmb();
		/* Host no longer owns this, needs to be LAST */
		CLEAR_BIT(hostdesc->Ctl_16, cpu_to_le16(DESC_CTL_HOSTOWN));

		/* ok, descriptor is handled, now check the next descriptor */
		hostdesc = &adev->rxhostdesc_start[tail];

		/* if next descriptor is empty, then bail out */
		if (!(hostdesc->Ctl_16 & cpu_to_le16(DESC_CTL_HOSTOWN))
		    || !(hostdesc->Status & cpu_to_le32(DESC_STATUS_FULL)))
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
 * acxpci_l_alloc_tx
 * Actually returns a txdesc_t* ptr
 *
 * FIXME: in case of fragments, should allocate multiple descrs
 * after figuring out how many we need and whether we still have
 * sufficiently many.
 */
tx_t* acxpci_alloc_tx(acx_device_t * adev)
{
	struct txdesc *txdesc;
	unsigned head;
	u8 ctl8;

	FN_ENTER;

	if (unlikely(!adev->tx_free)) {
		printk("acx: BUG: no free txdesc left\n");
		txdesc = NULL;
		goto end;
	}

	head = adev->tx_head;
	txdesc = acxpci_get_txdesc(adev, head);
	ctl8 = txdesc->Ctl_8;

	/* 2005-10-11: there were several bug reports on this happening
	 ** but now cause seems to be understood & fixed */

	// TODO OW Check if this is correct
	if (unlikely(DESC_CTL_HOSTOWN != (ctl8 & DESC_CTL_ACXDONE_HOSTOWN))) {
		/* whoops, descr at current index is not free, so probably
		 * ring buffer already full */
		printk("acx: BUG: tx_head:%d Ctl8:0x%02X - failed to find "
		       "free txdesc\n", head, ctl8);
		txdesc = NULL;
		goto end;
	}

	/* Needed in case txdesc won't be eventually submitted for tx */
	txdesc->Ctl_8 = DESC_CTL_ACXDONE_HOSTOWN;

	adev->tx_free--;
	log(L_BUFT, "acx: tx: got desc %u, %u remain\n", head, adev->tx_free);

	/* returning current descriptor, so advance to next free one */
	adev->tx_head = (head + 1) % TX_CNT;
      end:
	FN_EXIT0;

	return (tx_t *) txdesc;
}


void *acxpci_get_txbuf(acx_device_t * adev, tx_t * tx_opaque)
{
	return acxpci_get_txhostdesc(adev, (txdesc_t *) tx_opaque)->data;
}


/*
 * acxpci_l_tx_data
 *
 * Can be called from IRQ (rx -> (AP bridging or mgmt response) -> tx).
 * Can be called from acx_i_start_xmit (data frames from net core).
 *
 * FIXME: in case of fragments, should loop over the number of
 * pre-allocated tx descrs, properly setting up transfer data and
 * CTL_xxx flags according to fragment number.
 */
void
acxpci_tx_data(acx_device_t *adev, tx_t *tx_opaque, int len,
		 struct ieee80211_tx_info *ieeectl,struct sk_buff *skb)
{
	txdesc_t *txdesc = (txdesc_t *) tx_opaque;
	struct ieee80211_hdr *wireless_header;
	txhostdesc_t *hostdesc1, *hostdesc2;
	int rate;
	u8 Ctl_8, Ctl2_8;
	int wlhdr_len;

	int bitrate;

	FN_ENTER;

	/* fw doesn't tx such packets anyhow */
	/*	if (unlikely(len < WLAN_HDR_A3_LEN))
		goto end;
	 */

	hostdesc1 = acxpci_get_txhostdesc(adev, txdesc);
	wireless_header = (struct ieee80211_hdr *)hostdesc1->data;

	//wlhdr_len = ieee80211_hdrlen(le16_to_cpu(wireless_header->frame_control));
	wlhdr_len = WLAN_HDR_A3_LEN;

	/* modify flag status in separate variable to be able to write it back
	 * in one big swoop later (also in order to have less device memory
	 * accesses) */
	Ctl_8 = txdesc->Ctl_8;
	Ctl2_8 = 0;		/* really need to init it to 0, not txdesc->Ctl2_8, it seems */

	hostdesc2 = hostdesc1 + 1;

	/* DON'T simply set Ctl field to 0 here globally,
	 * it needs to maintain a consistent flag status (those are state flags!!),
	 * otherwise it may lead to severe disruption. Only set or reset particular
	 * flags at the exact moment this is needed... */

	/* let chip do RTS/CTS handshaking before sending
	 * in case packet size exceeds threshold */
	if (ieeectl->flags & IEEE80211_TX_RC_USE_RTS_CTS)
	{
		SET_BIT(Ctl2_8, DESC_CTL2_RTS);
	}
	else {
		CLEAR_BIT(Ctl2_8, DESC_CTL2_RTS);
	}

	bitrate = ieee80211_get_tx_rate(adev->ieee, ieeectl)->bitrate;
	rate = ieee80211_get_tx_rate(adev->ieee, ieeectl)->hw_value;

//	logf1(L_ANY, "bitrate=%i, rate(hw_value)=%04X\n", bitrate, rate)

	txdesc->total_length = cpu_to_le16(len);
	hostdesc2->length = cpu_to_le16(len - wlhdr_len);

	/* ACX111 */
	if (IS_ACX111(adev)) {
		/* note that if !txdesc->do_auto, txrate->cur
		 ** has only one nonzero bit */
		txdesc->u.r2.rate111 = cpu_to_le16(rate);
		
		/* WARNING: I was never able to make it work with prism54 AP.
		 * It was falling down to 1Mbit where shortpre is not applicable,
		 * and not working at all at "5,11 basic rates only" setting.
		 * I even didn't see tx packets in radio packet capture.
		 * Disabled for now --vda */
		/*| ((clt->shortpre && clt->cur!=RATE111_1) ? RATE111_SHORTPRE : 0) */
		    
#ifdef TODO_FIGURE_OUT_WHEN_TO_SET_THIS
		/* should add this to rate111 above as necessary */
		|(clt->pbcc511 ? RATE111_PBCC511 : 0)
#endif
		    hostdesc1->length = cpu_to_le16(len);
	}
	/* ACX100 */
	else {
		txdesc->u.r1.rate = rate;

#ifdef TODO_FIGURE_OUT_WHEN_TO_SET_THIS
		if (clt->pbcc511) {
			if (n == RATE100_5 || n == RATE100_11)
				n |= RATE100_PBCC511;
		}
		if (clt->shortpre && (clt->cur != RATE111_1))
			SET_BIT(Ctl_8, DESC_CTL_SHORT_PREAMBLE);	/* set Short Preamble */
#endif

		/* set autodma and reclaim and 1st mpdu */
		SET_BIT(Ctl_8,
			DESC_CTL_AUTODMA | DESC_CTL_RECLAIM |
			DESC_CTL_FIRSTFRAG);

#if ACX_FRAGMENTATION
		/* SET_BIT(Ctl2_8, DESC_CTL2_MORE_FRAG); cannot set it unconditionally,
       needs to be set for all non-last fragments */
#endif

		hostdesc1->length = cpu_to_le16(wlhdr_len);
	}

	/* don't need to clean ack/rts statistics here, already
	 * done on descr cleanup */

	/* clears HOSTOWN and ACXDONE bits, thus telling that the descriptors
	 * are now owned by the acx100; do this as LAST operation */
	CLEAR_BIT(Ctl_8, DESC_CTL_ACXDONE_HOSTOWN);
	/* flush writes before we release hostdesc to the adapter here */
	wmb();
	CLEAR_BIT(hostdesc1->Ctl_16, cpu_to_le16(DESC_CTL_HOSTOWN));
	CLEAR_BIT(hostdesc2->Ctl_16, cpu_to_le16(DESC_CTL_HOSTOWN));

	/* write back modified flags */
	CLEAR_BIT(Ctl2_8, DESC_CTL2_WEP);
	txdesc->Ctl2_8 = Ctl2_8;
	txdesc->Ctl_8 = Ctl_8;
	/* unused: txdesc->tx_time = cpu_to_le32(jiffies); */

	/* flush writes before we tell the adapter that it's its turn now */
	mmiowb();
	write_reg16(adev, IO_ACX_INT_TRIG, INT_TRIG_TXPRC);
	write_flush(adev);

	hostdesc1->skb = skb;

	/* log the packet content AFTER sending it,
	 * in order to not delay sending any further than absolutely needed
	 * Do separate logs for acx100/111 to have human-readable rates */

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
					acx_get_packet_type_string(fc), len, txdesc->u.r1.rate, (Ctl_8
							& DESC_CTL_SHORT_PREAMBLE) ? "(SPr)" : "",
					adev->status);

		if (0 && acx_debug & L_DATA) {
			printk("acx: tx: 802.11 [%d]: ", len);
			acx_dump_bytes(hostdesc1->data, len);
		}
	}

	FN_EXIT0;
}

/*
 * acxpci_l_clean_txdesc
 *
 * This function resets the txdescs' status when the ACX100
 * signals the TX done IRQ (txdescs have been processed), starting with
 * the pool index of the descriptor which we would use next,
 * in order to make sure that we can be as fast as possible
 * in filling new txdescs.
 * Everytime we get called we know where the next packet to be cleaned is.
 */
unsigned int acxpci_clean_txdesc(acx_device_t * adev)
{
	txdesc_t *txdesc;
	txhostdesc_t *hostdesc;
	unsigned finger;
	int num_cleaned;
	u16 r111;
	u8 error, ack_failures, rts_failures, rts_ok, r100;
	struct ieee80211_tx_info *txstatus;

	FN_ENTER;

	if (unlikely(acx_debug & L_DEBUG))
		acxpci_log_txbuffer(adev);

	log(L_BUFT, "acx: tx: cleaning up bufs from %u\n", adev->tx_tail);

	/* We know first descr which is not free yet. We advance it as far
	 ** as we see correct bits set in following descs (if next desc
	 ** is NOT free, we shouldn't advance at all). We know that in
	 ** front of tx_tail may be "holes" with isolated free descs.
	 ** We will catch up when all intermediate descs will be freed also */

	finger = adev->tx_tail;
	num_cleaned = 0;
	while (likely(finger != adev->tx_head)) {
		txdesc = acxpci_get_txdesc(adev, finger);

		/* If we allocated txdesc on tx path but then decided
		 ** to NOT use it, then it will be left as a free "bubble"
		 ** in the "allocated for tx" part of the ring.
		 ** We may meet it on the next ring pass here. */

		/* stop if not marked as "tx finished" and "host owned" */
		if ((txdesc->Ctl_8 & DESC_CTL_ACXDONE_HOSTOWN)
		    != DESC_CTL_ACXDONE_HOSTOWN) {
			if (unlikely(!num_cleaned)) {	/* maybe remove completely */
				log(L_BUFT, "acx: clean_txdesc: tail isn't free. "
				    "tail:%d head:%d\n",
				    adev->tx_tail, adev->tx_head);
			}
			break;
		}

		/* remember desc values... */
		error = txdesc->error;
		ack_failures = txdesc->ack_failures;
		rts_failures = txdesc->rts_failures;
		rts_ok = txdesc->rts_ok;
		r100 = txdesc->u.r1.rate;
		r111 = le16_to_cpu(txdesc->u.r2.rate111);

		// OW TODO 20091116 Compare mem.c
		/* need to check for certain error conditions before we
		 * clean the descriptor: we still need valid descr data here */
		hostdesc = acxpci_get_txhostdesc(adev, txdesc);
		txstatus = IEEE80211_SKB_CB(hostdesc->skb);

        if (!(txstatus->flags & IEEE80211_TX_CTL_NO_ACK) && !(error & 0x30))
			txstatus->flags |= IEEE80211_TX_STAT_ACK;

    	txstatus->status.rates[0].count = ack_failures + 1;

		/* ...and free the desc */
		txdesc->error = 0;
		txdesc->ack_failures = 0;
		txdesc->rts_failures = 0;
		txdesc->rts_ok = 0;
		/* signal host owning it LAST, since ACX already knows that this
		 ** descriptor is finished since it set Ctl_8 accordingly. */
		txdesc->Ctl_8 = DESC_CTL_HOSTOWN;

		adev->tx_free++;
		num_cleaned++;

		/* do error checking, rate handling and logging
		 * AFTER having done the work, it's faster */
		if (unlikely(error))
			acxpcimem_handle_tx_error(adev, error, finger,  txstatus);

		if (IS_ACX111(adev))
			log(L_BUFT,
			    "acx: tx: cleaned %u: !ACK=%u !RTS=%u RTS=%u r111=%04X tx_free=%u\n",
			    finger, ack_failures, rts_failures, rts_ok, r111, adev->tx_free);
		else
			log(L_BUFT,
			    "acx: tx: cleaned %u: !ACK=%u !RTS=%u RTS=%u rate=%u\n",
			    finger, ack_failures, rts_failures, rts_ok, r100);

		/* And finally report upstream */
		ieee80211_tx_status(adev->ieee, hostdesc->skb);

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
void acxpci_clean_txdesc_emergency(acx_device_t * adev)
{
	txdesc_t *txdesc;
	int i;

	FN_ENTER;

	for (i = 0; i < TX_CNT; i++) {
		txdesc = acxpci_get_txdesc(adev, i);

		/* free it */
		txdesc->ack_failures = 0;
		txdesc->rts_failures = 0;
		txdesc->rts_ok = 0;
		txdesc->error = 0;
		txdesc->Ctl_8 = DESC_CTL_HOSTOWN;
	}

	adev->tx_free = TX_CNT;

	FN_EXIT0;
}


static inline txdesc_t *acxpci_get_txdesc(acx_device_t * adev, int index)
{
	return (txdesc_t *) (((u8 *) adev->txdesc_start) +
			     index * adev->txdesc_size);
}

static inline txdesc_t *acxpci_advance_txdesc(acx_device_t * adev, txdesc_t * txdesc,
				       int inc)
{
	return (txdesc_t *) (((u8 *) txdesc) + inc * adev->txdesc_size);
}

static txhostdesc_t *acxpci_get_txhostdesc(acx_device_t * adev, txdesc_t * txdesc)
{
	int index = (u8 *) txdesc - (u8 *) adev->txdesc_start;

	FN_ENTER;

	if (unlikely(ACX_DEBUG && (index % adev->txdesc_size))) {
		printk("acx: bad txdesc ptr %p\n", txdesc);
		return NULL;
	}
	index /= adev->txdesc_size;
	if (unlikely(ACX_DEBUG && (index >= TX_CNT))) {
		printk("acx: bad txdesc ptr %p\n", txdesc);
		return NULL;
	}

	FN_EXIT0;

	return &adev->txhostdesc_start[index * 2];
}

int acx100pci_set_tx_level(acx_device_t * adev, u8 level_dbm)
{
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
	static const u8 dbm2val_maxim[21] = {
		63, 63, 63, 62,
		61, 61, 60, 60,
		59, 58, 57, 55,
		53, 50, 47, 43,
		38, 31, 23, 13,
		0
	};
	static const u8 dbm2val_rfmd[21] = {
		0, 0, 0, 1,
		2, 2, 3, 3,
		4, 5, 6, 8,
		10, 13, 16, 20,
		25, 32, 41, 50,
		63
	};
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
	printk("acx: %s: changing radio power level to %u dBm (%u)\n",
	       wiphy_name(adev->ieee->wiphy), level_dbm, table[level_dbm]);
	acxpci_write_phy_reg(adev, 0x11, table[level_dbm]);
	return OK;
}


/*
 * BOM Irq Handling, Timer
 * ==================================================
 */

static void acxpci_irq_enable(acx_device_t * adev)
{
	FN_ENTER;
	write_reg16(adev, IO_ACX_IRQ_MASK, adev->irq_mask);
	write_reg16(adev, IO_ACX_FEMR, 0x8000);
	write_flush(adev);
	adev->irqs_active = 1;
	FN_EXIT0;
}


static void acxpci_irq_disable(acx_device_t * adev)
{
	FN_ENTER;

	write_reg16(adev, IO_ACX_IRQ_MASK, HOST_INT_MASK_ALL);
	write_reg16(adev, IO_ACX_FEMR, 0x0);
	write_flush(adev);
	adev->irqs_active = 0;

	FN_EXIT0;
}

/* Interrupt handler bottom-half */
#define IRQ_ITERATE 0
void acxpci_irq_work(struct work_struct *work)
{
	acx_device_t *adev = container_of(work, struct acx_device, irq_work);
	int irqreason;
	int irqmasked;
#if IRQ_ITERATE
	unsigned int irqcnt=10;
#endif

	FN_ENTER;

	acx_sem_lock(adev);

	/* OW, 20100611: Iterating and latency:
	 *
	 * IRQ iteration can improve latency, by avoiding waiting for the schedling
	 * of the tx worklet.
	 */
#if IRQ_ITERATE
	while(irqcnt--) {
#endif

	/* We only get an irq-signal for IO_ACX_IRQ_MASK unmasked irq reasons.
	 * However masked irq reasons we still read with IO_ACX_IRQ_REASON or
	 * IO_ACX_IRQ_STATUS_NON_DES
	 */
	irqreason = read_reg16(adev, IO_ACX_IRQ_REASON);
	irqmasked = irqreason & ~adev->irq_mask;
	log(L_IRQ, "acxpci: irqstatus=%04X, irqmasked==%04X\n", irqreason, irqmasked);

#if IRQ_ITERATE
	if (!irqmasked) break;
#endif

		/* HOST_INT_CMD_COMPLETE handling */
		if (irqmasked & HOST_INT_CMD_COMPLETE) {
			log(L_IRQ, "acxpci: got Command_Complete IRQ\n");

			/* save the state for the running issue_cmd() */
			SET_BIT(adev->irq_status, HOST_INT_CMD_COMPLETE);
		}

		/* First report tx status. Just a guess, but it might be better in
		 * AP mode with hostapd, because tx status reporting of previous tx
		 * and new rx receiving are now in sequence. */
		if (irqmasked & HOST_INT_TX_COMPLETE) {
			log(L_IRQ, "acx: got Tx_Complete IRQ\n");
			
			/* The condition on TX_START_CLEAN was removed, because
			 * if was creating a race, sequencing problem in AP mode
			 * during WPA association with different STAs.
			 * 
			 * The result were many WPA assoc retries of the STA,
			 * until assoc finally succeeded. It happens 
			 * sporadically, but still often. I oberserved this 
			 * with a ath5k and acx STA.
			 * 
			 * It manifested as followed:
			 * 1) STA authenticates and associates
			 * 2) And then hostapd reported reception of a 
			 *    Data/PS-poll frame of an unassociated STA
			 * 3) hostapd sends disassoc frame
			 * 4) And then it was looping in retrying this seq, 
			 *    until it succeed 'by accident'
			 *
			 * Removing the TX_START_CLEAN check and always report
			 * directly on the tx status resolved this problem.
			 * Now WPA assoc succeeds directly and robust.			 			 			 			 
			 */			 
			acxpci_clean_txdesc(adev);

			// Restart queue if stopped and enough tx-descr free
			if ((adev->tx_free >= TX_START_QUEUE) && acx_queue_stopped(
					adev->ieee)) {
				log(L_BUF, "acx: tx: wake queue (avail. Tx desc %u)\n",
						adev->tx_free);
				acx_wake_queue(adev->ieee, NULL);
				// Schedule the tx. Doesn't harm. Required in case of irq-iteration.
				ieee80211_queue_work(adev->ieee, &adev->tx_work);
			}

		}

		/* Now do Rx processing */
		if (irqmasked & HOST_INT_RX_COMPLETE) {
			log(L_IRQ, "acxpci: got Rx_Complete IRQ\n");
			acxpci_process_rxdesc(adev);
		}


#if IRQ_ITERATE
		/* Tx new frames, after rx processing
		 * If queue is running. We indirectly use this as indicator,
		 * that tx_free >= TX_START_QUEUE */
		if (!acx_queue_stopped(adev->ieee))
				acx_tx_queue_go(adev);
#endif

		/* HOST_INT_INFO */
		if (irqmasked & HOST_INT_INFO) {
			acxpci_handle_info_irq(adev);
		}

		/* HOST_INT_SCAN_COMPLETE */
		if (irqmasked & HOST_INT_SCAN_COMPLETE) {
			log(L_IRQ, "acxpci: got Scan_Complete IRQ\n");
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

#if IRQ_ITERATE
	}
#endif

	/* Routine to perform blink with range
	 * FIXME: update_link_quality_led is a stub - add proper code and enable this again:
	if (unlikely(adev->led_power == 2))
		update_link_quality_led(adev);
	*/

	// Renable irq-signal again for irqs we are interested in
	write_reg16(adev, IO_ACX_IRQ_MASK, adev->irq_mask);
	write_flush(adev);

	// after_interrupt_jobs: need to be done outside acx_lock (Sleeping required. None atomic)
	if (adev->after_interrupt_jobs){
		acx_after_interrupt_task(adev);
	}

	acx_sem_unlock(adev);

	FN_EXIT0;
	return;

}


static irqreturn_t acxpci_interrupt(int irq, void *dev_id)
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
	log(L_IRQ, "acxpci: irqstatus=%04X, irqmasked=%04X,\n", irqreason, irqmasked);

	if (unlikely(irqreason == 0xffff)) {
		/* 0xffff value hints at missing hardware,
		 * so don't do anything.
		 * Not very clean, but other drivers do the same... */
		log(L_IRQ, "acxpci: irqstatus=FFFF: Device removed?: IRQ_NONE\n");
		goto none;
	}

	/* Our irq-signals are the ones, that were triggered by the IO_ACX_IRQ_MASK
	 * unmasked irqs.
	 */
	if (!irqmasked) {
		/* We are on a shared IRQ line and it wasn't our IRQ */
		log(L_IRQ, "acxpci: irqmasked zero: IRQ_NONE\n");
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
 * acxpci_handle_info_irq
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

static void acxpci_handle_info_irq(acx_device_t * adev)
{
#if ACX_DEBUG
	static const char *const info_type_msg[] = {
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

	info_type = acx_readl(adev->info_area);
	info_status = (info_type >> 16);
	info_type = (u16) info_type;

	/* inform fw that we have read this info message */
	acx_writel(info_type | 0x00010000, adev->info_area);
	write_reg16(adev, IO_ACX_INT_TRIG, INT_TRIG_INFOACK);
	write_flush(adev);

	log(L_CTL, "acx: info_type:%04X info_status:%04X\n", info_type, info_status);

	log(L_IRQ, "acx: got Info IRQ: status %04X type %04X: %s\n",
	    info_status, info_type,
	    info_type_msg[(info_type >= ARRAY_SIZE(info_type_msg)) ?
			  0 : info_type]
	    );
}

void acxpci_set_interrupt_mask(acx_device_t * adev)
{
	if (IS_ACX111(adev)) {
		adev->irq_mask = (u16) ~ (0
					  /* | HOST_INT_RX_DATA        */
					  | HOST_INT_TX_COMPLETE
					  /* | HOST_INT_TX_XFER        */
					  | HOST_INT_RX_COMPLETE
					  /* | HOST_INT_DTIM           */
					  /* | HOST_INT_BEACON         */
					  /* | HOST_INT_TIMER          */
					  /* | HOST_INT_KEY_NOT_FOUND  */
					  | HOST_INT_IV_ICV_FAILURE
					  | HOST_INT_CMD_COMPLETE
					  | HOST_INT_INFO
					  /* | HOST_INT_OVERFLOW       */
					  /* | HOST_INT_PROCESS_ERROR  */
					  | HOST_INT_SCAN_COMPLETE
					  | HOST_INT_FCS_THRESHOLD
					  /* | HOST_INT_UNKNOWN        */
		    );

	} else {
		adev->irq_mask = (u16) ~ (0
					  /* | HOST_INT_RX_DATA        */
					  | HOST_INT_TX_COMPLETE
					  /* | HOST_INT_TX_XFER        */
					  | HOST_INT_RX_COMPLETE
					  /* | HOST_INT_DTIM           */
					  /* | HOST_INT_BEACON         */
					  /* | HOST_INT_TIMER          */
					  /* | HOST_INT_KEY_NOT_FOUND  */
					  /* | HOST_INT_IV_ICV_FAILURE */
					  | HOST_INT_CMD_COMPLETE
					  | HOST_INT_INFO
					  /* | HOST_INT_OVERFLOW       */
					  /* | HOST_INT_PROCESS_ERROR  */
					  | HOST_INT_SCAN_COMPLETE
					  /* | HOST_INT_FCS_THRESHOLD  */
					  /* | HOST_INT_UNKNOWN        */
		    );

	}
}

/*
 * BOM Mac80211 Ops
 * ==================================================
 */

static const struct ieee80211_ops acxpci_hw_ops = {
	.tx = acx_op_tx,
	.conf_tx = acx_conf_tx,
	.add_interface = acx_e_op_add_interface,
	.remove_interface = acx_e_op_remove_interface,
	.start = acxpci_op_start,
	.configure_filter = acx_op_configure_filter,
	.stop = acxpci_op_stop,
	.config = acx_op_config,
	.bss_info_changed = acx_op_bss_info_changed,
	.set_key = acx_op_set_key,
	.get_stats = acx_op_get_stats,
#if CONFIG_ACX_MAC80211_VERSION < KERNEL_VERSION(2, 6, 34)
	.get_tx_stats = acx_e_op_get_tx_stats,
#endif
};


static int acxpci_op_start(struct ieee80211_hw *hw)
{
	acx_device_t *adev = ieee2adev(hw);
	int result = OK;

	FN_ENTER;
	acx_sem_lock(adev);

	adev->initialized = 0;

	/* TODO: pci_set_power_state(pdev, PCI_D0); ? */

	/* ifup device */
	acxpci_up(hw);

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

static void acxpci_op_stop(struct ieee80211_hw *hw)
{
	acx_device_t *adev = ieee2adev(hw);

	FN_ENTER;
	acx_sem_lock(adev);

	acx_stop_queue(adev->ieee, "on ifdown");

	/* disable all IRQs, release shared IRQ handler */
	acxpci_irq_disable(adev);
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

void acxpci_power_led(acx_device_t * adev, int enable)
{
	u16 gpio_pled = IS_ACX111(adev) ? 0x0040 : 0x0800;

	/* A hack. Not moving message rate limiting to adev->xxx
	 * (it's only a debug message after all) */
	static int rate_limit = 0;

	if (rate_limit++ < 3)
		log(L_IOCTL, "acx: Please report in case toggling the power "
		    "LED doesn't work for your card\n");
	if (enable)
		write_reg16(adev, IO_ACX_GPIO_OUT,
			    read_reg16(adev, IO_ACX_GPIO_OUT) & ~gpio_pled);
	else
		write_reg16(adev, IO_ACX_GPIO_OUT,
			    read_reg16(adev, IO_ACX_GPIO_OUT) | gpio_pled);
}

INLINE_IO int acxpci_adev_present(acx_device_t *adev)
{
	/* fast version (accesses the first register, IO_ACX_SOFT_RESET,
	 * which should be safe): */
	return acx_readl(adev->iobase) != 0xffffffff;
}


/*
 * BOM Ioctls
 * ==================================================
 */

#if 0
int
acx111pci_ioctl_info(struct net_device *ndev,
		     struct iw_request_info *info,
		     struct iw_param *vwrq, char *extra)
{
#if ACX_DEBUG > 1
	acx_device_t *adev = ndev2adev(ndev);
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
	acx_interrogate(adev, &memconf, ACX1xx_IE_QUEUE_CONFIG);

	/* get Acx111 Queue Configuration */
	memset(&queueconf, 0, sizeof(queueconf));
	acx_interrogate(adev, &queueconf, ACX1xx_IE_MEMORY_CONFIG_OPTIONS);

	/* get Acx111 Memory Map */
	memset(memmap, 0, sizeof(memmap));
	acx_interrogate(adev, &memmap, ACX1xx_IE_MEMORY_MAP);

	/* get Acx111 Rx Config */
	memset(rxconfig, 0, sizeof(rxconfig));
	acx_interrogate(adev, &rxconfig, ACX1xx_IE_RXCONFIG);

	/* get Acx111 fcs error count */
	memset(fcserror, 0, sizeof(fcserror));
	acx_interrogate(adev, &fcserror, ACX1xx_IE_FCS_ERROR_COUNT);

	/* get Acx111 rate fallback */
	memset(ratefallback, 0, sizeof(ratefallback));
	acx_interrogate(adev, &ratefallback, ACX1xx_IE_RATE_FALLBACK);

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
	       memconf.len, (int)sizeof(memconf),
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
	       queueconf.len, (int)sizeof(queueconf),
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
	       *((u16 *) & memmap[0x02]), (int)sizeof(memmap),
	       *((u32 *) & memmap[0x04]),
	       *((u32 *) & memmap[0x08]),
	       *((u32 *) & memmap[0x0C]),
	       *((u32 *) & memmap[0x10]),
	       *((u32 *) & memmap[0x14]),
	       *((u32 *) & memmap[0x18]),
	       *((u32 *) & memmap[0x1C]),
	       *((u32 *) & memmap[0x20]),
	       *((u32 *) & memmap[0x24]),
	       *((u32 *) & memmap[0x28]),
	       *((u32 *) & memmap[0x2C]),
	       *((u32 *) & memmap[0x30]), adev->iobase, adev->iobase2);

	/* dump Acx111 Rx Config */
	printk("acx: dump rx config:\n"
	       "data read: %d, struct size: %d\n"
	       "rx config: %X\n"
	       "rx filter config: %X\n",
	       *((u16 *) & rxconfig[0x02]), (int)sizeof(rxconfig),
	       *((u16 *) & rxconfig[0x04]), *((u16 *) & rxconfig[0x06]));

	/* dump Acx111 fcs error */
	printk("acx: dump fcserror:\n"
	       "data read: %d, struct size: %d\n"
	       "fcserrors: %X\n",
	       *((u16 *) & fcserror[0x02]), (int)sizeof(fcserror),
	       *((u32 *) & fcserror[0x04]));

	/* dump Acx111 rate fallback */
	printk("acx: dump rate fallback:\n"
	       "data read: %d, struct size: %d\n"
	       "ratefallback: %X\n",
	       *((u16 *) & ratefallback[0x02]), (int)sizeof(ratefallback),
	       *((u8 *) & ratefallback[0x04]));

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
			       rxdesc->Ctl_8,
			       rxdesc->rate, rxdesc->error, rxdesc->SNR);
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
			       (int)sizeof(struct txdesc),
			       txdesc,
			       acx2cpu(txdesc->pNextDesc),
			       acx2cpu(txdesc->AcxMemPtr),
			       acx2cpu(txdesc->HostMemPtr),
			       le16_to_cpu(txdesc->total_length),
			       txdesc->Ctl_8,
			       txdesc->Ctl2_8, txdesc->error,
			       txdesc->u.r1.rate);
			txdesc = acxpci_advance_txdesc(adev, txdesc, 1);
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
int
acx100pci_ioctl_set_phy_amp_bias(struct net_device *ndev,
				 struct iw_request_info *info,
				 struct iw_param *vwrq, char *extra)
{
	acx_device_t *adev = ndev2adev(ndev);
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
		    (gpio_old & 0xf8ff) | ((u16) * extra << 8));
	acx_unlock(adev, flags);

	log(L_DEBUG, "acx: gpio_old: 0x%04X\n", gpio_old);
	printk("acx: %s: PHY power amplifier bias: old:%d, new:%d\n",
	       ndev->name, (gpio_old & 0x0700) >> 8, (unsigned char)*extra);

	acx_sem_unlock(adev);

	return OK;
}
#endif /* 0 */


/*
 * BOM Driver, Module
 * ==================================================
 */

/*
 * acxpci_e_probe
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
static const u16 IO_ACX100[] = {
	0x0000,			/* IO_ACX_SOFT_RESET */

	0x0014,			/* IO_ACX_SLV_MEM_ADDR */
	0x0018,			/* IO_ACX_SLV_MEM_DATA */
	0x001c,			/* IO_ACX_SLV_MEM_CTL */
	0x0020,			/* IO_ACX_SLV_END_CTL */

	0x0034,			/* IO_ACX_FEMR */

	0x007c,			/* IO_ACX_INT_TRIG */
	0x0098,			/* IO_ACX_IRQ_MASK */
	0x00a4,			/* IO_ACX_IRQ_STATUS_NON_DES */
	0x00a8,			/* IO_ACX_IRQ_REASON */
	0x00ac,			/* IO_ACX_IRQ_ACK */
	0x00b0,			/* IO_ACX_HINT_TRIG */

	0x0104,			/* IO_ACX_ENABLE */

	0x0250,			/* IO_ACX_EEPROM_CTL */
	0x0254,			/* IO_ACX_EEPROM_ADDR */
	0x0258,			/* IO_ACX_EEPROM_DATA */
	0x025c,			/* IO_ACX_EEPROM_CFG */

	0x0268,			/* IO_ACX_PHY_ADDR */
	0x026c,			/* IO_ACX_PHY_DATA */
	0x0270,			/* IO_ACX_PHY_CTL */

	0x0290,			/* IO_ACX_GPIO_OE */

	0x0298,			/* IO_ACX_GPIO_OUT */

	0x02a4,			/* IO_ACX_CMD_MAILBOX_OFFS */
	0x02a8,			/* IO_ACX_INFO_MAILBOX_OFFS */
	0x02ac,			/* IO_ACX_EEPROM_INFORMATION */

	0x02d0,			/* IO_ACX_EE_START */
	0x02d4,			/* IO_ACX_SOR_CFG */
	0x02d8			/* IO_ACX_ECPU_CTRL */
};

static const u16 IO_ACX111[] = {
	0x0000,			/* IO_ACX_SOFT_RESET */

	0x0014,			/* IO_ACX_SLV_MEM_ADDR */
	0x0018,			/* IO_ACX_SLV_MEM_DATA */
	0x001c,			/* IO_ACX_SLV_MEM_CTL */
	0x0020,			/* IO_ACX_SLV_END_CTL */

	0x0034,			/* IO_ACX_FEMR */

	0x00b4,			/* IO_ACX_INT_TRIG */
	0x00d4,			/* IO_ACX_IRQ_MASK */
	/* we do mean NON_DES (0xf0), not NON_DES_MASK which is at 0xe0: */
	0x00f0,			/* IO_ACX_IRQ_STATUS_NON_DES */
	0x00e4,			/* IO_ACX_IRQ_REASON */
	0x00e8,			/* IO_ACX_IRQ_ACK */
	0x00ec,			/* IO_ACX_HINT_TRIG */

	0x01d0,			/* IO_ACX_ENABLE */

	0x0338,			/* IO_ACX_EEPROM_CTL */
	0x033c,			/* IO_ACX_EEPROM_ADDR */
	0x0340,			/* IO_ACX_EEPROM_DATA */
	0x0344,			/* IO_ACX_EEPROM_CFG */

	0x0350,			/* IO_ACX_PHY_ADDR */
	0x0354,			/* IO_ACX_PHY_DATA */
	0x0358,			/* IO_ACX_PHY_CTL */

	0x0374,			/* IO_ACX_GPIO_OE */

	0x037c,			/* IO_ACX_GPIO_OUT */

	0x0388,			/* IO_ACX_CMD_MAILBOX_OFFS */
	0x038c,			/* IO_ACX_INFO_MAILBOX_OFFS */
	0x0390,			/* IO_ACX_EEPROM_INFORMATION */

	0x0100,			/* IO_ACX_EE_START */
	0x0104,			/* IO_ACX_SOR_CFG */
	0x0108,			/* IO_ACX_ECPU_CTRL */
};

#ifdef CONFIG_PCI
static int __devinit
acxpci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	acx111_ie_configoption_t co;
	unsigned long mem_region1 = 0;
	unsigned long mem_region2 = 0;
	unsigned long mem_region1_size;
	unsigned long mem_region2_size;
	unsigned long phymem1;
	unsigned long phymem2;
	void *mem1 = NULL;
	void *mem2 = NULL;
	acx_device_t *adev = NULL;
	const char *chip_name;
	int result = -EIO;
	int err;
	u8 chip_type;
	struct ieee80211_hw *ieee;

	FN_ENTER;

	ieee = ieee80211_alloc_hw(sizeof(struct acx_device), &acxpci_hw_ops);
	if (!ieee) {
		printk("acx: could not allocate ieee80211 structure %s\n",
		       pci_name(pdev));
		goto fail_ieee80211_alloc_hw;
	}

	/* Initialize driver private data */
	SET_IEEE80211_DEV(ieee, &pdev->dev);
	ieee->flags &= ~IEEE80211_HW_RX_INCLUDES_FCS;
	/* TODO: mainline doesn't support the following flags yet */
	/*
	 ~IEEE80211_HW_MONITOR_DURING_OPER &
	 ~IEEE80211_HW_WEP_INCLUDE_IV;
	 */

	ieee->wiphy->interface_modes =
			BIT(NL80211_IFTYPE_STATION)	|
			BIT(NL80211_IFTYPE_ADHOC) |
			BIT(NL80211_IFTYPE_AP);
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
	spin_lock_init(&adev->spinlock);	/* initial state: unlocked */
	/* We do not start with downed sem: we want PARANOID_LOCKING to work */
	printk("acx: mutex_init(&adev->mutex); // adev = 0x%px\n", adev);
	mutex_init(&adev->mutex);
	/* since nobody can see new netdev yet, we can as well
	 ** just _presume_ that we're under sem (instead of actually taking it): */
	/* acx_sem_lock(adev); */
	adev->ieee = ieee;
	adev->pdev = pdev;
	adev->bus_dev = &pdev->dev;
	adev->dev_type = DEVTYPE_PCI;

/** Finished with private interface **/

/** begin board specific inits **/
	pci_set_drvdata(pdev, ieee);

	/* Enable the PCI device */
	if (pci_enable_device(pdev)) {
		printk("acx: pci_enable_device() FAILED\n");
		result = -ENODEV;
		goto fail_pci_enable_device;
	}

	/* enable busmastering (required for CardBus) */
	pci_set_master(pdev);


	/* chiptype is u8 but id->driver_data is ulong
	 ** Works for now (possible values are 1 and 2) */
	chip_type = (u8) id->driver_data;
	/* acx100 and acx111 have different PCI memory regions */
	if (chip_type == CHIPTYPE_ACX100) {
		chip_name = "ACX100";
		mem_region1 = PCI_ACX100_REGION1;
		mem_region1_size = PCI_ACX100_REGION1_SIZE;

		mem_region2 = PCI_ACX100_REGION2;
		mem_region2_size = PCI_ACX100_REGION2_SIZE;
	} else if (chip_type == CHIPTYPE_ACX111) {
		chip_name = "ACX111";
		mem_region1 = PCI_ACX111_REGION1;
		mem_region1_size = PCI_ACX111_REGION1_SIZE;

		mem_region2 = PCI_ACX111_REGION2;
		mem_region2_size = PCI_ACX111_REGION2_SIZE;
	} else {
		printk("acx: unknown chip type 0x%04X\n", chip_type);
		goto fail_unknown_chiptype;
	}

	/* Figure out our resources
	 *
	 * Request our PCI IO regions
	 */
	err = pci_request_region(pdev, mem_region1, "acx_1");
	if (err) {
		printk(KERN_WARNING "acx: pci_request_region (1/2) FAILED!"
			"No cardbus support in kernel?\n");
		goto fail_request_mem_region1;
	}

	phymem1 = pci_resource_start(pdev, mem_region1);

	err = pci_request_region(pdev, mem_region2, "acx_2");
	if (err) {
		printk(KERN_WARNING "acx: pci_request_region (2/2) FAILED!\n");
		goto fail_request_mem_region2;
	}

	phymem2 = pci_resource_start(pdev, mem_region2);

	/*
	 * We got them? Map them!
	 *
	 * We pass 0 as the third argument to pci_iomap(): it will map the full
	 * region in this case, which is what we want.
	 */

	mem1 = pci_iomap(pdev, mem_region1, 0);
	if (!mem1) {
		printk(KERN_WARNING "acx: ioremap() FAILED\n");
		goto fail_iomap1;
	}

	mem2 = pci_iomap(pdev, mem_region2, 0);
	if (!mem2) {
		printk(KERN_WARNING "acx: ioremap() #2 FAILED\n");
		goto fail_iomap2;
	}

	printk("acx: found an %s-based wireless network card at %s, irq:%d, "
	       "phymem1:0x%lX, phymem2:0x%lX, mem1:0x%p, mem1_size:%ld, "
	       "mem2:0x%p, mem2_size:%ld\n",
	       chip_name, pci_name(pdev), pdev->irq, phymem1, phymem2,
	       mem1, mem_region1_size, mem2, mem_region2_size);
	log(L_ANY, "acx: the initial debug setting is 0x%04X\n", acx_debug);
	adev->chip_type = chip_type;
	adev->chip_name = chip_name;
	adev->io = (CHIPTYPE_ACX100 == chip_type) ? IO_ACX100 : IO_ACX111;
	adev->membase = phymem1;
	adev->iobase = mem1;
	adev->membase2 = phymem2;
	adev->iobase2 = mem2;
	adev->irq = pdev->irq;

	if (adev->irq == 0) {
		printk("acx: can't use IRQ 0\n");
		goto fail_no_irq;
	}

	/* request shared IRQ handler */
	if (request_irq(adev->irq, acxpci_interrupt, IRQF_SHARED, KBUILD_MODNAME,
			adev)) {
		printk("acx: %s: request_irq FAILED\n", wiphy_name(adev->ieee->wiphy));
		result = -EAGAIN;
		goto fail_request_irq;
	}
	log(L_IRQ | L_INIT, "acx: using IRQ %d: OK\n", pdev->irq);

	// Acx irqs shall be off and are enabled later in acxpci_s_up
	acxpci_irq_disable(adev);

	/* to find crashes due to weird driver access
	 * to unconfigured interface (ifup) */
	adev->mgmt_timer.function = (void (*)(unsigned long))0x0000dead;


#ifdef NONESSENTIAL_FEATURES
	acx_show_card_eeprom_id(adev);
#endif /* NONESSENTIAL_FEATURES */


	/* PCI setup is finished, now start initializing the card */
	// -----
	
	acx_init_task_scheduler(adev);

	// Mac80211 Tx_queue
	INIT_WORK(&adev->tx_work, acx_tx_work);
	skb_queue_head_init(&adev->tx_queue);

	/* NB: read_reg() reads may return bogus data before reset_dev(),
	 * since the firmware which directly controls large parts of the I/O
	 * registers isn't initialized yet.
	 * acx100 seems to be more affected than acx111 */
	if (OK != acxpci_reset_dev(adev))
		goto fail_reset_dev;

	if (IS_ACX100(adev)) {
		/* ACX100: configopt struct in cmd mailbox - directly after reset */
		memcpy_fromio(&co, adev->cmd_area, sizeof(co));
	}

	if (OK != acx_init_mac(adev))
		goto fail_init_mac;

	if (IS_ACX111(adev)) {
		/* ACX111: configopt struct needs to be queried after full init */
		acx_interrogate(adev, &co, ACX111_IE_CONFIG_OPTIONS);
	}

	/* TODO: merge them into one function, they are called just once and are the same for pci & usb */
	if (OK != acxpci_read_eeprom_byte(adev, 0x05, &adev->eeprom_version))
		goto fail_read_eeprom_byte;

	acx_parse_configoption(adev, &co);
	acx_set_defaults(adev); // TODO OW may put this after acx_display_hardware_details(adev);
	acx_get_firmware_version(adev);	/* needs to be after acx_s_init_mac() */
	acx_display_hardware_details(adev);

	/* Register the card, AFTER everything else has been set up,
	 * since otherwise an ioctl could step on our feet due to
	 * firmware operations happening in parallel or uninitialized data */

	if (acx_proc_register_entries(ieee, 0) != OK)
		goto fail_proc_register_entries;

	printk("acx: net device %s, driver compiled "
	       "against wireless extensions %d and Linux %s\n",
	       wiphy_name(adev->ieee->wiphy), WIRELESS_EXT, UTS_RELEASE);

	MAC_COPY(adev->ieee->wiphy->perm_addr, adev->dev_addr);

/** done with board specific setup **/

	/* need to be able to restore PCI state after a suspend */
#ifdef CONFIG_PM
	pci_save_state(pdev);
#endif

	err = acx_setup_modes(adev);
	if (err) {
	printk("acx: can't setup hwmode\n");
		goto fail_setup_modes;
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

	// TODO FIXME OW 20100507
	// Check if reverse doing is correct. e.g. if alloc failed, no dealloc is required !!
	// => See vlynq probe

	// err = ieee80211_register_hw(ieee);
	fail_ieee80211_register_hw:
		ieee80211_unregister_hw(ieee);

	// err = acx_setup_modes(adev)
	fail_setup_modes:

	// acx_proc_register_entries(ieee, 0)
	fail_proc_register_entries:
		acx_proc_unregister_entries(ieee, 0);

	// acxpci_read_eeprom_byte(adev, 0x05, &adev->eeprom_version)
	fail_read_eeprom_byte:

	// acx_s_init_mac(adev)
	fail_init_mac:

	// acxpci_s_reset_dev(adev)
	fail_reset_dev:

	// request_irq(adev->irq, acxpci_i_interrupt, IRQF_SHARED, KBUILD_MODNAME,
	fail_request_irq:
		free_irq(adev->irq, adev);

	fail_no_irq:

	// pci_iomap(pdev, mem_region2, 0)
	fail_iomap2:
		pci_iounmap(pdev, mem2);

	// pci_iomap(pdev, mem_region1, 0)
	fail_iomap1:
		pci_iounmap(pdev, mem1);

	// 	err = pci_request_region(pdev, mem_region2, "acx_2");
	fail_request_mem_region2:
		pci_release_region(pdev, mem_region2);

	// err = pci_request_region(pdev, mem_region1, "acx_1");
	fail_request_mem_region1:
		pci_release_region(pdev, mem_region1);

	fail_unknown_chiptype:

	// pci_enable_device(pdev)
	fail_pci_enable_device:
		pci_disable_device(pdev);
		pci_set_drvdata(pdev, NULL);

	// OW TODO Check if OK for PM
#ifdef CONFIG_PM
	pci_set_power_state(pdev, PCI_D3hot);
#endif

	// ieee80211_alloc_hw
	fail_ieee80211_alloc_hw:
		ieee80211_free_hw(ieee);

	done:
		FN_EXIT1(result);
		return result;
}


/*
 * acxpci_e_remove
 *
 * Shut device down (if not hot unplugged)
 * and deallocate PCI resources for the acx chip.
 *
 * pdev - ptr to PCI device structure containing info about pci configuration
 */
static void __devexit acxpci_remove(struct pci_dev *pdev)
{
	struct ieee80211_hw *hw = (struct ieee80211_hw *)pci_get_drvdata(pdev);
	acx_device_t *adev = ieee2adev(hw);
	unsigned long mem_region1, mem_region2;

	FN_ENTER;

	if (!hw) {
		log(L_DEBUG, "acx: %s: card is unused. Skipping any release code\n",
		    __func__);
		goto end_no_lock;
	}

	// Unregister ieee80211 device
	log(L_INIT, "acxpci: removing device %s\n", wiphy_name(adev->ieee->wiphy));
	ieee80211_unregister_hw(adev->ieee);
	CLEAR_BIT(adev->dev_state_mask, ACX_STATE_IFACE_UP);

	/* If device wasn't hot unplugged... */
	if (acxpci_adev_present(adev)) {

		/* Disable both Tx and Rx to shut radio down properly */
		if (adev->initialized) {
			acx_issue_cmd(adev, ACX1xx_CMD_DISABLE_TX, NULL, 0);
			acx_issue_cmd(adev, ACX1xx_CMD_DISABLE_RX, NULL, 0);
			adev->initialized = 0;
		}

#ifdef REDUNDANT
		/* put the eCPU to sleep to save power
		 * Halting is not possible currently,
		 * since not supported by all firmware versions */
		acx_issue_cmd(adev, ACX100_CMD_SLEEP, NULL, 0);
#endif
		/* disable power LED to save power :-) */
		log(L_INIT, "acx: switching off power LED to save power\n");
		acxpci_power_led(adev, 0);
		/* stop our eCPU */
		if (IS_ACX111(adev)) {
			/* FIXME: does this actually keep halting the eCPU?
			 * I don't think so...
			 */
			acxpci_reset_mac(adev);
		} else {
			u16 temp;
			/* halt eCPU */
			temp = read_reg16(adev, IO_ACX_ECPU_CTRL) | 0x1;
			write_reg16(adev, IO_ACX_ECPU_CTRL, temp);
			write_flush(adev);
		}

	}

	// Proc
	acx_proc_unregister_entries(adev->ieee, 0);

	// IRQs
	acxpci_irq_disable(adev);
	synchronize_irq(adev->irq);
	free_irq(adev->irq, adev);

	// Mem regions
	if (IS_ACX100(adev)) {
		mem_region1 = PCI_ACX100_REGION1;
		mem_region2 = PCI_ACX100_REGION2;
	} else {
		mem_region1 = PCI_ACX111_REGION1;
		mem_region2 = PCI_ACX111_REGION2;
	}

	/* finally, clean up PCI bus state */
	acxpci_delete_dma_regions(adev);
	if (adev->iobase)
		iounmap(adev->iobase);
	if (adev->iobase2)
		iounmap(adev->iobase2);
	release_mem_region(pci_resource_start(pdev, mem_region1),
			   pci_resource_len(pdev, mem_region1));
	release_mem_region(pci_resource_start(pdev, mem_region2),
			   pci_resource_len(pdev, mem_region2));
	pci_disable_device(pdev);

	/* remove dev registration */
	pci_set_drvdata(pdev, NULL);

	/* Free netdev (quite late,
	 * since otherwise we might get caught off-guard
	 * by a netdev timeout handler execution
	 * expecting to see a working dev...) */
	ieee80211_free_hw(adev->ieee);

	/* put device into ACPI D3 mode (shutdown) */
#ifdef CONFIG_PM
	pci_set_power_state(pdev, PCI_D3hot);
#endif

	end_no_lock:
	FN_EXIT0;
}


/***********************************************************************
** TODO: PM code needs to be fixed / debugged / tested.
*/
#ifdef CONFIG_PM
static int
acxpci_e_suspend(struct pci_dev *pdev, pm_message_t state)
{
	struct ieee80211_hw *hw = pci_get_drvdata(pdev);
	acx_device_t *adev;

	FN_ENTER;
	printk("acx: suspend handler is experimental!\n");
	printk("acx: sus: dev %p\n", hw);

/*	if (!netif_running(ndev))
		goto end;
*/
	adev = ieee2adev(hw);
	printk("acx: sus: adev %p\n", adev);

	acx_sem_lock(adev);

	ieee80211_unregister_hw(hw);	/* this one cannot sleep */
	// OW 20100603 FIXME acxpci_s_down(hw);
	/* down() does not set it to 0xffff, but here we really want that */
	write_reg16(adev, IO_ACX_IRQ_MASK, 0xffff);
	write_reg16(adev, IO_ACX_FEMR, 0x0);
	acxpci_delete_dma_regions(adev);
	pci_save_state(pdev);
	pci_set_power_state(pdev, PCI_D3hot);

	acx_sem_unlock(adev);
	FN_EXIT0;
	return OK;
}


static int acxpci_e_resume(struct pci_dev *pdev)
{
	struct ieee80211_hw *hw = pci_get_drvdata(pdev);
	acx_device_t *adev;

	FN_ENTER;

	printk("acx: resume handler is experimental!\n");
	printk("acx: rsm: got dev %p\n", hw);


	adev = ieee2adev(hw);
	printk("acx: rsm: got adev %p\n", adev);

	acx_sem_lock(adev);

	pci_set_power_state(pdev, PCI_D0);
	printk("acx: rsm: power state PCI_D0 set\n");
	pci_restore_state(pdev);
	printk("acx: rsm: PCI state restored\n");

	if (OK != acxpci_reset_dev(adev))
		goto end_unlock;
	printk("acx: rsm: device reset done\n");
	if (OK != acx_init_mac(adev))
		goto end_unlock;
	printk("acx: rsm: init MAC done\n");

	acxpci_up(hw);
	printk("acx: rsm: acx up done\n");

	/* now even reload all card parameters as they were before suspend,
	 * and possibly be back in the network again already :-) */
	if (ACX_STATE_IFACE_UP & adev->dev_state_mask) {
		adev->set_mask = GETSET_ALL;
		acx_update_card_settings(adev);
		printk("acx: rsm: settings updated\n");
	}
	ieee80211_register_hw(hw);
	printk("acx: rsm: device attached\n");

      end_unlock:
	acx_sem_unlock(adev);
	/* we need to return OK here anyway, right? */
	FN_EXIT0;
	return OK;
}
#endif /* CONFIG_PM */
#endif /* CONFIG_PCI */

/*
 * Data for init_module/cleanup_module
 */
#ifdef CONFIG_PCI
static const struct pci_device_id acxpci_id_tbl[] __devinitdata = {
	{
	 .vendor = PCI_VENDOR_ID_TI,
	 .device = PCI_DEVICE_ID_TI_TNETW1100A,
	 .subvendor = PCI_ANY_ID,
	 .subdevice = PCI_ANY_ID,
	 .driver_data = CHIPTYPE_ACX100,
	 },
	{
	 .vendor = PCI_VENDOR_ID_TI,
	 .device = PCI_DEVICE_ID_TI_TNETW1100B,
	 .subvendor = PCI_ANY_ID,
	 .subdevice = PCI_ANY_ID,
	 .driver_data = CHIPTYPE_ACX100,
	 },
	{
	 .vendor = PCI_VENDOR_ID_TI,
	 .device = PCI_DEVICE_ID_TI_TNETW1130,
	 .subvendor = PCI_ANY_ID,
	 .subdevice = PCI_ANY_ID,
	 .driver_data = CHIPTYPE_ACX111,
	 },
	{
	 .vendor = 0,
	 .device = 0,
	 .subvendor = 0,
	 .subdevice = 0,
	 .driver_data = 0,
	 }
};

MODULE_DEVICE_TABLE(pci, acxpci_id_tbl);

static struct pci_driver
 acxpci_drv_id = {
	.name = "acx_pci",
	.id_table = acxpci_id_tbl,
	.probe = acxpci_probe,
	.remove = __devexit_p(acxpci_remove),
#ifdef CONFIG_PM
	.suspend = acxpci_e_suspend,
	.resume = acxpci_e_resume
#endif /* CONFIG_PM */
};
#endif /* CONFIG_PCI */


/*
 * VLYNQ support
 */

#ifdef CONFIG_VLYNQ
struct vlynq_reg_config {
	u32 offset;
	u32 value;
};

struct vlynq_known {
	u32 chip_id;
	char name[32];
	struct vlynq_mapping rx_mapping[4];
	int irq;
	int irq_type;
	int num_regs;
	struct vlynq_reg_config regs[10];
};

#define CHIP_TNETW1130 0x00000009
#define CHIP_TNETW1350 0x00000029

static struct vlynq_known vlynq_known_devices[] = {
	{
		.chip_id = CHIP_TNETW1130, .name = "TI TNETW1130",
		.rx_mapping = {
			{ .size = 0x22000, .offset = 0xf0000000 },
			{ .size = 0x40000, .offset = 0xc0000000 },
			{ .size = 0x0, .offset = 0x0 },
			{ .size = 0x0, .offset = 0x0 },
		},
		.irq = 0,
		.irq_type = IRQ_TYPE_EDGE_RISING,
		.num_regs = 5,
		.regs = {
			{
				.offset = 0x790,
				.value = (0xd0000000 - PHYS_OFFSET)
			},
			{
				.offset = 0x794,
				.value = (0xd0000000 - PHYS_OFFSET)
			},
			{ .offset = 0x740, .value = 0 },
			{ .offset = 0x744, .value = 0x00010000 },
			{ .offset = 0x764, .value = 0x00010000 },
		},
	},
	{
		.chip_id = CHIP_TNETW1350, .name = "TI TNETW1350",
		.rx_mapping = {
			{ .size = 0x100000, .offset = 0x00300000 },
			{ .size = 0x80000, .offset = 0x00000000 },
			{ .size = 0x0, .offset = 0x0 },
			{ .size = 0x0, .offset = 0x0 },
		},
		.irq = 0,
		.irq_type = IRQ_TYPE_EDGE_RISING,
		.num_regs = 5,
		.regs = {
			{
				.offset = 0x790,
				.value = (0x60000000 - PHYS_OFFSET)
			},
			{
				.offset = 0x794,
				.value = (0x60000000 - PHYS_OFFSET)
			},
			{ .offset = 0x740, .value = 0 },
			{ .offset = 0x744, .value = 0x00010000 },
			{ .offset = 0x764, .value = 0x00010000 },
		},
	},
};

static struct vlynq_device_id acx_vlynq_id[] = {
	{ CHIP_TNETW1130, vlynq_div_auto, 0 },
	// TNETW1350 not supported by the acx driver, therefore don't claim it anymore
	// { CHIP_TNETW1350, vlynq_div_auto, 1 },
	{ 0, 0, 0 },
};


static __devinit int vlynq_probe(struct vlynq_device *vdev,
				 struct vlynq_device_id *id)
{
	int result = -EIO, i;
	u32 addr;
	struct ieee80211_hw *ieee;
	acx_device_t *adev = NULL;
	acx111_ie_configoption_t co;
	struct vlynq_mapping mapping[4] = { { 0, }, };
	struct vlynq_known *match = NULL;

	FN_ENTER;

	ieee = ieee80211_alloc_hw(sizeof(struct acx_device), &acxpci_hw_ops);
	if (!ieee) {
		printk("acx: could not allocate ieee80211 structure %s\n",
		       dev_name(&vdev->dev));
		goto fail_vlynq_ieee80211_alloc_hw;
	}

	// Initialize driver private data
	SET_IEEE80211_DEV(ieee, &vdev->dev);
	ieee->flags &= ~IEEE80211_HW_RX_INCLUDES_FCS;

	ieee->wiphy->interface_modes =
			BIT(NL80211_IFTYPE_STATION)	|
			BIT(NL80211_IFTYPE_ADHOC) |
			BIT(NL80211_IFTYPE_AP);
	ieee->queues = 1;

	// We base signal quality on winlevel approach of previous driver
	// TODO OW 20100615 This should into a common init code
	ieee->flags |= IEEE80211_HW_SIGNAL_UNSPEC;
	ieee->max_signal = 100;

	adev = ieee2adev(ieee);

	memset(adev, 0, sizeof(*adev));
	/** Set up our private interface **/
	spin_lock_init(&adev->spinlock);	/* initial state: unlocked */
	/* We do not start with downed sem: we want PARANOID_LOCKING to work */
	mutex_init(&adev->mutex);
	/* since nobody can see new netdev yet, we can as well
	 ** just _presume_ that we're under sem (instead of actually taking it): */
	/* acx_sem_lock(adev); */
	adev->ieee = ieee;
	adev->vdev = vdev;
	adev->bus_dev = &vdev->dev;
	adev->dev_type = DEVTYPE_PCI;

	// Finished with private interface

	// Begin board specific inits
	result = vlynq_enable_device(vdev);
	if (result)
		goto fail_vlynq_enable_device;

	vlynq_set_drvdata(vdev, ieee);

	match = &vlynq_known_devices[id->driver_data];

	if (!match) {
		result = -ENODEV;
		goto fail_vlynq_known_devices;
	}

	mapping[0].offset = ARCH_PFN_OFFSET << PAGE_SHIFT;
	mapping[0].size = 0x02000000;
	vlynq_set_local_mapping(vdev, vdev->mem_start, mapping);
	vlynq_set_remote_mapping(vdev, 0, match->rx_mapping);

	set_irq_type(vlynq_virq_to_irq(vdev, match->irq), match->irq_type);

	addr = (u32)ioremap(vdev->mem_start, 0x1000);
	if (!addr) {
		printk(KERN_ERR "acx: %s: failed to remap io memory\n",
		       dev_name(&vdev->dev));
		result = -ENXIO;
		goto fail_vlynq_ioremap1;
	}

	for (i = 0; i < match->num_regs; i++)
		iowrite32(match->regs[i].value,
			  (u32 *)(addr + match->regs[i].offset));

	iounmap((void *)addr);

	if (!request_mem_region(vdev->mem_start, vdev->mem_end - vdev->mem_start, "acx")) {
		printk("acx: cannot reserve VLYNQ memory region\n");
		goto fail_vlynq_request_mem_region;
	}

	adev->iobase = ioremap(vdev->mem_start, vdev->mem_end - vdev->mem_start);
	if (!adev->iobase) {
		printk("acx: ioremap() FAILED\n");
		goto fail_vlynq_ioremap2;
	}
	adev->iobase2 = adev->iobase + match->rx_mapping[0].size;
	adev->chip_type = CHIPTYPE_ACX111;
	adev->chip_name = match->name;
	adev->io = IO_ACX111;
	adev->irq = vlynq_virq_to_irq(vdev, match->irq);

	printk("acx: found %s-based wireless network card at %s, irq:%d, "
	       "phymem:0x%x, mem:0x%p\n",
	       match->name, dev_name(&vdev->dev), adev->irq,
	       vdev->mem_start, adev->iobase);
	log(L_ANY, "acx: the initial debug setting is 0x%04X\n", acx_debug);

	if (0 == adev->irq) {
		printk("acx: can't use IRQ 0\n");
		goto fail_vlynq_irq;
	}

	/* request shared IRQ handler */
	if (request_irq
	    (adev->irq, acxpci_interrupt, IRQF_SHARED, KBUILD_MODNAME, adev)) {
		printk("acx: %s: request_irq FAILED\n", wiphy_name(adev->ieee->wiphy));
		result = -EAGAIN;
		goto done;
	}
	log(L_IRQ | L_INIT, "acx: using IRQ %d\n", adev->irq);

	// Acx irqs shall be off and are enabled later in acxpci_s_up
	acxpci_irq_disable(adev);

	/* to find crashes due to weird driver access
	 * to unconfigured interface (ifup) */
	adev->mgmt_timer.function = (void (*)(unsigned long))0x0000dead;

	/* PCI setup is finished, now start initializing the card */
	// -----

	acx_init_task_scheduler(adev);

	// Mac80211 Tx_queue
	INIT_WORK(&adev->tx_work, acx_tx_work);
	skb_queue_head_init(&adev->tx_queue);

	/* NB: read_reg() reads may return bogus data before reset_dev(),
	 * since the firmware which directly controls large parts of the I/O
	 * registers isn't initialized yet.
	 * acx100 seems to be more affected than acx111 */
	if (OK != acxpci_reset_dev(adev))
		goto fail_vlynq_reset_dev;

	if (OK != acx_init_mac(adev))
		goto fail_vlynq_init_mac;

	acx_interrogate(adev, &co, ACX111_IE_CONFIG_OPTIONS);
	/* TODO: merge them into one function, they are called just once and are the same for pci & usb */
	if (OK != acxpci_read_eeprom_byte(adev, 0x05, &adev->eeprom_version))
		goto fail_vlynq_read_eeprom_version;

	acx_parse_configoption(adev, &co);
	acx_set_defaults(adev);
	acx_get_firmware_version(adev);	/* needs to be after acx_s_init_mac() */
	acx_display_hardware_details(adev);

	/* Register the card, AFTER everything else has been set up,
	 * since otherwise an ioctl could step on our feet due to
	 * firmware operations happening in parallel or uninitialized data */

	if (acx_proc_register_entries(ieee, 0) != OK)
		goto fail_vlynq_proc_register_entries;

	/* Now we have our device, so make sure the kernel doesn't try
	 * to send packets even though we're not associated to a network yet */

	/* after register_netdev() userspace may start working with dev
	 * (in particular, on other CPUs), we only need to up the sem */
	/* acx_sem_unlock(adev); */

	printk("acx: net device %s, driver compiled "
	       "against wireless extensions %d and Linux %s\n",
	       wiphy_name(adev->ieee->wiphy), WIRELESS_EXT, UTS_RELEASE);

	MAC_COPY(adev->ieee->wiphy->perm_addr, adev->dev_addr);

/** done with board specific setup **/

	result = acx_setup_modes(adev);
	if (result) {
	printk("acx: can't register hwmode\n");
		goto fail_vlynq_setup_modes;
	}

	result = ieee80211_register_hw(adev->ieee);
	if (OK != result) {
		printk("acx: ieee80211_register_hw() FAILED: %d\n", result);
		goto fail_vlynq_ieee80211_register_hw;
	}
#if CMD_DISCOVERY
	great_inquisitor(adev);
#endif

	result = OK;
	goto done;

	/* error paths: undo everything in reverse order... */

	fail_vlynq_ieee80211_register_hw:

	fail_vlynq_setup_modes:

	fail_vlynq_proc_register_entries:
		acx_proc_unregister_entries(ieee, 0);

    fail_vlynq_read_eeprom_version:

	fail_vlynq_init_mac:

    fail_vlynq_reset_dev:

	fail_vlynq_irq:
      iounmap(adev->iobase);

    fail_vlynq_ioremap2:
		release_mem_region(vdev->mem_start, vdev->mem_end - vdev->mem_start);

    fail_vlynq_request_mem_region:

    fail_vlynq_ioremap1:

    fail_vlynq_known_devices:
		vlynq_disable_device(vdev);
		vlynq_set_drvdata(vdev, NULL);

    fail_vlynq_enable_device:
		ieee80211_free_hw(ieee);

    fail_vlynq_ieee80211_alloc_hw:

	done:
		FN_EXIT1(result);

	return result;
}

static void vlynq_remove(struct vlynq_device *vdev)
{
	struct ieee80211_hw *hw = vlynq_get_drvdata(vdev);
	acx_device_t *adev = ieee2adev(hw);
	FN_ENTER;

	if (!hw) {
		log(L_DEBUG, "acx: %s: card is unused. Skipping any release code\n",
		    __func__);
		goto end_no_lock;
	}


	// Unregister ieee80211 device
	log(L_INIT, "acxpci: removing device %s\n", wiphy_name(adev->ieee->wiphy));
	ieee80211_unregister_hw(adev->ieee);
	CLEAR_BIT(adev->dev_state_mask, ACX_STATE_IFACE_UP);

	/* If device wasn't hot unplugged... */
	if (acxpci_adev_present(adev)) {

		/* disable both Tx and Rx to shut radio down properly */
		if (adev->initialized) {
			acx_issue_cmd(adev, ACX1xx_CMD_DISABLE_TX, NULL, 0);
			acx_issue_cmd(adev, ACX1xx_CMD_DISABLE_RX, NULL, 0);
			adev->initialized = 0;
		}
		/* disable power LED to save power :-) */
		log(L_INIT, "acx: switching off power LED to save power\n");
		acxpci_power_led(adev, 0);

		/* stop our eCPU */
		// OW PCI still does something here (although also need to be reviewed).
	}

	// Proc
	acx_proc_unregister_entries(adev->ieee, 0);

	// IRQs
	acxpci_irq_disable(adev);
	synchronize_irq(adev->irq);
	free_irq(adev->irq, adev);

	/* finally, clean up PCI bus state */
	acxpci_delete_dma_regions(adev);
	if (adev->iobase)
		iounmap(adev->iobase);
	if (adev->iobase2)
		iounmap(adev->iobase2);
	release_mem_region(vdev->mem_start, vdev->mem_end - vdev->mem_start);

	vlynq_disable_device(vdev);

	/* remove dev registration */
	vlynq_set_drvdata(vdev, NULL);

	/* Free netdev (quite late,
	 * since otherwise we might get caught off-guard
	 * by a netdev timeout handler execution
	 * expecting to see a working dev...) */
	ieee80211_free_hw(adev->ieee);

	end_no_lock:
	FN_EXIT0;
}

static struct vlynq_driver vlynq_acx = {
	.name = "acx_vlynq",
	.id_table = acx_vlynq_id,
	.probe = vlynq_probe,
	.remove = __devexit_p(vlynq_remove),
};
#endif /* CONFIG_VLYNQ */


/*
 * acxpci_e_init_module
 *
 * Module initialization routine, called once at module load time
 */
int __init acxpci_init_module(void)
{
	int res;

	FN_ENTER;

	printk(KERN_EMERG);

#if (ACX_IO_WIDTH==32)
	log(L_INIT, "acx: compiled to use 32bit I/O access. "
	       "I/O timing issues might occur, such as "
	       "non-working firmware upload. Report them\n");
#else
	log(L_INIT, "acx: compiled to use 16bit I/O access only "
	       "(compatibility mode)\n");
#endif

#ifdef __LITTLE_ENDIAN
#define ENDIANNESS_STRING "running on a little-endian CPU\n"
#else
#define ENDIANNESS_STRING "running on a BIG-ENDIAN CPU\n"
#endif
	log(L_INIT,
	    "acx: " ENDIANNESS_STRING
	    " PCI/VLYNQ module initialized, "
	    "waiting for cards to probe...\n");

#if defined(CONFIG_PCI)
	res = pci_register_driver(&acxpci_drv_id);
#elif defined(CONFIG_VLYNQ)
	res = vlynq_register_driver(&vlynq_acx);
#endif

	if (res) {
		printk(KERN_ERR "acx_pci: can't register pci/vlynq driver\n");
	}

	FN_EXIT1(res);
	return res;
}


/*
 * acxpci_e_cleanup_module
 *
 * Called at module unload time. This is our last chance to
 * clean up after ourselves.
 */
void __exit acxpci_cleanup_module(void)
{
	FN_ENTER;

#if defined(CONFIG_PCI)
	pci_unregister_driver(&acxpci_drv_id);
#elif defined(CONFIG_VLYNQ)
	vlynq_unregister_driver(&vlynq_acx);
#endif
	log(L_INIT,
	    "acxpci: PCI module unloaded\n");
	FN_EXIT0;
}
