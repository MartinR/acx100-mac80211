/* src/acx100_ioctl.c - all the ioctl calls
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
 *
 * acx100-users@lists.sf.net
 * http://acx100.sf.net
 *
 * --------------------------------------------------------------------
 */

#ifdef S_SPLINT_S /* some crap that splint needs to not crap out */
#define __signed__ signed
#define __u64 unsigned long long
#define u64 unsigned long long
#define loff_t unsigned long
#define sigval_t unsigned long
#define siginfo_t unsigned long
#define stack_t unsigned long
#define __s64 signed long long
#endif
#include <linux/config.h>
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <asm/io.h>
#include <asm/uaccess.h> /* required for 2.4.x kernels; verify_write() */

#include <linux/if_arp.h>
#include <linux/wireless.h>
#if WIRELESS_EXT >= 13
#include <net/iw_handler.h>
#endif /* WE >= 13 */

/*================================================================*/
/* Project Includes */

#include <acx.h>

/* About the locking:
 *  I only locked the device whenever calls to the hardware are made or
 *  parts of the wlandevice struct are modified, otherwise no locking is
 *  performed. I don't now if this is safe, we'll see.
 */

extern u8 acx_signal_determine_quality(u8 signal, u8 noise);


/* if you plan to reorder something, make sure to reorder all other places
 * accordingly! */
/* someone broke SET/GET convention: SETs must have even position, GETs odd */
#define ACX100_IOCTL SIOCIWFIRSTPRIV
enum {
	ACX100_IOCTL_DEBUG = ACX100_IOCTL,
	ACX100_IOCTL_GET__________UNUSED1,
	ACX100_IOCTL_SET_PLED,
	ACX100_IOCTL_GET_PLED,
	ACX100_IOCTL_SET_RATES,
	ACX100_IOCTL_LIST_DOM,
	ACX100_IOCTL_SET_DOM,
	ACX100_IOCTL_GET_DOM,
	ACX100_IOCTL_SET_SCAN_PARAMS,
	ACX100_IOCTL_GET_SCAN_PARAMS,
	ACX100_IOCTL_SET_PREAMB,
	ACX100_IOCTL_GET_PREAMB,
	ACX100_IOCTL_SET_ANT,
	ACX100_IOCTL_GET_ANT,
	ACX100_IOCTL_RX_ANT,
	ACX100_IOCTL_TX_ANT,
	ACX100_IOCTL_SET_PHY_AMP_BIAS,
	ACX100_IOCTL_GET_PHY_CHAN_BUSY,
	ACX100_IOCTL_SET_ED,
	ACX100_IOCTL_GET__________UNUSED3,
	ACX100_IOCTL_SET_CCA,
	ACX100_IOCTL_GET__________UNUSED4,
	ACX100_IOCTL_MONITOR,
	ACX100_IOCTL_TEST,
	ACX100_IOCTL_DBG_SET_MASKS,
	ACX111_IOCTL_INFO,
	ACX100_IOCTL_DBG_SET_IO,
	ACX100_IOCTL_DBG_GET_IO
};

/* channel frequencies
 * TODO: Currently, every other 802.11 driver keeps its own copy of this. In
 * the long run this should be integrated into ieee802_11.h or wireless.h or
 * whatever IEEE802.11x framework evolves */
static const u16 acx_channel_freq[] = {
	2412, 2417, 2422, 2427, 2432, 2437, 2442,
	2447, 2452, 2457, 2462, 2467, 2472, 2484,
};

static const struct iw_priv_args acx_ioctl_private_args[] = {
#if ACX_DEBUG
{ cmd : ACX100_IOCTL_DEBUG,
	set_args : IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
	get_args : 0,
	name : "SetDebug" },
#endif
{ cmd : ACX100_IOCTL_SET_PLED,
	set_args : IW_PRIV_TYPE_BYTE | 2,
	get_args : 0,
	name : "SetLEDPower" },
{ cmd : ACX100_IOCTL_GET_PLED,
	set_args : 0,
	get_args : IW_PRIV_TYPE_BYTE | IW_PRIV_SIZE_FIXED | 2,
	name : "GetLEDPower" },
{ cmd : ACX100_IOCTL_SET_RATES,
	set_args : IW_PRIV_TYPE_CHAR | 256,
	get_args : 0,
	name : "SetRates" },
{ cmd : ACX100_IOCTL_LIST_DOM,
	set_args : 0,
	get_args : 0,
	name : "ListRegDomain" },
{ cmd : ACX100_IOCTL_SET_DOM,
	set_args : IW_PRIV_TYPE_BYTE | IW_PRIV_SIZE_FIXED | 1,
	get_args : 0,
	name : "SetRegDomain" },
{ cmd : ACX100_IOCTL_GET_DOM,
	set_args : 0,
	get_args : IW_PRIV_TYPE_BYTE | IW_PRIV_SIZE_FIXED | 1,
	name : "GetRegDomain" },
{ cmd : ACX100_IOCTL_SET_SCAN_PARAMS,
	set_args : IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 4,
	get_args : 0,
	name : "SetScanParams" },
{ cmd : ACX100_IOCTL_GET_SCAN_PARAMS,
	set_args : 0,
	get_args : IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 4,
	name : "GetScanParams" },
{ cmd : ACX100_IOCTL_SET_PREAMB,
	set_args : IW_PRIV_TYPE_BYTE | IW_PRIV_SIZE_FIXED | 1,
	get_args : 0,
	name : "SetSPreamble" },
{ cmd : ACX100_IOCTL_GET_PREAMB,
	set_args : 0,
	get_args : IW_PRIV_TYPE_BYTE | IW_PRIV_SIZE_FIXED | 1,
	name : "GetSPreamble" },
{ cmd : ACX100_IOCTL_SET_ANT,
	set_args : IW_PRIV_TYPE_BYTE | IW_PRIV_SIZE_FIXED | 1,
	get_args : 0,
	name : "SetAntenna" },
{ cmd : ACX100_IOCTL_GET_ANT,
	set_args : 0,
	get_args : 0,
	name : "GetAntenna" },
{ cmd : ACX100_IOCTL_RX_ANT,
	set_args : IW_PRIV_TYPE_BYTE | IW_PRIV_SIZE_FIXED | 1,
	get_args : 0,
	name : "SetRxAnt" },
{ cmd : ACX100_IOCTL_TX_ANT,
	set_args : IW_PRIV_TYPE_BYTE | IW_PRIV_SIZE_FIXED | 1,
	get_args : 0,
	name : "SetTxAnt" },
{ cmd : ACX100_IOCTL_SET_PHY_AMP_BIAS,
	set_args : IW_PRIV_TYPE_BYTE | IW_PRIV_SIZE_FIXED | 1,
	get_args : 0,
	name : "SetPhyAmpBias"},
{ cmd : ACX100_IOCTL_GET_PHY_CHAN_BUSY,
	set_args : 0,
	get_args : 0,
	name : "GetPhyChanBusy" },
{ cmd : ACX100_IOCTL_SET_ED,
	set_args : IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
	get_args : 0,
	name : "SetED" },
{ cmd : ACX100_IOCTL_SET_CCA,
	set_args : IW_PRIV_TYPE_BYTE | IW_PRIV_SIZE_FIXED | 1,
	get_args : 0,
	name : "SetCCA" },
{ cmd : ACX100_IOCTL_MONITOR,
	set_args : IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 2,
	get_args : 0,
	name : "monitor" },
{ cmd : ACX100_IOCTL_TEST,
	set_args : 0,
	get_args : 0,
	name : "Test" },
{ cmd : ACX100_IOCTL_DBG_SET_MASKS,
	set_args : IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 2,
	get_args : 0,
	name : "DbgSetMasks" },
{ cmd : ACX111_IOCTL_INFO,
	set_args : 0,
	get_args : 0,
	name : "GetAcx111Info" },
{ cmd : ACX100_IOCTL_DBG_SET_IO,
	set_args : IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 4,
	get_args : 0,
	name : "DbgSetIO" },
{ cmd : ACX100_IOCTL_DBG_GET_IO,
	set_args : IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 3,
	get_args : 0,
	name : "DbgGetIO" },
};

/*------------------------------------------------------------------------------
 * acx_ioctl_commit
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
 *----------------------------------------------------------------------------*/
static inline int acx_ioctl_commit(struct net_device *dev,
				      struct iw_request_info *info,
				      void *zwrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);
	
	FN_ENTER;
	acx_update_card_settings(priv, 0, 0, 0);
	FN_EXIT0();
	return OK;
}

static inline int acx_ioctl_get_name(struct net_device *dev, struct iw_request_info *info, char *cwrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);
	static const char * const names[] = { "IEEE 802.11b+/g+", "IEEE 802.11b+" };

	strcpy(cwrq, names[(CHIPTYPE_ACX111 == priv->chip_type) ? 0 : 1]);

	acxlog(L_IOCTL, "Get Name ==> %s\n", cwrq);
	return OK;
}

/*----------------------------------------------------------------
* acx_ioctl_set_freq
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
static int acx_ioctl_set_freq(struct net_device *dev, struct iw_request_info *info, struct iw_freq *fwrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);
	int channel = -1;
	unsigned int mult = 1;
	unsigned long flags;
	int result;

	FN_ENTER;
	acxlog(L_IOCTL, "Set Frequency <== %i (%i)\n", fwrq->m, fwrq->e);

	if (fwrq->e == 0 && fwrq->m <= 1000) {
		/* Setting by channel number */
		channel = fwrq->m;
	} else {
		/* If setting by frequency, convert to a channel */
		int i;

		for (i = 0; i < (6 - fwrq->e); i++)
			mult *= 10;

		for (i = 1; i <= 14; i++)
			if (fwrq->m == acx_channel_freq[i - 1] * mult)
				channel = i;
	}

	if (channel > 14) {
		result = -EINVAL;
		goto end;
	}

	result = acx_lock(priv, &flags);
	if (result) {
		goto end;
	}

	priv->channel = channel;
	/* hmm, the following code part is strange, but this is how
	 * it was being done before... */
	acxlog(L_IOCTL, "Changing to channel %d\n", channel);
	SET_BIT(priv->set_mask, GETSET_CHANNEL);
	acx_unlock(priv, &flags);
	result = -EINPROGRESS; /* need to call commit handler */
end:
	FN_EXIT1(result);
	return result;
}

static inline int acx_ioctl_get_freq(struct net_device *dev, struct iw_request_info *info, struct iw_freq *fwrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);
	fwrq->e = 0;
	fwrq->m = priv->channel;
	return OK;
}

/*----------------------------------------------------------------
* acx_ioctl_set_mode
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
static int acx_ioctl_set_mode(struct net_device *dev, struct iw_request_info *info, u32 *uwrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);
	unsigned long flags;
	int result;

	FN_ENTER;
	acxlog(L_IOCTL, "Set iwmode: %i\n", *uwrq);

	result = acx_lock(priv, &flags);
	if (result)
		goto end;

	switch(*uwrq) {
	case IW_MODE_AUTO:
		priv->mode = ACX_MODE_OFF;
		break;
#if WIRELESS_EXT > 14
	case IW_MODE_MONITOR:
		priv->mode = ACX_MODE_MONITOR;
		break;
#endif /* WIRELESS_EXT > 14 */
	case IW_MODE_ADHOC:
		priv->mode = ACX_MODE_0_ADHOC;
		break;
	case IW_MODE_INFRA:
		priv->mode = ACX_MODE_2_STA;
		break;
	case IW_MODE_MASTER:
#define USE_OWN_MASTER_MODE_CODE 1
#if USE_OWN_MASTER_MODE_CODE
		acxlog(0xffff, "Master mode (HostAP) is very, very "
			"experimental! It might work partially, but "
			"better get prepared for nasty surprises "
			"at any time... ;-)\n");
		priv->mode = ACX_MODE_3_AP;
		break;
#else
		acxlog(0xffff, "Master mode (HostAP) not supported! "
			"Can be supported once the driver switched "
			"to the new Linux 802.11 stack that's currently "
			"under development...\n");
		/* fall through */
#endif
	case IW_MODE_REPEAT:
	case IW_MODE_SECOND:
	default:
		result = -EOPNOTSUPP;
		goto end_unlock;
	}

	SET_BIT(priv->set_mask, GETSET_MODE);
	result = -EINPROGRESS;

end_unlock:
	acx_unlock(priv, &flags);
end:
	FN_EXIT1(result);
	return result;
}

static int acx_ioctl_get_mode(struct net_device *dev, struct iw_request_info *info, u32 *uwrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);
	int result = 0;

	switch (priv->mode) {
	case ACX_MODE_OFF:
		*uwrq = IW_MODE_AUTO; break;
#if WIRELESS_EXT > 14
	case ACX_MODE_MONITOR:
		*uwrq = IW_MODE_MONITOR; break;
#endif /* WIRELESS_EXT > 14 */
	case ACX_MODE_0_ADHOC:
		*uwrq = IW_MODE_ADHOC; break;
	case ACX_MODE_2_STA:
		*uwrq = IW_MODE_INFRA; break;
	case ACX_MODE_3_AP:
		*uwrq = IW_MODE_MASTER; break;
	default:
		result = -EOPNOTSUPP;
	}
	acxlog(L_IOCTL, "Get Mode ==> %d\n", *uwrq);
	return result;
}

static int acx_ioctl_set_sens(struct net_device *dev, struct iw_request_info *info, struct iw_param *vwrq, char *extra)
{
#if (WLAN_HOSTIF!=WLAN_USB)
	wlandevice_t *priv = acx_netdev_priv(dev);

	acxlog(L_IOCTL, "Set Sensitivity <== %d\n", vwrq->value);

	if ((RADIO_RFMD_11 == priv->radio_type)
	|| (RADIO_MAXIM_0D == priv->radio_type)
	|| (RADIO_RALINK_15 == priv->radio_type)
	|| (RADIO_RADIA_16 == priv->radio_type)
	|| (RADIO_UNKNOWN_17 == priv->radio_type)) {
		priv->sensitivity = (1 == vwrq->disabled) ? 0 : vwrq->value;
		SET_BIT(priv->set_mask, GETSET_SENSITIVITY);
		return -EINPROGRESS;
	} else {
#else
	{
#endif
		printk("Don't know how to modify sensitivity for this radio type, please try to add that!\n");
		return -EINVAL;
	}
}

static int acx_ioctl_get_sens(struct net_device *dev, struct iw_request_info *info, struct iw_param *vwrq, char *extra)
{
#if (WLAN_HOSTIF!=WLAN_USB)
	wlandevice_t *priv = acx_netdev_priv(dev);

	if ((RADIO_RFMD_11 == priv->radio_type)
	|| (RADIO_MAXIM_0D == priv->radio_type)
	|| (RADIO_RALINK_15 == priv->radio_type)
	|| (RADIO_RADIA_16 == priv->radio_type)
	|| (RADIO_UNKNOWN_17 == priv->radio_type)) {
		acxlog(L_IOCTL, "Get Sensitivity ==> %d\n", priv->sensitivity);

		vwrq->value = priv->sensitivity;
		vwrq->disabled = (vwrq->value == 0);
		vwrq->fixed = 1;
		return OK;
	} else {
#else
	{
#endif
		printk("Don't know how to get sensitivity for this radio type, please try to add that!\n");
		return -EINVAL;
	}
}

/*------------------------------------------------------------------------------
 * acx_ioctl_set_ap
 * 
 * Sets the MAC address of the AP to associate with 
 *
 * Arguments:
 *
 * Returns:
 *
 * Side effects:
 *
 * Call context: Process
 *
 * STATUS: NEW
 *
 *----------------------------------------------------------------------------*/
static int acx_ioctl_set_ap(struct net_device *dev,
				      struct iw_request_info *info,
				      struct sockaddr *awrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);
	int result = 0;
	const u8 *ap;

	FN_ENTER;
	if (NULL == awrq) {
		result = -EFAULT;
		goto end;
	}
	if (ARPHRD_ETHER != awrq->sa_family) {
                result = -EINVAL;
		goto end;
	}
	
	ap = awrq->sa_data;
	acxlog(L_IOCTL, "Set AP = "MACSTR"\n", MAC(ap));

	MAC_COPY(priv->ap, ap);

	/* We want to start rescan in managed or ad-hoc mode,
	** otherwise just set priv->ap.
	** "iwconfig <if> ap <mac> mode managed": we must be able
	** to set ap _first_ and _then_ set mode */
	switch (priv->mode) {
	case ACX_MODE_0_ADHOC:
	case ACX_MODE_2_STA:
		/* FIXME: if there is a convention on what zero AP means,
		** please add a comment about that. I don't know of any --vda */
		if (mac_is_zero(ap)) {
			/* "off" == 00:00:00:00:00:00 */
			MAC_BCAST(priv->ap);
			acxlog(L_IOCTL, "Not reassociating\n");
		} else {
			acxlog(L_IOCTL, "Forcing reassociation\n");
			SET_BIT(priv->set_mask, GETSET_RESCAN);
		}
		break;
	}
	result = -EINPROGRESS;
end:
	FN_EXIT1(result);
	return result;
}

static inline int acx_ioctl_get_ap(struct net_device *dev, struct iw_request_info *info, struct sockaddr *awrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);

	acxlog(L_IOCTL, "Get BSSID\n");
	if (ACX_STATUS_4_ASSOCIATED == priv->status) {
		/* as seen in Aironet driver, airo.c */
		MAC_COPY(awrq->sa_data, priv->bssid);
	} else {
		MAC_ZERO(awrq->sa_data);
	}
	awrq->sa_family = ARPHRD_ETHER;
	return OK;
}

/*----------------------------------------------------------------
* acx_ioctl_get_aplist
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
* Comment: deprecated in favour of iwscan.
* We simply return the list of currently available stations in range,
* don't do a new scan.
*
*----------------------------------------------------------------*/
static int acx_ioctl_get_aplist(struct net_device *dev, struct iw_request_info *info, struct iw_point *dwrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);
	struct sockaddr *address = (struct sockaddr *) extra;
	struct iw_quality qual[IW_MAX_AP];
	int i,cur;
	int result = OK;

	FN_ENTER;

	/* we have AP list only in STA mode */
	if (ACX_MODE_2_STA != priv->mode) {
		result = -EOPNOTSUPP;
		goto end;
	}

	cur = 0;
	for (i = 0; i < VEC_SIZE(priv->sta_list); i++) {
		struct client *bss = &priv->sta_list[i];
		if (!bss->used) continue;
		MAC_COPY(address[cur].sa_data, bss->bssid);
		address[cur].sa_family = ARPHRD_ETHER;
		qual[cur].level = bss->sir;
		qual[cur].noise = bss->snr;
#ifndef OLD_QUALITY
		qual[cur].qual = acx_signal_determine_quality(qual[cur].level, qual[cur].noise);
#else
		qual[cur].qual = (qual[cur].noise <= 100) ?
			       100 - qual[cur].noise : 0;
#endif
		qual[cur].updated = 0; /* no scan: level/noise/qual not updated */
		cur++;
	}
	if (cur) {
		dwrq->flags = 1;
		memcpy(extra + sizeof(struct sockaddr)*cur, &qual,
				sizeof(struct iw_quality)*cur);
	}
	dwrq->length = cur;
end:
	FN_EXIT1(result);
	return result;
}

static int acx_ioctl_set_scan(struct net_device *dev, struct iw_request_info *info, struct iw_param *vwrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);
	int result = -EINVAL;

	FN_ENTER;

	/* don't start scan if device is not up yet */
	if (0 == (priv->dev_state_mask & ACX_STATE_IFACE_UP)) {
		result = -EAGAIN;
		goto end;
	}

	/* This is NOT a rescan for new AP!
	** Do not use SET_BIT(GETSET_RESCAN); */
	acx_cmd_start_scan(priv);
	priv->scan_start = jiffies;
	priv->scan_running = 1;
	result = OK;

end:
	FN_EXIT1(result);
	return result;
}

#if WIRELESS_EXT > 13
static char *acx_ioctl_scan_add_station(wlandevice_t *priv, char *ptr, char *end_buf, struct client *bss)
{
	struct iw_event iwe;
	char *ptr_rate;

	FN_ENTER;

	/* MAC address has to be added first */
	iwe.cmd = SIOCGIWAP;
	iwe.u.ap_addr.sa_family = ARPHRD_ETHER;
	MAC_COPY(iwe.u.ap_addr.sa_data, bss->bssid);
	acxlog(L_IOCTL, "scan, station address: ");
	acx_log_mac_address(L_IOCTL, bss->bssid, "\n");
	ptr = iwe_stream_add_event(ptr, end_buf, &iwe, IW_EV_ADDR_LEN);

	/* Add ESSID */
	iwe.cmd = SIOCGIWESSID;
	iwe.u.data.length = bss->essid_len;
	iwe.u.data.flags = 1;
	acxlog(L_IOCTL, "scan, essid: %s\n", bss->essid);
	ptr = iwe_stream_add_point(ptr, end_buf, &iwe, bss->essid);
	
	/* Add mode */
	iwe.cmd = SIOCGIWMODE;
	if (bss->cap_info & (WF_MGMT_CAP_ESS | WF_MGMT_CAP_IBSS)) {
		if (bss->cap_info & WF_MGMT_CAP_ESS)
			iwe.u.mode = IW_MODE_MASTER;
		else
			iwe.u.mode = IW_MODE_ADHOC;
		acxlog(L_IOCTL, "scan, mode: %d\n", iwe.u.mode);
		ptr = iwe_stream_add_event(ptr, end_buf, &iwe, IW_EV_UINT_LEN);
	}

	/* Add frequency */
	iwe.cmd = SIOCGIWFREQ;
	iwe.u.freq.m = acx_channel_freq[bss->channel - 1] * 100000;
	iwe.u.freq.e = 1;
	acxlog(L_IOCTL, "scan, frequency: %d\n", iwe.u.freq.m);
	ptr = iwe_stream_add_event(ptr, end_buf, &iwe, IW_EV_FREQ_LEN);

	/* Add link quality */
	iwe.cmd = IWEVQUAL;
	/* FIXME: these values should be expressed in dBm, but we don't know
	 * how to calibrate it yet */
	iwe.u.qual.level = bss->sir;
	iwe.u.qual.noise = bss->snr;
#ifndef OLD_QUALITY
	iwe.u.qual.qual = acx_signal_determine_quality(iwe.u.qual.level, iwe.u.qual.noise);
#else
	iwe.u.qual.qual = (iwe.u.qual.noise <= 100) ?
				100 - iwe.u.qual.noise : 0;
#endif
	iwe.u.qual.updated = 7;
	acxlog(L_IOCTL, "scan, link quality: %d/%d/%d\n", iwe.u.qual.level, iwe.u.qual.noise, iwe.u.qual.qual);
	ptr = iwe_stream_add_event(ptr, end_buf, &iwe, IW_EV_QUAL_LEN);

	/* Add encryption */
	iwe.cmd = SIOCGIWENCODE;
	if (bss->cap_info & WF_MGMT_CAP_PRIVACY)
		iwe.u.data.flags = IW_ENCODE_ENABLED | IW_ENCODE_NOKEY;
	else
		iwe.u.data.flags = IW_ENCODE_DISABLED;
	iwe.u.data.length = 0;
	acxlog(L_IOCTL, "scan, encryption flags: %x\n", iwe.u.data.flags);
	ptr = iwe_stream_add_point(ptr, end_buf, &iwe, bss->essid);

	/* add rates */
	iwe.cmd = SIOCGIWRATE;
	iwe.u.bitrate.fixed = iwe.u.bitrate.disabled = 0;
	ptr_rate = ptr + IW_EV_LCP_LEN;

	{
	u16 rate = bss->rate_cap;
	const u8* p = bitpos2ratebyte;
	while (rate) {
		if (rate & 1) {
			iwe.u.bitrate.value = *p * 500000; /* units of 500kb/s */
			acxlog(L_IOCTL, "scan, rate: %d\n", iwe.u.bitrate.value);
			ptr = iwe_stream_add_value(ptr, ptr_rate, end_buf, &iwe, IW_EV_PARAM_LEN);
		}
		rate >>= 1;
		p++;
	}}

	if ((ptr_rate - ptr) > (ptrdiff_t)IW_EV_LCP_LEN)
		ptr = ptr_rate;

	/* drop remaining station data items for now */

	FN_EXIT1((int)ptr);
	return ptr;
}

static int acx_ioctl_get_scan(struct net_device *dev, struct iw_request_info *info, struct iw_point *dwrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);
	char *ptr = extra;
	int i;
	int result = OK;

	FN_ENTER;

	/* no scan available if device is not up yet */
	if (0 == (priv->dev_state_mask & ACX_STATE_IFACE_UP)) {
		acxlog(L_IOCTL, "iface not up yet\n");
		result = -EAGAIN;
		goto end;
	}

	if (priv->scan_start && time_before(jiffies, priv->scan_start + 3*HZ)) {
		acxlog(L_IOCTL, "scan still in progress, so no results yet, sorry\n");
		result = -EAGAIN;
		goto end;
	}
	priv->scan_start = 0;

#if ENODATA_TO_BE_USED_AFTER_SCAN_ERROR_ONLY
	if (priv->bss_table_count == 0)	{
		/* no stations found */
		result = -ENODATA;
		goto end;
	}
#endif

	for (i = 0; i < VEC_SIZE(priv->sta_list); i++) {
		struct client *bss = &priv->sta_list[i];
		if (!bss->used) continue;
		ptr = acx_ioctl_scan_add_station(priv, ptr, extra + IW_SCAN_MAX_DATA, bss);
	}
	dwrq->length = ptr - extra;
	dwrq->flags = 0;

end:
	FN_EXIT1(result);
	return result;
}
#endif /* WIRELESS_EXT > 13 */

/*----------------------------------------------------------------
* acx_ioctl_set_essid
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
static int acx_ioctl_set_essid(struct net_device *dev, struct iw_request_info *info, struct iw_point *dwrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);
	int len = dwrq->length;
	unsigned long flags;
	int result;

	FN_ENTER;
	acxlog(L_IOCTL, "Set ESSID '%s', length %d, flags 0x%04x\n", extra, len, dwrq->flags);

	if (len < 0) {
		result = -EINVAL;
		goto end;
	}

	result = acx_lock(priv, &flags);
	if (result) {
		goto end;
	}

	/* ESSID disabled? */
	if (0 == dwrq->flags) {
		priv->essid_active = 0;

	} else {
		if (dwrq->length > IW_ESSID_MAX_SIZE+1)	{
			result = -E2BIG;
			goto end_unlock;
		}

		priv->essid_len = len - 1;
		memcpy(priv->essid, extra, priv->essid_len);
		priv->essid[priv->essid_len] = '\0';
		priv->essid_active = 1;
	}

	SET_BIT(priv->set_mask, GETSET_RESCAN);

end_unlock:
	acx_unlock(priv, &flags);
	result = -EINPROGRESS;
end:
	FN_EXIT1(result);
	return result;
}

static int acx_ioctl_get_essid(struct net_device *dev, struct iw_request_info *info, struct iw_point *dwrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);

	acxlog(L_IOCTL, "Get ESSID ==> %s\n", priv->essid);

	dwrq->flags = priv->essid_active;
	if (priv->essid_active)	{
		memcpy(extra, priv->essid, priv->essid_len);
		extra[priv->essid_len] = '\0';
		dwrq->length = priv->essid_len + 1;
		dwrq->flags = 1;
	}
	return OK;
}

/*----------------------------------------------------------------
* acx_ioctl_set_rate
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
static void
update_client_rates(wlandevice_t *priv, u16 rate)
{
	int i;
	for(i = 0; i < VEC_SIZE(priv->sta_list); i++) {
		client_t *clt = &priv->sta_list[i];
		if (!clt->used)	continue;
		clt->rate_cfg = (clt->rate_cap & rate);
		if (!clt->rate_cfg) {
			/* no compatible rates left: kick client */
			printk("Client %d kicked: rates are incompatible\n", i);
			acx_sta_list_del(priv, clt);
			continue;
		}
		clt->rate_cur &= clt->rate_cfg;
		if (!clt->rate_cur) {
			/* current rate become invalid, choose a valid one */
			int cur = 1;
			while (!(cur & clt->rate_cfg)) cur<<=1;
			clt->rate_cur = cur;
		}
		clt->fallback_count = clt->stepup_count = 0;
	}
	switch (priv->mode) {
	case ACX_MODE_2_STA:
		if (priv->ap_client && !priv->ap_client->used) {
			/* Owwww... we kicked our AP!! :) */
			SET_BIT(priv->set_mask, GETSET_RESCAN);
		}
	}
}

/* maps bits from acx111 rate to rate in Mbits */
static const unsigned int acx111_rate_tbl[] = {
     1000000,
     2000000,
     5500000,
     6000000,
     9000000,
    11000000,
    12000000,
    18000000,
    22000000,
    24000000,
    36000000,
    48000000,
    54000000,
};

static int
acx_ioctl_set_rate(struct net_device *dev,
		struct iw_request_info *info,
		struct iw_param *vwrq,
		char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);
	u16 txrate_cfg = 1;
	unsigned long flags;
	int autorate;
	int result = -EINVAL;

	FN_ENTER;
	acxlog(L_IOCTL,
	       "rate %d fixed 0x%x disabled 0x%x flags 0x%x\n",
	       vwrq->value, vwrq->fixed, vwrq->disabled, vwrq->flags);

	if ((0 == vwrq->fixed) || (1 == vwrq->fixed)) {
		int i = VEC_SIZE(acx111_rate_tbl)-1;
		if(vwrq->value == -1)
			/* "iwconfig rate auto" --> choose highest */
			vwrq->value = (CHIPTYPE_ACX100 == priv->chip_type) ? 22000000 : 54000000;
		while(i >= 0) {
			if(vwrq->value == acx111_rate_tbl[i]) {
				txrate_cfg <<= i;
				i = 0;
				break;
			}
			i--;
		}
		if(i == -1) { /* no matching rate */
			result = -EINVAL;
			goto end;
		}
	} else {	/* rate N, N<1000 (driver specific): we don't use this */
		result = -EOPNOTSUPP;
		goto end;
	}
	/* now: only one bit is set in txrate_cfg, corresponding to
	** indicated rate */

	autorate = (vwrq->fixed == 0) && (RATE111_1 != txrate_cfg);
	if(autorate) {
		/* convert 00100000 -> 00111111 */
		txrate_cfg = (txrate_cfg<<1)-1;
	}

	if (CHIPTYPE_ACX100 == priv->chip_type) {
		txrate_cfg &= RATE111_ACX100_COMPAT;
		if(!txrate_cfg) {
			result = -ENOTSUPP; /* rate is not supported by acx100 */
			goto end;
		}
	}

	result = acx_lock(priv, &flags);
	if (result)
		goto end;

	priv->rate_auto = autorate;
	priv->rate_oper = txrate_cfg;
	priv->rate_basic = txrate_cfg;
	if (autorate) /* only do that in auto mode, non-auto will be able to use one specific Tx rate only anyway */
		priv->rate_basic &= RATE111_80211B_COMPAT; /* only use 802.11b base rates, for standard 802.11b H/W compatibility */
	priv->rate_bcast = 1 << highest_bit(txrate_cfg);
	acx_update_dot11_ratevector(priv);
	update_client_rates(priv, txrate_cfg);

	SET_BIT(priv->set_mask, SET_RATE_FALLBACK);
	acx_unlock(priv, &flags);
	result = -EINPROGRESS;
end:
	FN_EXIT1(result);
	return result;
}
/*----------------------------------------------------------------
* acx_ioctl_get_rate
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
static int
acx_ioctl_get_rate(struct net_device *dev,
		struct iw_request_info *info,
		struct iw_param *vwrq,
		char *extra)
{
	/* TODO: remember rate of last tx, show it. think about multiple peers... */

	wlandevice_t *priv = acx_netdev_priv(dev);
	unsigned int n = highest_bit(priv->rate_oper);
 
	if (n >= VEC_SIZE(acx111_rate_tbl)) {
		printk(KERN_ERR "acx_ioctl_get_rate: driver BUG! n=%d. please report\n",n);
		n = 0;
	}

	vwrq->value = acx111_rate_tbl[n];
	vwrq->fixed = !priv->rate_auto;
	vwrq->disabled = 0;
	return OK;
}

static int acx_ioctl_set_rts(struct net_device *dev, struct iw_request_info *info, struct iw_param *vwrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);
	int val = vwrq->value;

	if (vwrq->disabled)
		val = 2312;
	if ((val < 0) || (val > 2312))
		return -EINVAL;

	priv->rts_threshold = val;
	return OK;
}

static inline int acx_ioctl_get_rts(struct net_device *dev, struct iw_request_info *info, struct iw_param *vwrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);

	vwrq->value = priv->rts_threshold;
	vwrq->disabled = (vwrq->value >= 2312);
	vwrq->fixed = 1;
	return OK;
}

/*----------------------------------------------------------------
* acx_ioctl_set_encode
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
static int acx_ioctl_set_encode(struct net_device *dev, struct iw_request_info *info, struct iw_point *dwrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);
	int index;
	unsigned long flags;
	int result;

	FN_ENTER;
	acxlog(L_IOCTL,
	       "Set Encoding flags = 0x%04x, size = %i, key: %s\n",
	       dwrq->flags, dwrq->length, extra ? "set" : "No key");

	result = acx_lock(priv, &flags);
	if (result) {
		goto end;
	}

	index = (dwrq->flags & IW_ENCODE_INDEX) - 1;

	if (dwrq->length > 0) {

		/* if index is 0 or invalid, use default key */
		if ((index < 0) || (index > 3))
			index = (int)priv->wep_current_index;

		if (0 == (dwrq->flags & IW_ENCODE_NOKEY)) {
			if (dwrq->length > 29)
				dwrq->length = 29; /* restrict it */

			if (dwrq->length > 13)
				priv->wep_keys[index].size = 29; /* 29*8 == 232, WEP256 */
			else
			if (dwrq->length > 5)
				priv->wep_keys[index].size = 13; /* 13*8 == 104bit, WEP128 */
			else
			if (dwrq->length > 0)
				priv->wep_keys[index].size = 5; /* 5*8 == 40bit, WEP64 */
			else
				/* disable key */
				priv->wep_keys[index].size = 0;

			memset(priv->wep_keys[index].key, 0, sizeof(priv->wep_keys[index].key));
			memcpy(priv->wep_keys[index].key, extra, dwrq->length);
		}

	} else {
		/* set transmit key */
		if ((index >= 0) && (index <= 3))
			priv->wep_current_index = index;
		else
			if (0 == (dwrq->flags & IW_ENCODE_MODE)) {
				/* complain if we were not just setting
				 * the key mode */
				result =  -EINVAL;
				goto end_unlock;
			}
	}

	priv->wep_enabled = !(dwrq->flags & IW_ENCODE_DISABLED);

	if (dwrq->flags & IW_ENCODE_OPEN) {
		priv->auth_alg = WLAN_AUTH_ALG_OPENSYSTEM;
		priv->wep_restricted = 0;

	} else if (dwrq->flags & IW_ENCODE_RESTRICTED) {
		priv->auth_alg = WLAN_AUTH_ALG_SHAREDKEY;
		priv->wep_restricted = 1;
	}

	/* set flag to make sure the card WEP settings get updated */
	SET_BIT(priv->set_mask, GETSET_WEP);

	acxlog(L_IOCTL, "len = %d, key at 0x%p, flags = 0x%x\n",
	       dwrq->length, extra,
	       dwrq->flags);

	for (index = 0; index <= 3; index++) {
		if (priv->wep_keys[index].size) {
			acxlog(L_IOCTL,
			       "index = %d, size = %d, key at 0x%p\n",
			       priv->wep_keys[index].index,
			       priv->wep_keys[index].size,
			       priv->wep_keys[index].key);
		}
	}
	result = -EINPROGRESS;

end_unlock:
	acx_unlock(priv, &flags);
end:
	FN_EXIT1(result);
	return result;
}

/*----------------------------------------------------------------
* acx_ioctl_get_encode
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
static int acx_ioctl_get_encode(struct net_device *dev, struct iw_request_info *info, struct iw_point *dwrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);
	int index = (dwrq->flags & IW_ENCODE_INDEX) - 1;

	if (priv->wep_enabled == 0) {
		dwrq->flags = IW_ENCODE_DISABLED;

	} else {
		if ((index < 0) || (index > 3))
			index = (int)priv->wep_current_index;

		dwrq->flags =
			(priv->wep_restricted == 1) ? IW_ENCODE_RESTRICTED : IW_ENCODE_OPEN;
		dwrq->length = priv->wep_keys[index].size;

		memcpy(extra,
			     priv->wep_keys[index].key,
			     priv->wep_keys[index].size);
	}

	/* set the current index */
	SET_BIT(dwrq->flags, index + 1);

	acxlog(L_IOCTL, "len = %d, key = %p, flags = 0x%x\n",
	       dwrq->length, dwrq->pointer,
	       dwrq->flags);

	return OK;
}

static int acx_ioctl_set_power(struct net_device *dev, struct iw_request_info *info, struct iw_param *vwrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);

	acxlog(L_IOCTL, "Set 802.11 Power Save flags = 0x%04x\n", vwrq->flags);
	if (vwrq->disabled) {
		CLEAR_BIT(priv->ps_wakeup_cfg, PS_CFG_ENABLE);
		SET_BIT(priv->set_mask, GETSET_POWER_80211);
		return -EINPROGRESS;
	}
	if ((vwrq->flags & IW_POWER_TYPE) == IW_POWER_TIMEOUT) {
		u16 ps_timeout = (vwrq->value * 1024) / 1000;

		if (ps_timeout > 255)
			ps_timeout = 255;
		acxlog(L_IOCTL, "setting PS timeout value to %d time units due to %dus\n", ps_timeout, vwrq->value);
		priv->ps_hangover_period = ps_timeout;
	} else if ((vwrq->flags & IW_POWER_TYPE) == IW_POWER_PERIOD) {
		u16 ps_periods = vwrq->value / 1000000;

		if (ps_periods > 255)
			ps_periods = 255;
		acxlog(L_IOCTL, "setting PS period value to %d periods due to %dus\n", ps_periods, vwrq->value);
		priv->ps_listen_interval = ps_periods;
		CLEAR_BIT(priv->ps_wakeup_cfg, PS_CFG_WAKEUP_MODE_MASK);
		SET_BIT(priv->ps_wakeup_cfg, PS_CFG_WAKEUP_EACH_ITVL);
	}
	switch (vwrq->flags & IW_POWER_MODE) {
		/* FIXME: are we doing the right thing here? */
		case IW_POWER_UNICAST_R:
			CLEAR_BIT(priv->ps_options, PS_OPT_STILL_RCV_BCASTS);
			break;
		case IW_POWER_MULTICAST_R:
			SET_BIT(priv->ps_options, PS_OPT_STILL_RCV_BCASTS);
			break;
		case IW_POWER_ALL_R:
			SET_BIT(priv->ps_options, PS_OPT_STILL_RCV_BCASTS);
			break;
		case IW_POWER_ON:
			break;
		default:
			acxlog(L_IOCTL, "unknown PS mode\n");
			return -EINVAL;
	}

	SET_BIT(priv->ps_wakeup_cfg, PS_CFG_ENABLE);
	SET_BIT(priv->set_mask, GETSET_POWER_80211);

	return -EINPROGRESS;
			
}

static int acx_ioctl_get_power(struct net_device *dev, struct iw_request_info *info, struct iw_param *vwrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);

	acxlog(L_IOCTL, "Get 802.11 Power Save flags = 0x%04x\n", vwrq->flags);
	vwrq->disabled = ((priv->ps_wakeup_cfg & PS_CFG_ENABLE) == 0);
	if (vwrq->disabled)
		return OK;
	if ((vwrq->flags & IW_POWER_TYPE) == IW_POWER_TIMEOUT) {
		vwrq->value = priv->ps_hangover_period * 1000 / 1024;
		vwrq->flags = IW_POWER_TIMEOUT;
	} else {
		vwrq->value = priv->ps_listen_interval * 1000000;
		vwrq->flags = IW_POWER_PERIOD|IW_POWER_RELATIVE;
	}
	if (priv->ps_options & PS_OPT_STILL_RCV_BCASTS)
		SET_BIT(vwrq->flags, IW_POWER_ALL_R);
	else
		SET_BIT(vwrq->flags, IW_POWER_UNICAST_R);

	return OK;
}

/*----------------------------------------------------------------
* acx_ioctl_get_txpow
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
static inline int acx_ioctl_get_txpow(struct net_device *dev, struct iw_request_info *info, struct iw_param *vwrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);

	vwrq->flags = IW_TXPOW_DBM;
	vwrq->disabled = 0;
	vwrq->fixed = 1;
	vwrq->value = priv->tx_level_dbm;

	acxlog(L_IOCTL, "Get Tx power ==> %d dBm\n", priv->tx_level_dbm);

	return OK;
}

/*----------------------------------------------------------------
* acx_ioctl_set_txpow
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
static int acx_ioctl_set_txpow(struct net_device *dev, struct iw_request_info *info, struct iw_param *vwrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);
	unsigned long flags;
	int result;

	FN_ENTER;
	acxlog(L_IOCTL, "Set Tx power <== %d, disabled %d, flags 0x%04x\n", vwrq->value, vwrq->disabled, vwrq->flags);

	result = acx_lock(priv, &flags);
	if (result) {
		goto end;
	}

	if (vwrq->disabled != priv->tx_disabled) {
		SET_BIT(priv->set_mask, GETSET_TX); /* Tx status needs update later */
	}

	priv->tx_disabled = vwrq->disabled;
	if (vwrq->value == -1) {
		if (vwrq->disabled) {
			priv->tx_level_dbm = 0;
			acxlog(L_IOCTL, "Disable radio Tx\n");
		} else {
			priv->tx_level_auto = 1;
			acxlog(L_IOCTL, "Set Tx power auto (NIY)\n");
		}
	} else {
		priv->tx_level_dbm = vwrq->value <= 20 ? vwrq->value : 20;
		priv->tx_level_auto = 0;
		acxlog(L_IOCTL, "Set Tx power = %d dBm\n", priv->tx_level_dbm);
	}
	SET_BIT(priv->set_mask, GETSET_TXPOWER);
	acx_unlock(priv, &flags);
	result = -EINPROGRESS;
end:
	FN_EXIT1(result);
	return result;
}

/*----------------------------------------------------------------
* acx_ioctl_get_range
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
static int acx_ioctl_get_range(struct net_device *dev, struct iw_request_info *info, struct iw_point *dwrq, char *extra)
{
	if (dwrq->pointer != NULL) {
		struct iw_range *range = (struct iw_range *)extra;
		wlandevice_t *priv = acx_netdev_priv(dev);
		unsigned int i;

		dwrq->length = sizeof(struct iw_range);
		memset(range, 0, sizeof(struct iw_range));
		range->num_channels = 0;
		for (i = 1; i <= 14; i++) {
			if (priv->reg_dom_chanmask & (1 << (i - 1))) {
				range->freq[range->num_channels].i = i;
				range->freq[range->num_channels].m = acx_channel_freq[i - 1] * 100000;
				range->freq[range->num_channels++].e = 1; /* MHz values */
			}
		}
		range->num_frequency = range->num_channels;

		range->min_rts = 0;
		range->max_rts = 2312;
		/* range->min_frag = 256;
		 * range->max_frag = 2312;
		 */

		range->encoding_size[0] = 5;
		range->encoding_size[1] = 13;
		range->encoding_size[2] = 29;
		range->num_encoding_sizes = 3;
		range->max_encoding_tokens = 4;
		
		range->min_pmp = 0;
		range->max_pmp = 5000000;
		range->min_pmt = 0;
		range->max_pmt = 65535 * 1000;
		range->pmp_flags = IW_POWER_PERIOD;
		range->pmt_flags = IW_POWER_TIMEOUT;
		range->pm_capa = IW_POWER_PERIOD | IW_POWER_TIMEOUT | IW_POWER_ALL_R;

		for (i = 0; i <= IW_MAX_TXPOWER - 1; i++)
			range->txpower[i] = 20 * i / (IW_MAX_TXPOWER - 1);
		range->num_txpower = IW_MAX_TXPOWER;
		range->txpower_capa = IW_TXPOW_DBM;

		range->we_version_compiled = WIRELESS_EXT;
		range->we_version_source = 0x9;

		range->retry_capa = IW_RETRY_LIMIT;
		range->retry_flags = IW_RETRY_LIMIT;
		range->min_retry = 1;
		range->max_retry = 255;

		range->r_time_flags = IW_RETRY_LIFETIME;
		range->min_r_time = 0;
		range->max_r_time = 65535; /* FIXME: lifetime ranges and orders of magnitude are strange?? */

#if (WLAN_HOSTIF!=WLAN_USB)
		if (CHIPTYPE_ACX111 == priv->chip_type)
			range->sensitivity = 3;
		else
		if (CHIPTYPE_ACX100 == priv->chip_type)
			range->sensitivity = 255;
#else
			range->sensitivity = 0;
#endif

		for (i=0; i < priv->rate_supported_len; i++) {
			range->bitrate[i] = (priv->rate_supported[i] & ~0x80) * 500000;
			/* never happens, but keep it, to be safe: */
			if (range->bitrate[i] == 0)
				break;
		}
		range->num_bitrates = i;

		range->max_qual.qual = 100;
		range->max_qual.level = 100;
		range->max_qual.noise = 100;
		/* TODO: better values */
		range->avg_qual.qual = 90;
		range->avg_qual.level = 80;
		range->avg_qual.noise = 2;
	}

	return OK;
}


/*================================================================*/
/* Private functions						  */
/*================================================================*/

#if WIRELESS_EXT < 13
/*----------------------------------------------------------------
* acx_ioctl_get_iw_priv
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
* STATUS: FINISHED
*
* Comment: I added the monitor mode and changed the stuff below to look more like the orinoco driver
*
*----------------------------------------------------------------*/
static int acx_ioctl_get_iw_priv(struct iwreq *iwr)
{
	int result = -EINVAL;

	if (!iwr->u.data.pointer)
		return -EINVAL;
	result = verify_area(VERIFY_WRITE, iwr->u.data.pointer,
			sizeof(acx_ioctl_private_args));
	if (result)
		return result;

	iwr->u.data.length = VEC_SIZE(acx_ioctl_private_args);
	if (copy_to_user(iwr->u.data.pointer, acx_ioctl_private_args, sizeof(acx_ioctl_private_args)) != 0)
		result = -EFAULT;

	return result;
}
#endif

/*----------------------------------------------------------------
* acx_ioctl_get_nick
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
static inline int acx_ioctl_get_nick(struct net_device *dev, struct iw_request_info *info, struct iw_point *dwrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);

	/* FIXME : consider spinlock here */
	strcpy(extra, priv->nick);
	/* FIXME : consider spinlock here */

	dwrq->length = strlen(extra)+1;

	return OK;
}

/*----------------------------------------------------------------
* acx_ioctl_set_nick
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
* Comment: copied from orinoco.c
*
*----------------------------------------------------------------*/
static int acx_ioctl_set_nick(struct net_device *dev, struct iw_request_info *info, struct iw_point *dwrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);
	unsigned long flags;
	int result;

	FN_ENTER;

	result = acx_lock(priv, &flags);
	if (result) {
		goto end;
	}

	if(dwrq->length > IW_ESSID_MAX_SIZE + 1) {
		result = -E2BIG;
		goto end_unlock;
	}

	/* extra includes trailing \0, so it's ok */
	strcpy(priv->nick, extra);
	result = OK;

end_unlock:
	acx_unlock(priv, &flags);
end:
	FN_EXIT1(result);
	return result;
}

/*------------------------------------------------------------------------------
 * acx_ioctl_get_retry
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
 *----------------------------------------------------------------------------*/
static int acx_ioctl_get_retry(struct net_device *dev,
					 struct iw_request_info *info,
					 struct iw_param *vwrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);
	unsigned int type = vwrq->flags & IW_RETRY_TYPE;
	unsigned int modifier = vwrq->flags & IW_RETRY_MODIFIER;
	unsigned long flags;
	int result;

	result = acx_lock(priv, &flags);
	if (result) {
		goto end;
	}

	/* return the short retry number by default */
	if (type == IW_RETRY_LIFETIME) {
		vwrq->flags = IW_RETRY_LIFETIME;
		vwrq->value = priv->msdu_lifetime;
	} else if (modifier == IW_RETRY_MAX) {
		vwrq->flags = IW_RETRY_LIMIT | IW_RETRY_MAX;
		vwrq->value = priv->long_retry;
	} else {
		vwrq->flags = IW_RETRY_LIMIT;
		if (priv->long_retry != priv->short_retry)
			SET_BIT(vwrq->flags, IW_RETRY_MIN);
		vwrq->value = priv->short_retry;
	}
	acx_unlock(priv, &flags);
	/* can't be disabled */
	vwrq->disabled = (u8)0;
	result = OK;

end:
        return result;
}

/*----------------------------------------------------------------
* acx_ioctl_set_retry
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
static int acx_ioctl_set_retry(struct net_device *dev,
					 struct iw_request_info *info,
					 struct iw_param *vwrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);
	unsigned long flags;
	int result;

	FN_ENTER;

	if (!vwrq) {
		result = -EFAULT;
		goto end;
	}
	if (vwrq->disabled) {
		result = -EINVAL;
		goto end;
	}

	result = acx_lock(priv, &flags);
	if (result) {
		goto end;
	}

	result = -EINVAL;
	if (IW_RETRY_LIMIT == (vwrq->flags & IW_RETRY_TYPE)) {
		printk("old retry limits: short %d long %d\n", priv->short_retry, priv->long_retry);
                if (vwrq->flags & IW_RETRY_MAX) {
                        priv->long_retry = vwrq->value;
                } else if (vwrq->flags & IW_RETRY_MIN) {
                        priv->short_retry = vwrq->value;
                } else {
                        /* no modifier: set both */
                        priv->long_retry = vwrq->value;
                        priv->short_retry = vwrq->value;
                }
		printk("new retry limits: short %d long %d\n", priv->short_retry, priv->long_retry);
		SET_BIT(priv->set_mask, GETSET_RETRY);
		result = -EINPROGRESS;
	}
	else if (vwrq->flags & IW_RETRY_LIFETIME) {
		priv->msdu_lifetime = vwrq->value;
		printk("new MSDU lifetime: %d\n", priv->msdu_lifetime);
		SET_BIT(priv->set_mask, SET_MSDU_LIFETIME);
		result = -EINPROGRESS;
	}
	acx_unlock(priv, &flags);
end:
	FN_EXIT1(result);
	return result;
}


/******************************* private ioctls ******************************/


/*----------------------------------------------------------------
* acx_ioctl_set_debug
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
#if ACX_DEBUG
static int acx_ioctl_set_debug(struct net_device *dev,
					 struct iw_request_info *info,
					 struct iw_param *vwrq, char *extra)
{
	unsigned int debug_new = *((unsigned int *)extra);
	int result = -EINVAL;

	acxlog(0xffff, "%s: setting debug from 0x%04X to 0x%04X\n", __func__,
	       debug, debug_new);
	debug = debug_new;

	result = OK;
	return result;

}
#endif

extern const u8 reg_domain_ids[];
extern const u8 reg_domain_ids_len;

static const char * const reg_domain_strings[] =
{ "FCC (USA)        (1-11)",
  "DOC/IC (Canada)  (1-11)",
	/* BTW: WLAN use in ETSI is regulated by
	 * ETSI standard EN 300 328-2 V1.1.2 */
  "ETSI (Europe)    (1-13)",
  "Spain           (10-11)",
  "France          (10-13)",
  "MKK (Japan)        (14)",
  "MKK1             (1-14)",
  "Israel            (3-9) (not all firmware versions)",
  NULL /* needs to remain as last entry */
};

/*----------------------------------------------------------------
* acx_ioctl_list_reg_domain
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
static int acx_ioctl_list_reg_domain(struct net_device *dev, struct iw_request_info *info, struct iw_param *vwrq, char *extra)
{
	unsigned int i;
	const char * const *entry;

	printk("Domain/Country  Channels  Setting\n");
	for (i = 0, entry = reg_domain_strings; *entry; i++, entry++)
		printk("%s      %d\n", *entry, i+1);
	return OK;
}

/*----------------------------------------------------------------
* acx_ioctl_set_reg_domain
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
static int acx_ioctl_set_reg_domain(struct net_device *dev, struct iw_request_info *info, struct iw_param *vwrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);
	unsigned long flags;
	int result;

	FN_ENTER;

	if ((*extra < 1) || ((size_t)*extra > reg_domain_ids_len)) {
		result = -EINVAL;
		goto end;
	}

	result = acx_lock(priv, &flags);
	if (result) {
		goto end;
	}

	priv->reg_dom_id = reg_domain_ids[*extra - 1];
	SET_BIT(priv->set_mask, GETSET_REG_DOMAIN);
	acx_unlock(priv, &flags);

	result = -EINPROGRESS;
end:
	FN_EXIT1(result);
	return result;
}

/*----------------------------------------------------------------
* acx_ioctl_get_reg_domain
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
static int acx_ioctl_get_reg_domain(struct net_device *dev, struct iw_request_info *info, struct iw_param *vwrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);
	unsigned int i;

	for (i=1; i <= 7; i++) {
		if (reg_domain_ids[i-1] == priv->reg_dom_id) {
			acxlog(L_STD, "regulatory domain is currently set to %d (0x%x): %s\n", i, priv->reg_dom_id, reg_domain_strings[i-1]);
			*extra = i;
			break;
		}
	}

	return OK;
}

/*----------------------------------------------------------------
* acx_ioctl_set_short_preamble
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
static const char * const preamble_modes[] = {
	"off",
	"on",
	"auto (peer capability dependent)",
	"unknown mode, error"
};

static int acx_ioctl_set_short_preamble(struct net_device *dev, struct iw_request_info *info, struct iw_param *vwrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);
	unsigned long flags;
	int i;
	int result;

	FN_ENTER;

	if ((unsigned char)*extra > 2) {
		result = -EINVAL;
		goto end;
	}

	result = acx_lock(priv, &flags);
	if (result) {
		goto end;
	}

	priv->preamble_mode = (u8)*extra;
	switch (priv->preamble_mode) {
	case 0: /* long */
		priv->preamble_cur = 0;
		break;
	case 1:
		/* short, kick incapable peers */
		priv->preamble_cur = 1;
		for(i = 0; i < VEC_SIZE(priv->sta_list); i++) {
			client_t *clt = &priv->sta_list[i];
			if (!clt->used) continue;
			if (!(clt->cap_info & WF_MGMT_CAP_SHORT)) {
				clt->used = CLIENT_EMPTY_SLOT_0;
			}
		}
		switch (priv->mode) {
		case ACX_MODE_2_STA:
			if (priv->ap_client && !priv->ap_client->used) {
				/* We kicked our AP :) */
				SET_BIT(priv->set_mask, GETSET_RESCAN);
			}
		}
		break;
	case 2: /* auto. short only if all peers are short-capable */
		priv->preamble_cur = 1;
		for(i = 0; i < VEC_SIZE(priv->sta_list); i++) {
			client_t *clt = &priv->sta_list[i];
			if (!clt->used) continue;
			if (!(clt->cap_info & WF_MGMT_CAP_SHORT)) {
				priv->preamble_cur = 0;
				break;
			}
		}
		break;
	}
	acx_unlock(priv, &flags);
	printk("new short preamble setting: configured %s, active %s\n",
			preamble_modes[priv->preamble_mode],
			preamble_modes[priv->preamble_cur]);
	result = OK;
end:
	FN_EXIT1(result);
	return result;
}

/*----------------------------------------------------------------
* acx_ioctl_get_short_preamble
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
static int acx_ioctl_get_short_preamble(struct net_device *dev, struct iw_request_info *info, struct iw_param *vwrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);

	printk("current short preamble setting: configured %s, active %s\n",
			preamble_modes[priv->preamble_mode],
			preamble_modes[priv->preamble_cur]);

	*extra = (char)priv->preamble_mode;

	return OK;
}

/*----------------------------------------------------------------
* acx_ioctl_set_antenna
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
* Comment: TX and RX antenna can be set separately but this function good
*          for testing 0-4 bits
*----------------------------------------------------------------*/
static int acx_ioctl_set_antenna(struct net_device *dev, struct iw_request_info *info, struct iw_param *vwrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);

	/* I don't suppose we need to lock priv here */
	printk("old antenna value: 0x%02X (COMBINED bit mask)\n"
		     "Rx antenna selection:\n"
		     "0x00 ant. 1\n"
		     "0x40 ant. 2\n"
		     "0x80 full diversity\n"
		     "0xc0 partial diversity\n"
		     "0x0f dwell time mask (in units of us)\n"
		     "Tx antenna selection:\n"
		     "0x00 ant. 2\n" /* yep, those ARE reversed! */
		     "0x20 ant. 1\n"
		     "new antenna value: 0x%02X\n",
		     priv->antenna, (u8)*extra);

	priv->antenna = (u8)*extra;
	SET_BIT(priv->set_mask, GETSET_ANTENNA);

	return -EINPROGRESS;
}

/*----------------------------------------------------------------
* acx_ioctl_get_antenna
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
static int acx_ioctl_get_antenna(struct net_device *dev, struct iw_request_info *info, struct iw_param *vwrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);

	printk("current antenna value: 0x%02X (COMBINED bit mask)\n"
		     "Rx antenna selection:\n"
		     "0x00 ant. 1\n"
		     "0x40 ant. 2\n"
		     "0x80 full diversity\n"
		     "0xc0 partial diversity\n"
		     "Tx antenna selection:\n"
		     "0x00 ant. 2\n" /* yep, those ARE reversed! */
		     "0x20 ant. 1\n", priv->antenna);

	return 0;
}

/*----------------------------------------------------------------
* acx_ioctl_set_rx_antenna
*
*
* Arguments: 0 == antenna1; 1 == antenna2; 2 == full diversity; 3 == partial diversity
* Returns:
*
* Side effects:
*
* Call context:
*
* STATUS: NEW
*
* Comment: Could anybody test which antenna is the external one
*
*----------------------------------------------------------------*/
static int acx_ioctl_set_rx_antenna(struct net_device *dev, struct iw_request_info *info, struct iw_param *vwrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);
	unsigned long flags;
	int result;

	FN_ENTER;

	if (*extra > 3) {
		result = -EINVAL;
		goto end;
	}

	printk("old antenna value: 0x%02X\n", priv->antenna);
	/* better keep the separate operations atomic */
	result = acx_lock(priv, &flags);
	if (result) {
		goto end;
	}

	priv->antenna &= 0x3f;
	SET_BIT(priv->antenna, (*extra << 6));
	SET_BIT(priv->set_mask, GETSET_ANTENNA);
	acx_unlock(priv, &flags);
	printk("new antenna value: 0x%02X\n", priv->antenna);
	result = -EINPROGRESS;

end:
	FN_EXIT1(result);
	return result;
}

/*----------------------------------------------------------------
* acx_ioctl_set_tx_antenna
*
*
* Arguments: 0 == antenna2; 1 == antenna1;
*
* Returns:
*
* Side effects:
*
* Call context:
*
* STATUS: NEW
*
* Comment: Could anybody test which antenna is the external one
*
*----------------------------------------------------------------*/
static int acx_ioctl_set_tx_antenna(struct net_device *dev, struct iw_request_info *info, struct iw_param *vwrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);
	unsigned long flags;
	int result;

	FN_ENTER;

	if (*extra > 1) {
		result = -EINVAL;
		goto end;
	}

	printk("old antenna value: 0x%02X\n", priv->antenna);
	/* better keep the separate operations atomic */
	result = acx_lock(priv, &flags);
	if (result) {
		goto end;
	}
	priv->antenna &= ~0x30;
	SET_BIT(priv->antenna, ((*extra & 0x01) << 5));
	SET_BIT(priv->set_mask, GETSET_ANTENNA);
	acx_unlock(priv, &flags);
	printk("new antenna value: 0x%02X\n", priv->antenna);
	result = -EINPROGRESS;

end:
	FN_EXIT1(result);
	return result;
}

/*----------------------------------------------------------------
* acx_ioctl_wlansniff
* STATUS: NEW
* can we just remove this in favor of monitor mode? --vda
*----------------------------------------------------------------*/
static int acx_ioctl_wlansniff(struct net_device *dev, struct iw_request_info *info, struct iw_param *vwrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);
	unsigned int *params = (unsigned int*)extra;
	unsigned int enable = (unsigned int)(params[0] > 0);
	unsigned long flags;
	int result;

	FN_ENTER;

	result = acx_lock(priv, &flags);
	if (result) {
		goto end;
	}

	/* not using printk() here, since it distorts kismet display
	 * when printk messages activated */
	acxlog(L_IOCTL, "setting monitor to: 0x%02X\n", params[0]);

	switch (params[0]) {
	case 0:
		priv->netdev->type = ARPHRD_ETHER;
		break;
	case 1:
		priv->netdev->type = ARPHRD_IEEE80211_PRISM;
		break;
	case 2:
		priv->netdev->type = ARPHRD_IEEE80211;
		break;
	}

	if (params[0]) {
		priv->mode = ACX_MODE_MONITOR;
		SET_BIT(priv->set_mask, GETSET_MODE);
	}

	if (enable) {
		priv->channel = params[1];
		SET_BIT(priv->set_mask, GETSET_RX);
	}
	acx_unlock(priv, &flags);
	result = -EINPROGRESS;

end:
	FN_EXIT1(result);
	return result;
}

/*----------------------------------------------------------------
* acx_ioctl_unknown11
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
static int acx_ioctl_unknown11(struct net_device *dev, struct iw_request_info *info, struct iw_param *vwrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);
	unsigned long flags;
	client_t client;
	int result;

	result = acx_lock(priv, &flags);
	if (result) {
		goto end;
	}
	acx_transmit_disassoc(&client, priv);
	acx_unlock(priv, &flags);
	result = OK;

end:
	return result;
}

/* debug helper function to be able to debug various issues relatively easily */
static int acx_ioctl_dbg_set_masks(struct net_device *dev, struct iw_request_info *info, struct iw_param *vwrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);
	const unsigned int *params = (unsigned int*)extra;
	int result;

	acxlog(L_IOCTL, "setting flags in settings mask: get_mask %08x set_mask %08x\n"
	                "before: get_mask %08x set_mask %08x\n",
			params[0], params[1],
			priv->get_mask, priv->set_mask);
	SET_BIT(priv->get_mask, params[0]);
	SET_BIT(priv->set_mask, params[1]);
	acxlog(L_IOCTL, "after:  get_mask %08x set_mask %08x\n", priv->get_mask, priv->set_mask);
	result = -EINPROGRESS; /* immediately call commit handler */

	return result;
}

/* specialized register io for debug purposes, see ioctl below */
static inline u32 _acx_read_reg32(wlandevice_t *priv, unsigned int offset)
{
#if ACX_IO_WIDTH == 32
	return readl(priv->iobase + offset);
#else 
	return readw(priv->iobase + offset)
	    + (readw(priv->iobase + offset + 2) << 16);
#endif
}

static inline void _acx_write_reg32(wlandevice_t *priv, unsigned int offset, u32 val)
{
#if ACX_IO_WIDTH == 32
	writel(val, priv->iobase + offset);
#else 
	writew(val & 0xffff, priv->iobase + offset);
	writew(val >> 16, priv->iobase + offset + 2);
#endif
}

/* debug helper function to be able to debug I/O things relatively easily */
static int acx_ioctl_dbg_get_io(struct net_device *dev, struct iw_request_info *info, struct iw_param *vwrq, char *extra)
{
	int result = -EINVAL;
#if (WLAN_HOSTIF!=WLAN_USB)
	wlandevice_t *priv = acx_netdev_priv(dev);
	unsigned int *params = (unsigned int*)extra;

	/* expected value order: DbgGetIO type address magic */

	if (params[2] != 0x1234) {
		acxlog(L_IOCTL, "wrong magic: 0x%04x doesn't match 0x%04x! If you don't know what you're doing, then please stop NOW, this can be DANGEROUS!!\n", params[2], 0x1234);
		goto end;
	}

	switch (params[0]) {
		case 0x0: /* Internal RAM */
			acxlog(L_IOCTL, "sorry, access to internal RAM not implemented yet.\n");
			break;
		case 0xffff: /* MAC registers */
			acxlog(L_IOCTL, "value at register 0x%04x is 0x%08x\n", params[1], _acx_read_reg32(priv, params[1]));
			break;
		case 0x81: /* PHY RAM table */
			acxlog(L_IOCTL, "sorry, access to PHY RAM not implemented yet.\n");
			break;
		case 0x82: /* PHY registers */
			acxlog(L_IOCTL, "sorry, access to PHY registers not implemented yet.\n");
			break;
		default:
			acxlog(0xffff, "Invalid I/O type specified, aborting!\n");
			goto end;
	}
	result = OK;
end:
#else
	acxlog(L_IOCTL, "ACX100 USB not supported yet!\n");
#endif
	return result;
}

/* debug helper function to be able to debug I/O things relatively easily */
static int acx_ioctl_dbg_set_io(struct net_device *dev, struct iw_request_info *info, struct iw_param *vwrq, char *extra)
{
	int result = -EINVAL;
#if (WLAN_HOSTIF!=WLAN_USB)
	wlandevice_t *priv = acx_netdev_priv(dev);
	int *params = (int*)extra;

	/* expected value order: DbgSetIO type address value magic */

	if (params[3] != 0x1234) {
		acxlog(0xffff, "wrong magic: 0x%04x doesn't match 0x%04x! If you don't know what you're doing, then please stop NOW, this can be DANGEROUS!!\n", params[3], 0x1234);
		goto end;
	}

	switch (params[0]) {
		case 0x0: /* Internal RAM */
			acxlog(L_IOCTL, "sorry, access to internal RAM not implemented yet.\n");
			break;
		case 0xffff: /* MAC registers */
			acxlog(L_IOCTL, "setting value at register 0x%04x to 0x%08x\n", params[1], params[2]);
			_acx_write_reg32(priv, params[1], params[2]);
			break;
		case 0x81: /* PHY RAM table */
			acxlog(L_IOCTL, "sorry, access to PHY RAM not implemented yet.\n");
			break;
		case 0x82: /* PHY registers */
			acxlog(L_IOCTL, "sorry, access to PHY registers not implemented yet.\n");
			break;
		default:
			acxlog(0xffff, "Invalid I/O type specified, aborting!\n");
			goto end;
	}
	result = OK;
end:
#else
	acxlog(L_IOCTL, "ACX100 USB not supported yet!\n");
#endif
	return result;
}

/*----------------------------------------------------------------
* acx111_ioctl_info
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
static int acx111_ioctl_info(struct net_device *dev, struct iw_request_info *info, struct iw_param *vwrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);
	const TIWLAN_DC *pDc = &priv->dc;
	const struct rxdescriptor *pRxDesc = pDc->pRxDescQPool;
	int i;
	int result;
	struct acx111_ie_memoryconfig memconf;
	struct acx111_ie_queueconfig queueconf;
	char memmap[0x34];
	char rxconfig[0x8];
	char fcserror[0x8];
	char ratefallback[0x5];
	const struct rxhostdescriptor *rx_host_desc;
	const struct txdescriptor *tx_desc;
	const struct txhostdescriptor *tx_host_desc;

/*
	result = acx_lock(priv, &flags);
	if (result) {
		goto end;
	}*/

	if (CHIPTYPE_ACX111 != priv->chip_type) {
		acxlog(L_STD | L_IOCTL, "ACX111-specific function called with non-ACX111 chip, aborting!\n");
		return OK;
	}

	/* get Acx111 Memory Configuration */
	memset(&memconf, 0x00, sizeof(memconf));

	if (OK != acx_interrogate(priv, &memconf, ACX1xx_IE_QUEUE_CONFIG)) {
		acxlog(L_STD, "read memconf returns error\n");
	}

	/* get Acx111 Queue Configuration */
	memset(&queueconf, 0x00, sizeof(queueconf));

	if (OK != acx_interrogate(priv, &queueconf, ACX1xx_IE_MEMORY_CONFIG_OPTIONS)) {
		acxlog(L_STD, "read queuehead returns error\n");
	}

	/* get Acx111 Memory Map */
	memset(memmap, 0x00, sizeof(memmap));

	if (OK != acx_interrogate(priv, &memmap, ACX1xx_IE_MEMORY_MAP)) {
		acxlog(L_STD, "read mem map returns error\n");
	}

	/* get Acx111 Rx Config */
	memset(rxconfig, 0x00, sizeof(rxconfig));

	if (OK != acx_interrogate(priv, &rxconfig, ACX1xx_IE_RXCONFIG)) {
		acxlog(L_STD, "read rxconfig returns error\n");
	}
	
	/* get Acx111 fcs error count */
	memset(fcserror, 0x00, sizeof(fcserror));

	if (OK != acx_interrogate(priv, &fcserror, ACX1xx_IE_FCS_ERROR_COUNT)) {
		acxlog(L_STD, "read fcserror returns error\n");
	}
	
	/* get Acx111 rate fallback */
	memset(ratefallback, 0x00, sizeof(ratefallback));

	if (OK != acx_interrogate(priv, &ratefallback, ACX1xx_IE_RATE_FALLBACK)) {
		acxlog(L_STD, "read ratefallback returns error\n");
	}

#if (WLAN_HOSTIF!=WLAN_USB)
	/* force occurrence of a beacon interrupt */
	acx_write_reg16(priv, IO_ACX_HINT_TRIG, 0x20);
#endif

	/* dump Acx111 Mem Configuration */
	acxlog(L_STD, "dump mem config:\n"
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
		      memconf.len, sizeof(memconf),
		      memconf.no_of_stations,
		      memconf.memory_block_size,
		      memconf.tx_rx_memory_block_allocation,
		      memconf.count_rx_queues, memconf.count_tx_queues,
		      memconf.options,
		      memconf.fragmentation,
		      memconf.rx_queue1_count_descs,
		      memconf.rx_queue1_host_rx_start,
		      memconf.tx_queue1_count_descs,
		      memconf.tx_queue1_attributes);

	/* dump Acx111 Queue Configuration */
	acxlog(L_STD, "dump queue head:\n"
		      "data read: %d, struct size: %d\n"
		      "tx_memory_block_address (from card): %X\n"
		      "rx_memory_block_address (from card): %X\n"
		      "rx1_queue address (from card): %X\n"
		      "tx1_queue address (from card): %X\n"
		      "tx1_queue attributes (from card): %X\n",
		      queueconf.len, sizeof(queueconf),
		      queueconf.tx_memory_block_address,
		      queueconf.rx_memory_block_address,
		      queueconf.rx1_queue_address,
		      queueconf.tx1_queue_address,
		      queueconf.tx1_attributes);

	/* dump Acx111 Mem Map */
	acxlog(L_STD, "dump mem map:\n"
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
		      *((u16 *)&memmap[0x02]), sizeof(memmap),
		      *((u32 *)&memmap[0x04]),
		      *((u32 *)&memmap[0x08]),
		      *((u32 *)&memmap[0x0C]),
		      *((u32 *)&memmap[0x10]),
		      *((u32 *)&memmap[0x14]),
		      *((u32 *)&memmap[0x18]),
		      *((u32 *)&memmap[0x1C]),
		      *((u32 *)&memmap[0x20]),
		      *((u32 *)&memmap[0x24]),
		      *((u32 *)&memmap[0x28]),
		      *((u32 *)&memmap[0x2C]),
		      *((u32 *)&memmap[0x30]),
		      priv->iobase,
		      priv->iobase2);

	/* dump Acx111 Rx Config */
	acxlog(L_STD, "dump rx config:\n"
	              "data read: %d, struct size: %d\n"
	              "rx config: %X\n"
	              "rx filter config: %X\n",
		      *((u16 *)&rxconfig[0x02]), sizeof(rxconfig),
		      *((u16 *)&rxconfig[0x04]),
		      *((u16 *)&rxconfig[0x06]));

	/* dump Acx111 fcs error */
	acxlog(L_STD, "dump fcserror:\n"
	              "data read: %d, struct size: %d\n"
	              "fcserrors: %X\n",
		      *((u16 *)&fcserror[0x02]), sizeof(fcserror),
		      *((u32 *)&fcserror[0x04]));

	/* dump Acx111 rate fallback */
	acxlog(L_STD, "dump rate fallback:\n"
	              "data read: %d, struct size: %d\n"
	              "ratefallback: %X\n",
		      *((u16 *)&ratefallback[0x02]), sizeof(ratefallback),
		      *((u8 *)&ratefallback[0x04]));

	/* dump acx111 internal rx descriptor ring buffer */

	/* loop over complete receive pool */
	for (i = 0; i < pDc->rx_pool_count; i++) {

		acxlog(L_STD, "\ndump internal rxdescriptor %d:\n"
		              "mem pos %p\n"
		              "next 0x%X\n"
		              "acx mem pointer (dynamic) 0x%X\n"
		              "CTL (dynamic) 0x%X\n"
		              "Rate (dynamic) 0x%X\n"
		              "RxStatus (dynamic) 0x%X\n"
		              "Mod/Pre (dynamic) 0x%X\n",
			      i,
			      pRxDesc,
			      le32_to_cpu(pRxDesc->pNextDesc),
			      le32_to_cpu(pRxDesc->ACXMemPtr),
			      pRxDesc->Ctl_8,
			      pRxDesc->rate,
			      pRxDesc->error,
			      pRxDesc->SNR);

		pRxDesc++;
	}

	/* dump host rx descriptor ring buffer */

	rx_host_desc = (struct rxhostdescriptor *) pDc->pRxHostDescQPool;

	/* loop over complete receive pool */
	for (i = 0; i < pDc->rx_pool_count; i++) {

		acxlog(L_STD, "\ndump host rxdescriptor %d:\n"
		              "mem pos %p\n"
		              "buffer mem pos 0x%X\n"
		              "buffer mem offset 0x%X\n"
		              "CTL 0x%X\n"
		              "Length 0x%X\n"
		              "next 0x%X\n"
		              "Status 0x%X\n",
			      i,
			      rx_host_desc,
			      (u32)le32_to_cpu(rx_host_desc->data_phy),
			      rx_host_desc->data_offset,
			      le16_to_cpu(rx_host_desc->Ctl_16),
			      le16_to_cpu(rx_host_desc->length),
			      (u32)rx_host_desc->desc_phy_next,
			      rx_host_desc->Status);

		rx_host_desc++;

	}

	/* dump acx111 internal tx descriptor ring buffer */
	tx_desc = pDc->pTxDescQPool;

	/* loop over complete transmit pool */
	for (i = 0; i < pDc->tx_pool_count; i++) {

		acxlog(L_STD, "\ndump internal txdescriptor %d:\n"
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
			      sizeof(struct txdescriptor),
			      tx_desc,
			      le32_to_cpu(tx_desc->pNextDesc),
			      le32_to_cpu(tx_desc->AcxMemPtr),
			      le32_to_cpu(tx_desc->HostMemPtr),
			      le16_to_cpu(tx_desc->total_length),
			      tx_desc->Ctl_8,
			      tx_desc->Ctl2_8, tx_desc->error,
			      tx_desc->u.r1.rate);

		tx_desc = GET_NEXT_TX_DESC_PTR(pDc, tx_desc);
	}


	/* dump host tx descriptor ring buffer */

	tx_host_desc = (struct txhostdescriptor *) pDc->pTxHostDescQPool;

	/* loop over complete host send pool */
	for (i = 0; i < pDc->tx_pool_count * 2; i++) {

		acxlog(L_STD, "\ndump host txdescriptor %d:\n"
		              "mem pos %p\n"
		              "buffer mem pos 0x%X\n"
		              "buffer mem offset 0x%X\n"
		              "CTL 0x%X\n"
		              "Length 0x%X\n"
		              "next 0x%X\n"
		              "Status 0x%X\n",
			      i,
			      tx_host_desc,
			      (u32)le32_to_cpu(tx_host_desc->data_phy),
			      tx_host_desc->data_offset,
			      le16_to_cpu(tx_host_desc->Ctl_16),
			      le16_to_cpu(tx_host_desc->length),
			      (u32)le32_to_cpu(tx_host_desc->desc_phy_next),
			      le32_to_cpu(tx_host_desc->Status));

		tx_host_desc++;

	}

	/* acx_write_reg16(priv, 0xb4, 0x4); */

	/* acx_unlock(priv, &flags); */
	result = OK;

	return result;
}


/*----------------------------------------------------------------
* acx_ioctl_set_rates
* This ioctl takes string parameter. Examples:
* iwpriv wlan0 SetRates "1,2"
*	use 1 and 2 Mbit rates, both are in basic rate set
* iwpriv wlan0 SetRates "1,2 5,11"
*	use 1,2,5.5,11 Mbit rates. 1 and 2 are basic
* iwpriv wlan0 SetRates "1,2 5c,11c"
*	same ('c' means 'CCK modulation' and it is a default for 5 and 11)
* iwpriv wlan0 SetRates "1,2 5p,11p"
*	use 1,2,5.5,11 Mbit, 1,2 are basic. 5 and 11 are using PBCC
* iwpriv wlan0 SetRates "1,2,5,11 22p"
*	use 1,2,5.5,11,22 Mbit. 1,2,5.5 and 11 are basic. 22 is using PBCC
*	(this is the maximum acx100 can do (modulo x4 mode))
* iwpriv wlan0 SetRates "1,2,5,11 22"
*	same. 802.11 defines only PBCC modulation
*	for 22 and 33 Mbit rates, so there is no ambiguity
* iwpriv wlan0 SetRates "1,2,5,11 6o,9o,12o,18o,24o,36o,48o,54o"
*	1,2,5.5 and 11 are basic. 11g OFDM rates are enabled but
*	they are not in basic rate set.	22 Mbit is disabled.
* iwpriv wlan0 SetRates "1,2,5,11 6,9,12,18,24,36,48,54"
*	same. OFDM is default for 11g rates except 22 and 33 Mbit,
*	thus 'o' is optional
* iwpriv wlan0 SetRates "1,2,5,11 6d,9d,12d,18d,24d,36d,48d,54d"
*	1,2,5.5 and 11 are basic. 11g CCK-OFDM rates are enabled
*	(acx111 does not support CCK-OFDM, driver will reject this cmd)
* iwpriv wlan0 SetRates "6,9,12 18,24,36,48,54"
*	6,9,12 are basic, rest of 11g rates is enabled. Using OFDM
*----------------------------------------------------------------*/
#include "setrate.c"

/* disallow: 33Mbit (unsupported by hw) */
/* disallow: CCKOFDM (unsupported by hw) */
static int
acx111_supported(int mbit, int modulation, void *opaque)
{
	if(mbit==33) return -ENOTSUPP;
	if(modulation==DOT11_MOD_CCKOFDM) return -ENOTSUPP;
	return OK;
}

static const u16
acx111mask[] = {
	[DOT11_RATE_1 ] = RATE111_1 ,
	[DOT11_RATE_2 ] = RATE111_2 ,
	[DOT11_RATE_5 ] = RATE111_5 ,
	[DOT11_RATE_11] = RATE111_11,
	[DOT11_RATE_22] = RATE111_22,
	/* [DOT11_RATE_33] = */
	[DOT11_RATE_6 ] = RATE111_6 ,
	[DOT11_RATE_9 ] = RATE111_9 ,
	[DOT11_RATE_12] = RATE111_12,
	[DOT11_RATE_18] = RATE111_18,
	[DOT11_RATE_24] = RATE111_24,
	[DOT11_RATE_36] = RATE111_36,
	[DOT11_RATE_48] = RATE111_48,
	[DOT11_RATE_54] = RATE111_54,
};

static u32
acx111_gen_mask(int mbit, int modulation, void *opaque)
{
	/* lower 16 bits show selected 1, 2, CCK and OFDM rates */
	/* upper 16 bits show selected PBCC rates */
	u32 m = acx111mask[rate_mbit2enum(mbit)];
	if(modulation==DOT11_MOD_PBCC)
		return m<<16;
	return m;
}

static int
verify_rate(u32 rate, int chip_type)
{
	/* never happens. be paranoid */
	if(!rate) return -EINVAL;
 
	/* disallow: mixing PBCC and CCK at 5 and 11Mbit
	** (can be supported, but needs complicated handling in tx code) */
	if( ( rate & ((RATE111_11+RATE111_5)<<16) )
	&&  ( rate & (RATE111_11+RATE111_5) )
	) {
		return -ENOTSUPP;
	}
	if (CHIPTYPE_ACX100 == chip_type) {
		if( rate & ~(RATE111_ACX100_COMPAT+(RATE111_ACX100_COMPAT<<16)) )
			return -ENOTSUPP;
	}
	return 0;
}
 
static int
acx_ioctl_set_rates(struct net_device *dev, struct iw_request_info *info,
		 struct iw_param *vwrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);
	unsigned long flags;
	int result;
	u32 brate = 0, orate = 0; /* basic, operational rate set */

	FN_ENTER;

	acxlog(L_IOCTL, "set_rates %s\n", extra);
	result = fill_ratemasks(extra, &brate, &orate, acx111_supported, acx111_gen_mask, 0);
	if (result) goto end;
	SET_BIT(orate, brate);
	acxlog(L_IOCTL, "brate %08x orate %08x\n", brate, orate);

	result = verify_rate(brate, priv->chip_type);
	if (result) goto end;
	result = verify_rate(orate, priv->chip_type);
	if (result) goto end;

	result = acx_lock(priv, &flags);
	if (result) goto end;

	priv->rate_basic = brate;
	priv->rate_oper = orate;
	/* TODO: ideally, we shall monitor highest basic rate
	** which was successfully sent to every peer
	** (say, last we checked, everybody could hear 5.5 Mbits)
	** and use that for bcasts when we want to reach all peers.
	** For beacons, we probably shall use lowest basic rate
	** because we want to reach all *potential* new peers too */
	priv->rate_bcast = 1 << highest_bit(brate);
	priv->rate_auto = !has_only_one_bit(orate);
	update_client_rates(priv, orate);
	/* TODO: get rid of ratevector, build it only when needed */
	acx_update_dot11_ratevector(priv);

	SET_BIT(priv->set_mask, SET_RATE_FALLBACK);
	acx_unlock(priv, &flags);
	result = -EINPROGRESS;
end:
	FN_EXIT1(result);
	return result;
}

/*----------------------------------------------------------------
* acx100_ioctl_set_phy_amp_bias
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
static int acx100_ioctl_set_phy_amp_bias(struct net_device *dev, struct iw_request_info *info, struct iw_param *vwrq, char *extra)
{
#if (WLAN_HOSTIF!=WLAN_USB)
	wlandevice_t *priv = acx_netdev_priv(dev);
	u16 gpio_old;

	if (priv->chip_type != CHIPTYPE_ACX100) {
		/* WARNING!!!
		 * Removing this check *might* damage
		 * hardware, since we're tweaking GPIOs here after all!!!
		 * You've been warned...
		 * WARNING!!! */
		acxlog(L_IOCTL, "sorry, setting bias level for non-ACX100 not supported yet.\n");
		return 0;
	}
	
	if (*extra > 7)	{
		acxlog(0xffff, "invalid bias parameter, range is 0-7\n");
		return -EINVAL;
	}

	gpio_old = acx_read_reg16(priv, IO_ACX_GPIO_OUT);
	acxlog(L_DEBUG, "gpio_old: 0x%04x\n", gpio_old);
	printk("old PHY power amplifier bias: %d\n", (gpio_old & 0x0700) >> 8);
	acx_write_reg16(priv, IO_ACX_GPIO_OUT, (gpio_old & 0xf8ff) | ((u16)*extra << 8));
	printk("new PHY power amplifier bias: %d\n", (unsigned char)*extra);
#else
	acxlog(L_IOCTL, "ACX100 USB not supported yet!\n");
#endif
	return 0;
}

/*----------------------------------------------------------------
* acx_ioctl_get_phy_chan_busy_percentage
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
static int acx_ioctl_get_phy_chan_busy_percentage(struct net_device *dev, struct iw_request_info *info, struct iw_param *vwrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);
	struct {
		u16 type;
		u16 len;
		u32 busytime;
		u32 totaltime;
	} usage;

	if (OK != acx_interrogate(priv, &usage, ACX1xx_IE_MEDIUM_USAGE))
		return NOT_OK;
	printk("Medium busy percentage since last invocation: %d%% (microseconds: %u of %u)\n", 100 * (le32_to_cpu(usage.busytime) / 100) / (le32_to_cpu(usage.totaltime) / 100), le32_to_cpu(usage.busytime), le32_to_cpu(usage.totaltime)); /* prevent calculation overflow */
	return OK;
}

/*----------------------------------------------------------------
* acx_ioctl_set_ed_threshold
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
static inline int acx_ioctl_set_ed_threshold(struct net_device *dev, struct iw_request_info *info, struct iw_param *vwrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);

	printk("old ED threshold value: %d\n", priv->ed_threshold);
	priv->ed_threshold = (unsigned char)*extra;
	printk("new ED threshold value: %d\n", (unsigned char)*extra);
	SET_BIT(priv->set_mask, GETSET_ED_THRESH);

	return -EINPROGRESS;
}

/*----------------------------------------------------------------
* acx_ioctl_set_cca
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
static inline int acx_ioctl_set_cca(struct net_device *dev, struct iw_request_info *info, struct iw_param *vwrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);
	unsigned long flags;
	int result;

	result = acx_lock(priv, &flags);
	if (result) {
		goto end;
	}

	printk("old CCA value: 0x%02X\n", priv->cca);
	priv->cca = (unsigned char)*extra;
	printk("new CCA value: 0x%02X\n", (unsigned char)*extra);
	SET_BIT(priv->set_mask, GETSET_CCA);
	acx_unlock(priv, &flags);
	result = -EINPROGRESS;

end:
	return result;
}

static const char * const scan_modes[] = { "active", "passive", "background" };
static int acx_ioctl_set_scan_params(struct net_device *dev, struct iw_request_info *info, struct iw_param *vwrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);
	unsigned long flags;
	int result;
	const int *params = (int *)extra;

	result = acx_lock(priv, &flags);
	if (result) {
		goto end;
	}

	printk("old scan parameters: mode %d (%s), min chan time %dTU, max chan time %dTU, max scan rate byte: %d\n", priv->scan_mode, scan_modes[priv->scan_mode], priv->scan_probe_delay, priv->scan_duration, priv->scan_rate);
	if ((params[0] != -1) && (params[0] >= 0) && (params[0] <= 2))
		priv->scan_mode = params[0];
	if (params[1] != -1)
		priv->scan_probe_delay = params[1];
	if (params[2] != -1)
		priv->scan_duration = params[2];
	if ((params[3] != -1) && (params[3] <= 255))
		priv->scan_rate = params[3];
	printk("new scan parameters: mode %d (%s), min chan time %dTU, max chan time %dTU, max scan rate byte: %d\n", priv->scan_mode, scan_modes[priv->scan_mode], priv->scan_probe_delay, priv->scan_duration, priv->scan_rate);
	SET_BIT(priv->set_mask, GETSET_RESCAN);
	acx_unlock(priv, &flags);
	result = -EINPROGRESS;

end:
	return result;
}

static int acx_ioctl_get_scan_params(struct net_device *dev, struct iw_request_info *info, struct iw_param *vwrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);
	unsigned long flags;
	int result;
	int *params = (int *)extra;

	result = acx_lock(priv, &flags);
	if (result) {
		goto end;
	}

	printk("current scan parameters: mode %d (%s), min chan time %dTU, max chan time %dTU, max scan rate byte: %d\n", priv->scan_mode, scan_modes[priv->scan_mode], priv->scan_probe_delay, priv->scan_duration, priv->scan_rate);

	params[0] = priv->scan_mode;
	params[1] = priv->scan_probe_delay;
	params[2] = priv->scan_duration;
	params[3] = priv->scan_rate;

	acx_unlock(priv, &flags);
	result = OK;

end:
	return result;
}

static const char * const led_modes[] = { "off", "on", "LinkQuality" };
static int acx100_ioctl_set_led_power(struct net_device *dev, struct iw_request_info *info, struct iw_param *vwrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);
	unsigned long flags;
	int result;

	result = acx_lock(priv, &flags);
	if (result) {
		goto end;
	}
	printk("old power LED status: %d (%s)\n", priv->led_power, led_modes[priv->led_power]);
	priv->led_power = (unsigned char)extra[0];
	if (priv->led_power == 2) {
		printk("old max link quality setting: %d\n", priv->brange_max_quality);
		if (extra[1])
			priv->brange_max_quality = (unsigned char)extra[1];
	}
	printk("new power LED status: %d (%s)\n", priv->led_power, led_modes[priv->led_power]);
	if (priv->led_power == 2)
		printk("new max link quality setting: %d\n", priv->brange_max_quality);
	SET_BIT(priv->set_mask, GETSET_LED_POWER);

	acx_unlock(priv, &flags);
	result = -EINPROGRESS;

end:
	return result;
}

static inline int acx100_ioctl_get_led_power(struct net_device *dev, struct iw_request_info *info, struct iw_param *vwrq, char *extra)
{
	wlandevice_t *priv = acx_netdev_priv(dev);
	extra[0] = priv->led_power;
	if (priv->led_power == 2)
		extra[1] = priv->brange_max_quality;
	else
		extra[1] = -1;
	return OK;
}


#if WIRELESS_EXT >= 13
static const iw_handler acx_ioctl_handler[] =
{
	(iw_handler) acx_ioctl_commit,		/* SIOCSIWCOMMIT */
	(iw_handler) acx_ioctl_get_name,	/* SIOCGIWNAME */
	(iw_handler) NULL,			/* SIOCSIWNWID */
	(iw_handler) NULL,			/* SIOCGIWNWID */
	(iw_handler) acx_ioctl_set_freq,	/* SIOCSIWFREQ */
	(iw_handler) acx_ioctl_get_freq,	/* SIOCGIWFREQ */
	(iw_handler) acx_ioctl_set_mode,	/* SIOCSIWMODE */
	(iw_handler) acx_ioctl_get_mode,	/* SIOCGIWMODE */
	(iw_handler) acx_ioctl_set_sens,	/* SIOCSIWSENS */
	(iw_handler) acx_ioctl_get_sens,	/* SIOCGIWSENS */
	(iw_handler) NULL,			/* SIOCSIWRANGE */
	(iw_handler) acx_ioctl_get_range,	/* SIOCGIWRANGE */
	(iw_handler) NULL,			/* SIOCSIWPRIV */
	(iw_handler) NULL,			/* SIOCGIWPRIV */
	(iw_handler) NULL,			/* SIOCSIWSTATS */
	(iw_handler) NULL,			/* SIOCGIWSTATS */
#if IW_HANDLER_VERSION > 4
	iw_handler_set_spy,			/* SIOCSIWSPY */
	iw_handler_get_spy,			/* SIOCGIWSPY */
	iw_handler_set_thrspy,			/* SIOCSIWTHRSPY */
	iw_handler_get_thrspy,			/* SIOCGIWTHRSPY */
#else /* IW_HANDLER_VERSION > 4 */
#ifdef WIRELESS_SPY
	(iw_handler) NULL /* acx_ioctl_set_spy FIXME */,	/* SIOCSIWSPY */
	(iw_handler) NULL /* acx_ioctl_get_spy FIXME */,	/* SIOCGIWSPY */
#else /* WSPY */
	(iw_handler) NULL,			/* SIOCSIWSPY */
	(iw_handler) NULL,			/* SIOCGIWSPY */
#endif /* WSPY */
	(iw_handler) NULL,			/* [nothing] */
	(iw_handler) NULL,			/* [nothing] */
#endif /* IW_HANDLER_VERSION > 4 */
	(iw_handler) acx_ioctl_set_ap,		/* SIOCSIWAP */
	(iw_handler) acx_ioctl_get_ap,		/* SIOCGIWAP */
	(iw_handler) NULL,			/* [nothing] */
	(iw_handler) acx_ioctl_get_aplist,	/* SIOCGIWAPLIST */
#if WIRELESS_EXT > 13
	(iw_handler) acx_ioctl_set_scan,	/* SIOCSIWSCAN */
	(iw_handler) acx_ioctl_get_scan,	/* SIOCGIWSCAN */
#else /* WE > 13 */
	(iw_handler) NULL,			/* SIOCSIWSCAN */
	(iw_handler) NULL,			/* SIOCGIWSCAN */
#endif /* WE > 13 */
	(iw_handler) acx_ioctl_set_essid,	/* SIOCSIWESSID */
	(iw_handler) acx_ioctl_get_essid,	/* SIOCGIWESSID */
	(iw_handler) acx_ioctl_set_nick,	/* SIOCSIWNICKN */
	(iw_handler) acx_ioctl_get_nick,	/* SIOCGIWNICKN */
	(iw_handler) NULL,			/* [nothing] */
	(iw_handler) NULL,			/* [nothing] */
	(iw_handler) acx_ioctl_set_rate,	/* SIOCSIWRATE */
	(iw_handler) acx_ioctl_get_rate,	/* SIOCGIWRATE */
	(iw_handler) acx_ioctl_set_rts,		/* SIOCSIWRTS */
	(iw_handler) acx_ioctl_get_rts,		/* SIOCGIWRTS */
	(iw_handler) NULL /* acx_ioctl_set_frag FIXME */,	/* SIOCSIWFRAG */
	(iw_handler) NULL /* acx_ioctl_get_frag FIXME */,	/* SIOCGIWFRAG */
	(iw_handler) acx_ioctl_set_txpow,	/* SIOCSIWTXPOW */
	(iw_handler) acx_ioctl_get_txpow,	/* SIOCGIWTXPOW */
	(iw_handler) acx_ioctl_set_retry,	/* SIOCSIWRETRY */
	(iw_handler) acx_ioctl_get_retry,	/* SIOCGIWRETRY */
	(iw_handler) acx_ioctl_set_encode,	/* SIOCSIWENCODE */
	(iw_handler) acx_ioctl_get_encode,	/* SIOCGIWENCODE */
	(iw_handler) acx_ioctl_set_power,	/* SIOCSIWPOWER */
	(iw_handler) acx_ioctl_get_power,	/* SIOCGIWPOWER */
};

static const iw_handler acx_ioctl_private_handler[] =
{
#if ACX_DEBUG
[ACX100_IOCTL_DEBUG			- ACX100_IOCTL] = (iw_handler) acx_ioctl_set_debug,
#else
[ACX100_IOCTL_DEBUG			- ACX100_IOCTL] = (iw_handler) NULL,
#endif
[ACX100_IOCTL_SET_PLED           	- ACX100_IOCTL] = (iw_handler) acx100_ioctl_set_led_power,
[ACX100_IOCTL_GET_PLED			- ACX100_IOCTL] = (iw_handler) acx100_ioctl_get_led_power,
[ACX100_IOCTL_SET_RATES          	- ACX100_IOCTL] = (iw_handler) acx_ioctl_set_rates,
[ACX100_IOCTL_LIST_DOM           	- ACX100_IOCTL] = (iw_handler) acx_ioctl_list_reg_domain,
[ACX100_IOCTL_SET_DOM            	- ACX100_IOCTL] = (iw_handler) acx_ioctl_set_reg_domain,
[ACX100_IOCTL_GET_DOM            	- ACX100_IOCTL] = (iw_handler) acx_ioctl_get_reg_domain,
[ACX100_IOCTL_SET_SCAN_PARAMS      	- ACX100_IOCTL] = (iw_handler) acx_ioctl_set_scan_params,
[ACX100_IOCTL_GET_SCAN_PARAMS      	- ACX100_IOCTL] = (iw_handler) acx_ioctl_get_scan_params,
[ACX100_IOCTL_SET_PREAMB         	- ACX100_IOCTL] = (iw_handler) acx_ioctl_set_short_preamble,
[ACX100_IOCTL_GET_PREAMB         	- ACX100_IOCTL] = (iw_handler) acx_ioctl_get_short_preamble,
[ACX100_IOCTL_SET_ANT            	- ACX100_IOCTL] = (iw_handler) acx_ioctl_set_antenna,
[ACX100_IOCTL_GET_ANT            	- ACX100_IOCTL] = (iw_handler) acx_ioctl_get_antenna,
[ACX100_IOCTL_RX_ANT             	- ACX100_IOCTL] = (iw_handler) acx_ioctl_set_rx_antenna,
[ACX100_IOCTL_TX_ANT             	- ACX100_IOCTL] = (iw_handler) acx_ioctl_set_tx_antenna,
[ACX100_IOCTL_SET_PHY_AMP_BIAS   	- ACX100_IOCTL] = (iw_handler) acx100_ioctl_set_phy_amp_bias,
[ACX100_IOCTL_GET_PHY_CHAN_BUSY		- ACX100_IOCTL] = (iw_handler) acx_ioctl_get_phy_chan_busy_percentage,
[ACX100_IOCTL_SET_ED             	- ACX100_IOCTL] = (iw_handler) acx_ioctl_set_ed_threshold,
[ACX100_IOCTL_SET_CCA            	- ACX100_IOCTL] = (iw_handler) acx_ioctl_set_cca,
[ACX100_IOCTL_MONITOR            	- ACX100_IOCTL] = (iw_handler) acx_ioctl_wlansniff,
[ACX100_IOCTL_TEST               	- ACX100_IOCTL] = (iw_handler) acx_ioctl_unknown11,
[ACX100_IOCTL_DBG_SET_MASKS      	- ACX100_IOCTL] = (iw_handler) acx_ioctl_dbg_set_masks,
[ACX111_IOCTL_INFO			- ACX100_IOCTL] = (iw_handler) acx111_ioctl_info,
[ACX100_IOCTL_DBG_SET_IO         	- ACX100_IOCTL] = (iw_handler) acx_ioctl_dbg_set_io,
[ACX100_IOCTL_DBG_GET_IO         	- ACX100_IOCTL] = (iw_handler) acx_ioctl_dbg_get_io,
};

const struct iw_handler_def acx_ioctl_handler_def =
{
	.num_standard = VEC_SIZE(acx_ioctl_handler),
	.num_private = VEC_SIZE(acx_ioctl_private_handler),
	.num_private_args = VEC_SIZE(acx_ioctl_private_args),
	.standard = (iw_handler *) acx_ioctl_handler,
	.private = (iw_handler *) acx_ioctl_private_handler,
	.private_args = (struct iw_priv_args *) acx_ioctl_private_args,
#if WIRELESS_EXT > 15
	.spy_offset = ((void *) (&((struct wlandevice *) NULL)->spy_data) - (void *) NULL),
#endif /* WE > 15 */
};

#endif /* WE >= 13 */



#if WIRELESS_EXT < 13
/*================================================================*/
/* Main function						  */
/*================================================================*/
/*----------------------------------------------------------------
* acx_ioctl_old
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
* STATUS: FINISHED
*
* Comment:
* This is the *OLD* ioctl handler.
* Make sure to not only place your additions here, but instead mainly
* in the new one (acx_ioctl_handler[])!
* FIXME: remove commented #ifdefs, since they're duplicates?
*
*----------------------------------------------------------------*/
int acx_ioctl_old(netdevice_t *dev, struct ifreq *ifr, int cmd)
{
	wlandevice_t *priv = acx_netdev_priv(dev);
	int result = 0;
/* #if WIRELESS_EXT < 13 [see above] */
	struct iwreq *iwr = (struct iwreq *)ifr;
/* #endif */

	acxlog(L_IOCTL, "%s cmd = 0x%04X\n", __func__, cmd);

	/* This is the way it is done in the orinoco driver.
	 * Check to see if device is present.
	 */
	if (0 == netif_device_present(dev)) {
		return -ENODEV;
	}

	switch (cmd) {
/* WE 13 and higher will use acx_ioctl_handler_def */
/* #if WIRELESS_EXT < 13 [see above] */
	case SIOCGIWNAME:
		/* get name == wireless protocol */
		result = acx_ioctl_get_name(dev, NULL,
					       (char *)&(iwr->u.name), NULL);
		break;

	case SIOCSIWNWID: /* pre-802.11, */
	case SIOCGIWNWID: /* not supported. */
		result = -EOPNOTSUPP;
		break;

	case SIOCSIWFREQ:
		/* set channel/frequency (Hz)
		   data can be frequency or channel :
		   0-1000 = channel
		   > 1000 = frequency in Hz */
		result = acx_ioctl_set_freq(dev, NULL, &(iwr->u.freq), NULL);
		break;

	case SIOCGIWFREQ:
		/* get channel/frequency (Hz) */
		result = acx_ioctl_get_freq(dev, NULL, &(iwr->u.freq), NULL);
		break;

	case SIOCSIWMODE:
		/* set operation mode */
		result = acx_ioctl_set_mode(dev, NULL, &(iwr->u.mode), NULL);
		break;

	case SIOCGIWMODE:
		/* get operation mode */
		result = acx_ioctl_get_mode(dev, NULL, &(iwr->u.mode), NULL);
		break;

	case SIOCSIWSENS:
		/* Set sensitivity */
		result = acx_ioctl_set_sens(dev, NULL, &(iwr->u.sens), NULL);
		break; 

	case SIOCGIWSENS:
		/* Get sensitivity */
		result = acx_ioctl_get_sens(dev, NULL, &(iwr->u.sens), NULL);
		break;

#if WIRELESS_EXT > 10
	case SIOCGIWRANGE:
		/* Get range of parameters */
		{
			struct iw_range range;
			result = acx_ioctl_get_range(dev, NULL,
					&(iwr->u.data), (char *)&range);
			if (copy_to_user(iwr->u.data.pointer, &range,
					 sizeof(struct iw_range)))
				result = -EFAULT;
		}
		break;
#endif

	case SIOCGIWPRIV:
		result = acx_ioctl_get_iw_priv(iwr);
		break;

	/* FIXME: */
	/* case SIOCSIWSPY: */
	/* case SIOCGIWSPY: */
	/* case SIOCSIWTHRSPY: */
	/* case SIOCGIWTHRSPY: */

	case SIOCSIWAP:
		/* set access point by MAC address */
		result = acx_ioctl_set_ap(dev, NULL, &(iwr->u.ap_addr),
					     NULL);
		break;

	case SIOCGIWAP:
		/* get access point MAC address */
		result = acx_ioctl_get_ap(dev, NULL, &(iwr->u.ap_addr),
					     NULL);
		break;

	case SIOCGIWAPLIST:
		/* get list of access points in range */
		result = acx_ioctl_get_aplist(dev, NULL, &(iwr->u.data),
						 NULL);
		break;

#if NOT_FINISHED_YET
	/* FIXME: do proper interfacing to activate that! */
	case SIOCSIWSCAN:
		/* start a station scan */
		result = acx_ioctl_set_scan(iwr, priv);
		break;

	case SIOCGIWSCAN:
		/* get list of stations found during scan */
		result = acx_ioctl_get_scan(iwr, priv);
		break;
#endif

	case SIOCSIWESSID:
		/* set ESSID (network name) */
		{
			char essid[IW_ESSID_MAX_SIZE+1];

			if (iwr->u.essid.length > IW_ESSID_MAX_SIZE)
			{
				result = -E2BIG;
				break;
			}
			if (copy_from_user(essid, iwr->u.essid.pointer,
						iwr->u.essid.length))
			{
				result = -EFAULT;
				break;
			}
			result = acx_ioctl_set_essid(dev, NULL,
					&(iwr->u.essid), essid);
		}
		break;

	case SIOCGIWESSID:
		/* get ESSID */
		{
			char essid[IW_ESSID_MAX_SIZE+1];
			if (iwr->u.essid.pointer)
				result = acx_ioctl_get_essid(dev, NULL,
					&(iwr->u.essid), essid);
			if (copy_to_user(iwr->u.essid.pointer, essid,
						iwr->u.essid.length))
				result = -EFAULT;
		}
		break;

	case SIOCSIWNICKN:
		/* set nick */
		{
			char nick[IW_ESSID_MAX_SIZE+1];

			if (iwr->u.data.length > IW_ESSID_MAX_SIZE)
			{
				result = -E2BIG;
				break;
			}
			if (copy_from_user(nick, iwr->u.data.pointer,
						iwr->u.data.length))
			{
				result = -EFAULT;
				break;
			}
			result = acx_ioctl_set_nick(dev, NULL,
					&(iwr->u.data), nick);
		}
		break;

	case SIOCGIWNICKN:
		/* get nick */
		{
			char nick[IW_ESSID_MAX_SIZE+1];
			if (iwr->u.data.pointer)
				result = acx_ioctl_get_nick(dev, NULL,
						&(iwr->u.data), nick);
			if (copy_to_user(iwr->u.data.pointer, nick,
						iwr->u.data.length))
				result = -EFAULT;
		}
		break;

	case SIOCSIWRATE:
		/* set default bit rate (bps) */
		result = acx_ioctl_set_rate(dev, NULL, &(iwr->u.bitrate),
					       NULL);
		break;

	case SIOCGIWRATE:
		/* get default bit rate (bps) */
		result = acx_ioctl_get_rate(dev, NULL, &(iwr->u.bitrate),
					       NULL);
		break;

	case  SIOCSIWRTS:
		/* set RTS threshold value */
		result = acx_ioctl_set_rts(dev, NULL, &(iwr->u.rts), NULL);
		break;
	case  SIOCGIWRTS:
		/* get RTS threshold value */
		result = acx_ioctl_get_rts(dev, NULL,  &(iwr->u.rts), NULL);
		break;

	/* FIXME: */
	/* case  SIOCSIWFRAG: */
	/* case  SIOCGIWFRAG: */

#if WIRELESS_EXT > 9
	case SIOCGIWTXPOW:
		/* get tx power */
		result = acx_ioctl_get_txpow(dev, NULL, &(iwr->u.txpower),
						NULL);
		break;

	case SIOCSIWTXPOW:
		/* set tx power */
		result = acx_ioctl_set_txpow(dev, NULL, &(iwr->u.txpower),
						NULL);
		break;
#endif

	case SIOCSIWRETRY:
		result = acx_ioctl_set_retry(dev, NULL, &(iwr->u.retry), NULL);
		break;
		
	case SIOCGIWRETRY:
		result = acx_ioctl_get_retry(dev, NULL, &(iwr->u.retry), NULL);
		break;

	case SIOCSIWENCODE:
		{
			/* set encoding token & mode */
			u8 key[29];
			if (iwr->u.encoding.pointer) {
				if (iwr->u.encoding.length > 29) {
					result = -E2BIG;
					break;
				}
				if (copy_from_user(key, iwr->u.encoding.pointer, iwr->u.encoding.length)) {
					result = -EFAULT;
					break;
				}
			}
			else
			if (iwr->u.encoding.length) {
				result = -EINVAL;
				break;
			}
			result = acx_ioctl_set_encode(dev, NULL,
					&(iwr->u.encoding), key);
		}
		break;

	case SIOCGIWENCODE:
		{
			/* get encoding token & mode */
			u8 key[29];

			result = acx_ioctl_get_encode(dev, NULL,
					&(iwr->u.encoding), key);
			if (iwr->u.encoding.pointer) {
				if (copy_to_user(iwr->u.encoding.pointer,
						key, iwr->u.encoding.length))
					result = -EFAULT;
			}
		}
		break;

	/******************** iwpriv ioctls below ********************/
#if ACX_DEBUG
	case ACX100_IOCTL_DEBUG:
		acx_ioctl_set_debug(dev, NULL, NULL, iwr->u.name);
		break;
#endif
		
	case ACX100_IOCTL_SET_PLED:
		acx100_ioctl_set_led_power(dev, NULL, NULL, iwr->u.name);
		break;

	case ACX100_IOCTL_GET_PLED:
		acx100_ioctl_get_led_power(dev, NULL, NULL, iwr->u.name);
		break;
		
	case ACX100_IOCTL_LIST_DOM:
		acx_ioctl_list_reg_domain(dev, NULL, NULL, NULL);
		break;
		
	case ACX100_IOCTL_SET_DOM:
		acx_ioctl_set_reg_domain(dev, NULL, NULL, iwr->u.name);
		break;
		
	case ACX100_IOCTL_GET_DOM:
		acx_ioctl_get_reg_domain(dev, NULL, NULL, iwr->u.name);
		break;
		
	case ACX100_IOCTL_SET_SCAN_PARAMS:
		acx_ioctl_set_scan_params(dev, NULL, NULL, iwr->u.name);
		break;

	case ACX100_IOCTL_GET_SCAN_PARAMS:
		acx_ioctl_get_scan_params(dev, NULL, NULL, iwr->u.name);
		break;

	case ACX100_IOCTL_SET_PREAMB:
		acx_ioctl_set_short_preamble(dev, NULL, NULL, iwr->u.name);
		break;
		
	case ACX100_IOCTL_GET_PREAMB:
		acx_ioctl_get_short_preamble(dev, NULL, NULL, iwr->u.name);
		break;
		
	case ACX100_IOCTL_SET_ANT:
		acx_ioctl_set_antenna(dev, NULL, NULL, iwr->u.name);
		break;
		
	case ACX100_IOCTL_GET_ANT:
		acx_ioctl_get_antenna(dev, NULL, NULL, NULL);
		break;
		
	case ACX100_IOCTL_RX_ANT:
		acx_ioctl_set_rx_antenna(dev, NULL, NULL, iwr->u.name);
		break;
		
	case ACX100_IOCTL_TX_ANT:
		acx_ioctl_set_tx_antenna(dev, NULL, NULL, iwr->u.name);
		break;
		
	case ACX100_IOCTL_SET_ED:
		acx_ioctl_set_ed_threshold(dev, NULL, NULL, iwr->u.name);
		break;
		
	case ACX100_IOCTL_SET_CCA:
		acx_ioctl_set_cca(dev, NULL, NULL, iwr->u.name);
		break;
		
	case ACX100_IOCTL_MONITOR:	/* set sniff (monitor) mode */
		acxlog(L_IOCTL, "%s: IWPRIV monitor\n", dev->name);

		/* can only be done by admin */
		if (!capable(CAP_NET_ADMIN)) {
			result = -EPERM;
			break;
		}
		result = acx_ioctl_wlansniff(dev, NULL, NULL, iwr->u.name);
		break;

	case ACX100_IOCTL_TEST:
		acx_ioctl_unknown11(dev, NULL, NULL, NULL);
		break;

	case ACX111_IOCTL_INFO:
		acx111_ioctl_info(dev, NULL, NULL, NULL);
		break;

/* #endif */

	default:
		acxlog(L_IOCTL, "wireless ioctl 0x%04X queried but not implemented yet!\n", cmd);
		result = -EOPNOTSUPP;
		break;
	}

	if ((priv->dev_state_mask & ACX_STATE_IFACE_UP) && priv->set_mask)
		acx_update_card_settings(priv, 0, 0, 0);

/* #if WIRELESS_EXT < 13 [see above] */
	/* older WEs don't have a commit handler,
	 * so we need to fix return code in this case */
	if (-EINPROGRESS == result)
		result = 0;
/* #endif */

	return result;
}
#endif /* WE < 13 */

