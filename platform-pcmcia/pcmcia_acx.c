/*
 * WLAN (TI TNETW1100B) support for PCMCIA cards.
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file COPYING in the main directory of this archive for
 * more details.
 *
 * Copyright (c) 2008 Denis Grigoriev <dgreenday at gmail.com>
 * Copyright (c) 2010 Vasily Khoruzhick <anarsoul at gmail.com>
 * Copyright (c) 2010 Martin Röder <m-roeder at gmx.net>
 */


#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>
#include <linux/io.h>

#include <pcmcia/cs_types.h>
#include <pcmcia/cs.h>
#include <pcmcia/cistpl.h>
#include <pcmcia/cisreg.h>
#include <pcmcia/ds.h>

static struct pcmcia_device_id acx_ids[] = {
	PCMCIA_DEVICE_MANF_CARD(0x0097, 0x8402),
	PCMCIA_DEVICE_MANF_CARD(0x0250, 0xb001),
	PCMCIA_DEVICE_NULL,
};
MODULE_DEVICE_TABLE(pcmcia, acx_ids);

static struct resource acx_resources[] = {
	[0] = {
		.start	= -1,
		.end	= -1,
		.flags	= IORESOURCE_MEM,
	},
	[1] = {
		.start	= -1,
		.end	= -1,
		.flags	= IORESOURCE_IRQ,
	},
};

static void pcmcia_wlan_e_release(struct device *dev) {
}

static struct platform_device acx_device = {
	.name	= "acx-mem",
	.dev	= {
		.release = &pcmcia_wlan_e_release
	},
	.num_resources	= ARRAY_SIZE(acx_resources),
	.resource	= acx_resources,
};

static int __init pcmcia_wlan_init(void)
{
	int res;
	printk(KERN_INFO "pcmcia_wlan_init: acx-mem platform_device_register\n");
	res = platform_device_register(&acx_device);
	if (acx_device.dev.driver == NULL) {
		printk(KERN_ERR "%s: acx-mem driver is not loaded\n", __func__);
		platform_device_unregister(&acx_device);
		return -EINVAL;
	}

	if (res != 0) {
		printk(KERN_ERR "%s: acx-mem platform_device_register: failed\n",
			__func__);
	}

	return res;
}

static void __exit  pcmcia_wlan_exit(void)
{
	platform_device_unregister(&acx_device);
}

static int acx_cs_probe(struct pcmcia_device *pdev)
{
	tuple_t tuple;
	cisparse_t parse;
	u_char buf[64];
	win_req_t req;
	memreq_t map;
	int status;

	tuple.Attributes = 0;
	tuple.TupleData = (cisdata_t *)buf;
	tuple.TupleDataMax = sizeof(buf);
	tuple.TupleOffset = 0;
	tuple.DesiredTuple = CISTPL_CFTABLE_ENTRY;

	for (status = pcmcia_get_first_tuple(pdev, &tuple); status == 0; status = pcmcia_get_next_tuple(pdev, &tuple))
	{
		cistpl_cftable_entry_t dflt = { 0 };
		cistpl_cftable_entry_t *cfg = &(parse.cftable_entry);

		if (pcmcia_get_tuple_data(pdev, &tuple) != 0 || pcmcia_parse_tuple(&tuple, &parse) != 0)
			continue;
		if (cfg->flags & CISTPL_CFTABLE_DEFAULT) dflt = *cfg;
		if (cfg->index == 0)
			continue;
		pdev->conf.ConfigIndex = cfg->index;

		/* Use power settings for Vcc and Vpp if present */
		/*  Note that the CIS values need to be rescaled */
		if (cfg->vpp1.present & (1<<CISTPL_POWER_VNOM))
			pdev->conf.Vpp = cfg->vpp1.param[CISTPL_POWER_VNOM]/10000;
		else if (dflt.vpp1.present & (1<<CISTPL_POWER_VNOM))
			pdev->conf.Vpp = dflt.vpp1.param[CISTPL_POWER_VNOM]/10000;

		/* Do we need to allocate an interrupt? */
		if (cfg->irq.IRQInfo1 || dflt.irq.IRQInfo1)
			pdev->conf.Attributes |= CONF_ENABLE_IRQ;
		if ((cfg->mem.nwin > 0) || (dflt.mem.nwin > 0))
		{
			cistpl_mem_t *mem = (cfg->mem.nwin) ? &cfg->mem : &dflt.mem;

			req.Attributes = WIN_DATA_WIDTH_16|WIN_MEMORY_TYPE_CM|WIN_ENABLE|WIN_USE_WAIT;
			req.Base = mem->win[0].host_addr;
			req.Size = mem->win[0].len;
			req.Size=0x1000;
			req.AccessSpeed = 0;
			if (pcmcia_request_window(&pdev, &req, &pdev->win) != 0)
				continue;
			map.Page = 0; map.CardOffset = mem->win[0].card_addr;
			if (pcmcia_map_mem_page(pdev->win, &map) != 0)
				continue;
			else
				printk(KERN_INFO "MEMORY WINDOW FOUND!!!\n");
		}
		break;
	}
	if (pdev->conf.Attributes & CONF_ENABLE_IRQ)
	{
		printk(KERN_INFO "requesting IRQ...\n");
		pdev->irq.Attributes |= IRQ_TYPE_DYNAMIC_SHARING;
		if ((status = pcmcia_request_irq(pdev, &pdev->irq)) != 0)
			return status;
	}

	/*
	  This actually configures the PCMCIA socket -- setting up
	  the I/O windows and the interrupt mapping, and putting the
	  card and host interface into "Memory and IO" mode.
	*/
	printk(KERN_INFO "requesting configuration...\n");
	if ((status = pcmcia_request_configuration(pdev, &pdev->conf)) != 0)
		return status;

	acx_resources[0].start = req.Base;
	acx_resources[0].end = req.Base + req.Size - 1;
	acx_resources[1].start = acx_resources[1].end = pdev->irq.AssignedIRQ;

	/* Finally, report what we've done */
	printk(KERN_INFO "index 0x%02x: ", pdev->conf.ConfigIndex);
	if (pdev->conf.Attributes & CONF_ENABLE_IRQ)
		printk("irq %d", pdev->irq.AssignedIRQ);
	if (pdev->io.NumPorts1)
		printk(", io 0x%04x-0x%04x", pdev->io.BasePort1,
			pdev->io.BasePort1+pdev->io.NumPorts1-1);
	if (pdev->io.NumPorts2)
		printk(" & 0x%04x-0x%04x", pdev->io.BasePort2,
			pdev->io.BasePort2+pdev->io.NumPorts2-1);
	if (pdev->win)
		printk(", mem 0x%06lx-0x%06lx\n", req.Base,
			req.Base+req.Size-1);

	if ((status = pcmcia_wlan_init()) != 0);
		goto error;

	return 0;

error:
	pcmcia_disable_device(pdev);
	return status;
}

static void acx_cs_detach(struct pcmcia_device *pdev)
{
	pcmcia_wlan_exit();
}

static struct pcmcia_driver acx_driver = {
	.owner		= THIS_MODULE,
	.drv		= {
	    .name	= "acx_cs",
	},
	.probe		= acx_cs_probe,
	.remove		= acx_cs_detach,
	.id_table 	= acx_ids,
//<---->.suspend<------>= acx_cs_suspend,
//<---->.resume><------>= acx_cs_resume,
};

static int __init acx_cs_init(void)
{
        /* return success if at least one succeeded */
	printk(KERN_INFO "acxcs_init()\n");
	return pcmcia_register_driver(&acx_driver);
}

static void __exit acx_cs_cleanup(void)
{
	pcmcia_unregister_driver(&acx_driver);
}

module_init(acx_cs_init);
module_exit(acx_cs_cleanup);

MODULE_AUTHOR("Martin Röder <m-roeder at gmx.net>");
MODULE_DESCRIPTION("WLAN driver for PCMCIA");
MODULE_LICENSE("GPL");
