/* src/ihw.c - low level control functions
 *
 * --------------------------------------------------------------------
 *
 * Copyright (C) 2003  ACX100 Open Source Project
 *
 *   The contents of this file are subject to the Mozilla Public
 *   License Version 1.1 (the "License"); you may not use this file
 *   except in compliance with the License. You may obtain a copy of
 *   the License at http://www.mozilla.org/MPL/
 *
 *   Software distributed under the License is distributed on an "AS
 *   IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or
 *   implied. See the License for the specific language governing
 *   rights and limitations under the License.
 *
 *   Alternatively, the contents of this file may be used under the
 *   terms of the GNU Public License version 2 (the "GPL"), in which
 *   case the provisions of the GPL are applicable instead of the
 *   above.  If you wish to allow the use of your version of this file
 *   only under the terms of the GPL and not to allow others to use
 *   your version of this file under the MPL, indicate your decision
 *   by deleting the provisions above and replace them with the notice
 *   and other provisions required by the GPL.  If you do not delete
 *   the provisions above, a recipient may use your version of this
 *   file under either the MPL or the GPL.
 *
 * --------------------------------------------------------------------
 *
 * Inquiries regarding the ACX100 Open Source Project can be
 * made directly to:
 
 * acx100-users@lists.sf.net
 * http://acx100.sf.net
 *
 * --------------------------------------------------------------------
 */

#if (WLAN_HOSTIF==WLAN_USB)

#if ACX_DEBUG
extern void acx_dump_bytes(void *,int);
#endif

#include <linux/config.h>
#define WLAN_DBVAR	prism2_debug
#include <linux/version.h>
#include <linux/module.h>
#include <linux/kernel.h>

#include <linux/usb.h>
#include <linux/types.h>
#include <linux/if_arp.h>
#include <linux/wireless.h>

#include <linux/pci.h>


/*================================================================*/
/* Project Includes */

#include <acx.h>


/* try to make it compile for both 2.4.x and 2.6.x kernels */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 5, 0)

static inline int submit_urb(struct urb *urb, int mem_flags) {
	        return usb_submit_urb(urb, mem_flags);
}

#else

/* 2.4.x kernels */
#define USB_24	1

static inline int submit_urb(struct urb *urb, int mem_flags) {
	        return usb_submit_urb(urb);
}

#endif

#define FILL_SETUP_PACKET(_pack,_rtype,_req,_val,_ind,_len) \
								(_pack)[0]=_rtype; \
								(_pack)[1]=_req; \
								((unsigned short *)(_pack))[1]=_val; \
								((unsigned short *)(_pack))[2]=_ind; \
								((unsigned short *)(_pack))[3]=_len;

#if USB_24
static void acx100usb_control_complete(struct urb *);
#else
static void acx100usb_control_complete(struct urb *, struct pt_regs *);
#endif

int acx_issue_cmd(wlandevice_t *priv,unsigned int cmd,void *pdr,int paramlen,u32 timeout) {
	int result,skipridheader,blocklen,inpipe,outpipe,acklen=sizeof(priv->usbin);
	int ucode;
	struct usb_device *usbdev;
	FN_ENTER;
	skipridheader=0;
	/* ----------------------------------------------------
	** get context from wlandevice
	** ------------------------------------------------- */
	usbdev=priv->usbdev;
	/* ----------------------------------------------------
	** check which kind of command was issued...
	** ------------------------------------------------- */
	priv->usbout.cmd=cmd;
	priv->usbout.status=0;
	if (cmd==ACX100_CMD_INTERROGATE) {
		priv->usbout.u.rridreq.rid=((memmap_t *)pdr)->type;
		priv->usbout.u.rridreq.frmlen=paramlen;
		blocklen=8;
		switch (priv->usbout.u.rridreq.rid) {
			case ACX100_RID_SCAN_STATUS:skipridheader=1;break;
		}
		if (skipridheader) acklen=paramlen+4;
		else acklen=paramlen+8;
		acxlog(L_XFER,"sending interrogate: cmd=%d status=%d rid=%d frmlen=%d\n",priv->usbout.cmd,priv->usbout.status,priv->usbout.u.rridreq.rid,priv->usbout.u.rridreq.frmlen);
	} else if (cmd==ACX100_CMD_CONFIGURE) {
		priv->usbout.u.wridreq.rid=((memmap_t *)pdr)->type;
		priv->usbout.u.wridreq.frmlen=paramlen;
		memcpy(priv->usbout.u.wridreq.data,&(((memmap_t *)pdr)->m),paramlen);
		blocklen=8+paramlen;
	} else if ((cmd==ACX100_CMD_ENABLE_RX)||(cmd==ACX100_CMD_ENABLE_TX)||(cmd==ACX100_CMD_SLEEP)) {
		priv->usbout.u.rxtx.data=1;		/* just for testing */
		blocklen=5;
	} else {
		/* ----------------------------------------------------
		** All other commands (not thoroughly tested)
		** ------------------------------------------------- */
		if ((pdr)&&(paramlen>0)) memcpy(priv->usbout.u.wmemreq.data,pdr,paramlen);
		blocklen=4+paramlen;
	}
	/* ----------------------------------------------------
	** Obtain the I/O pipes
	** ------------------------------------------------- */
	outpipe=usb_sndctrlpipe(usbdev,0);      /* default endpoint for ctrl-transfers: 0 */
	inpipe =usb_rcvctrlpipe(usbdev,0);      /* default endpoint for ctrl-transfers: 0 */
#if ACX_DEBUG
	acxlog(L_XFER,"sending USB control msg (out) (blocklen=%d)\n",blocklen);
	if (debug&L_DATA) acx_dump_bytes(&(priv->usbout),blocklen);
#endif
	/* --------------------------------------
	** fill setup packet and control urb
	** ----------------------------------- */
	FILL_SETUP_PACKET(priv->usb_setup,USB_TYPE_VENDOR|USB_DIR_OUT,ACX100_USB_UNKNOWN_REQ1,0,0,blocklen)
	usb_fill_control_urb(priv->ctrl_urb,usbdev,outpipe,priv->usb_setup,&(priv->usbout),blocklen,acx100usb_control_complete,priv);
	priv->ctrl_urb->timeout=timeout;
	ucode=submit_urb(priv->ctrl_urb, GFP_KERNEL);
	if (ucode!=0) {
		acxlog(L_STD,"ctrl message FAILED with errcode %d\n",ucode);
		return(0);
	}
	/* ---------------------------------
	** wait for request to complete...
	** ------------------------------ */
	while (priv->ctrl_urb->status==-EINPROGRESS) {
		udelay(500);
	}
	/* ---------------------------------
	** check the result
	** ------------------------------ */
	result=priv->ctrl_urb->actual_length;
	acxlog(L_XFER,"wrote=%d bytes (status=%d)\n",result,priv->ctrl_urb->status);
	if (result<0) {
		return(0);
	}
	/* --------------------------------------
	** Check for device acknowledge ...
	** -------------------------------------- */
	acxlog(L_XFER,"sending USB control msg (in) (acklen=%d)\n",acklen);
	priv->usbin.status=0; /* delete old status flag -> set to fail */
	FILL_SETUP_PACKET(priv->usb_setup,USB_TYPE_VENDOR|USB_DIR_IN,ACX100_USB_UNKNOWN_REQ1,0,0,acklen)
	usb_fill_control_urb(priv->ctrl_urb,usbdev,inpipe,priv->usb_setup,&(priv->usbin),acklen,acx100usb_control_complete,priv);
	priv->ctrl_urb->timeout=timeout;
	ucode=submit_urb(priv->ctrl_urb, GFP_KERNEL);
	if (ucode!=0) {
		acxlog(L_STD,"ctrl message (ack) FAILED with errcode %d\n",ucode);
		return(0);
	}
	/* ---------------------------------
	** wait for request to complete...
	** ------------------------------ */
	while (priv->ctrl_urb->status==-EINPROGRESS) {
		udelay(500);
	}
	/* ---------------------------------
	** check the result
	** ------------------------------ */
	result=priv->ctrl_urb->actual_length;
	acxlog(L_XFER,"read=%d bytes\n",result);
	if (result<0) {
		FN_EXIT0();
		return(0);
	}
	if (priv->usbin.status!=1) {
		acxlog(L_DEBUG,"command returned status %d\n",priv->usbin.status);
	}
	if (cmd==ACX100_CMD_INTERROGATE) {
		if ((pdr)&&(paramlen>0)) {
			if (skipridheader) {
				memcpy(pdr,&(priv->usbin.u.rmemresp.data),paramlen);
				acxlog(L_XFER,"response frame: cmd=%d status=%d\n",priv->usbin.cmd,priv->usbin.status);
				acxlog(L_DATA,"incoming bytes (%d):\n",paramlen);
				if (debug&L_DATA) acx_dump_bytes(pdr,paramlen);
			}
			else {
				memcpy(pdr,&(priv->usbin.u.rridresp.rid),paramlen+4);
				acxlog(L_XFER,"response frame: cmd=%d status=%d rid=%d frmlen=%d\n",priv->usbin.cmd,priv->usbin.status,priv->usbin.u.rridresp.rid,priv->usbin.u.rridresp.frmlen);
				acxlog(L_DATA,"incoming bytes (%d):\n",paramlen+4);
				if (debug&L_DATA) acx_dump_bytes(pdr,paramlen+4);
			}
		}
	}
	FN_EXIT0();
	return(1);
}

#if USB_24
static void acx100usb_control_complete(struct urb *urb)
#else
static void acx100usb_control_complete(struct urb *urb, struct pt_regs *regs)
#endif
{
	FN_ENTER;
	FN_EXIT0();
}

/*****************************************************************************
 *
 * The Upper Layer
 *
 ****************************************************************************/

static short CtlLength[0x14] = {
	0,
	ACX100_RID_ACX_TIMER_LEN,
	ACX100_RID_POWER_MGMT_LEN,
	ACX100_RID_QUEUE_CONFIG_LEN,
	ACX100_RID_BLOCK_SIZE_LEN,
	ACX100_RID_MEMORY_CONFIG_OPTIONS_LEN,
	ACX100_RID_RATE_FALLBACK_LEN,
	ACX100_RID_WEP_OPTIONS_LEN,
	ACX100_RID_MEMORY_MAP_LEN,
	ACX100_RID_SCAN_STATUS_LEN,
	ACX100_RID_ASSOC_ID_LEN,
	0,
	0,
	ACX100_RID_FWREV_LEN,
	ACX100_RID_FCS_ERROR_COUNT_LEN,
	ACX100_RID_MEDIUM_USAGE_LEN,
	ACX100_RID_RXCONFIG_LEN,
	0,
	0,
	ACX100_RID_FIRMWARE_STATISTICS_LEN
};


static short CtlLengthDot11[0x14] = {
	0,
	ACX100_RID_DOT11_STATION_ID_LEN,
	0,
	ACX100_RID_DOT11_BEACON_PERIOD_LEN,
	ACX100_RID_DOT11_DTIM_PERIOD_LEN,
	ACX100_RID_DOT11_SHORT_RETRY_LIMIT_LEN,
	ACX100_RID_DOT11_LONG_RETRY_LIMIT_LEN,
	ACX100_RID_DOT11_WEP_DEFAULT_KEY_LEN,
	ACX100_RID_DOT11_MAX_XMIT_MSDU_LIFETIME_LEN,
	0,
	ACX100_RID_DOT11_CURRENT_REG_DOMAIN_LEN,
	ACX100_RID_DOT11_CURRENT_ANTENNA_LEN,
	0,
	ACX100_RID_DOT11_TX_POWER_LEVEL_LEN,
	ACX100_RID_DOT11_CURRENT_CCA_MODE_LEN,
	ACX100_RID_DOT11_ED_THRESHOLD_LEN,
	ACX100_RID_DOT11_WEP_DEFAULT_KEY_SET_LEN,
	0,
	0,
	0
};




/*----------------------------------------------------------------
** acx_configure():
**
**  Inputs:
**     priv -> Pointer to wlandevice structure
**    pdr -> Field for parameter data
**   type -> Type of request (Request-ID)
** ---------------------------------------------------------------
** Returns:
**  (int) Success indicator (1 on success, 0 on failure)
**
** Description:
*----------------------------------------------------------------*/

int acx_configure(wlandevice_t *priv, void *pdr, short type)
{
  u16 len;
  /* ----------------------------------------------------
  ** check if ACX100 control command or if 802.11 command
  ** ------------------------------------------------- */
  if (type<0x1000) len=CtlLength[type];
  else len=CtlLengthDot11[type-0x1000];
	if (len==0) {
		acxlog(L_STD,"WARNING: ENCOUNTERED ZEROLENGTH RID (%x)\n",type);
	}
  acxlog(L_XFER,"configuring: type(rid)=0x%X len=%d\n",type,len);
  ((memmap_t *)pdr)->type = cpu_to_le16(type);
  return(acx_issue_cmd(priv,ACX100_CMD_CONFIGURE,pdr,len,ACX_CMD_TIMEOUT_DEFAULT));
}



/*----------------------------------------------------------------
** acx_configure_length():
**
**  Inputs:
**     priv -> Pointer to wlandevice structure
**    pdr -> Field for parameter data
**   type -> Type of request (Request-ID)
** ---------------------------------------------------------------
** Returns:
**  (int) Success indicator (0 on failure, 1 on success)
**
** Description:
** ----------------------------------------------------------------*/

int acx_configure_length(wlandevice_t *priv, void *pdr,short type,short len) {
  acxlog(L_XFER,"configuring: type(rid)=0x%X len=%d\n",type,len);
  ((memmap_t *)pdr)->type = cpu_to_le16(type);
  return(acx_issue_cmd(priv,ACX100_CMD_CONFIGURE,pdr,len,ACX_CMD_TIMEOUT_DEFAULT));
}



/*----------------------------------------------------------------
** acx_interrogate():
**  Inputs:
**     priv -> Pointer to wlandevice structure
**    pdr -> Field for parameter data
**   type -> Type of request (Request-ID)
** ---------------------------------------------------------------
** Returns:
**  (int) Errorcode (0 if OK)
**
** Description:
**--------------------------------------------------------------*/

int acx_interrogate(wlandevice_t *priv, void *pdr, short type)
{
  u16 len;
  /* ----------------------------------------------------
  ** check if ACX100 control command or if 802.11 command
  ** ------------------------------------------------- */
  if (type<0x1000) len=CtlLength[type];
  else len=CtlLengthDot11[type-0x1000];
	if (len==0) {
		acxlog(L_STD,"WARNING: ENCOUNTERED ZEROLENGTH RID (%x)\n",type);
	}
  acxlog(L_XFER,"interrogating: type(rid)=0x%X len=%d pdr=%p\n",type,len,pdr);
  ((memmap_t *)pdr)->type = cpu_to_le16(type);
  return(acx_issue_cmd(priv,ACX100_CMD_INTERROGATE,pdr,len,ACX_CMD_TIMEOUT_DEFAULT));
}



/*****************************************************************************
 *
 * MAC Address Stuff
 *
 ****************************************************************************/

/*----------------------------------------------------------------
* acx_power_led
*
*
* Arguments:
*
* Returns:
*
* Side effects:
*
* Call context:
*
* STATUS: NEW
*
* Comment:
*
*----------------------------------------------------------------*/
void acx_power_led(wlandevice_t *priv, u8 enable) {
  /*
  if (enable)
    acx_write_reg16(priv, 0x290, acx_read_reg16(priv, 0x290) & ~0x0800);
  else
    acx_write_reg16(priv, 0x290, acx_read_reg16(priv, 0x290) | 0x0800);
  */
}

#endif
