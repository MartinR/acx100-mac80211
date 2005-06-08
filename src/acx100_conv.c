/* src/p80211conv.c - conversion between 802.11 and ethernet
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
 * This code is based on elements which are
 * Copyright (C) 1999 AbsoluteValue Systems, Inc.  All Rights Reserved.
 * info@linux-wlan.com
 * http://www.linux-wlan.com
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
#define loff_t unsigned long
#define sigval_t unsigned long
#define siginfo_t unsigned long
#define stack_t unsigned long
#define __s64 signed long long
#endif
#include <linux/config.h>
#include <linux/version.h>

#include <linux/skbuff.h>
#include <linux/if_arp.h>
#include <linux/etherdevice.h>
#include <linux/wireless.h>
#if WIRELESS_EXT >= 13
#include <net/iw_handler.h>
#endif

#include <acx.h>

/*----------------------------------------------------------------
* acx_rxdesc_to_txdesc
*
* Converts a rx descriptor to a tx descriptor.
* Used in AP mode to prepare packets from one STA for tx to the other
*----------------------------------------------------------------*/

void acx_rxdesc_to_txdesc(const struct rxhostdescriptor *rxhostdesc,
				struct txdescriptor *txdesc)
{
	struct txhostdescriptor *head;
	struct txhostdescriptor *body;
	int body_len;
	
	head = txdesc->fixed_size.s.host_desc;
	body = head + 1;
	
	head->data_offset = 0;
	body->data_offset = 0;
	
	body_len = RXBUF_BYTES_RCVD(rxhostdesc->data) - WLAN_HDR_A3_LEN;
	memcpy(head->data, &rxhostdesc->data->hdr_a3, WLAN_HDR_A3_LEN);
	memcpy(body->data, &rxhostdesc->data->data_a3, body_len);

	head->length = cpu_to_le16(WLAN_HDR_A3_LEN);
	body->length = cpu_to_le16(body_len);
	txdesc->total_length = cpu_to_le16(body_len + WLAN_HDR_A3_LEN);
}

/*----------------------------------------------------------------
* proto_is_stt
*
* Searches the 802.1h Selective Translation Table for a given 
* protocol.
*
* Arguments:
*	prottype	protocol number (in host order) to search for.
*
* Returns:
*	1 - if the table is empty or a match is found.
*	0 - if the table is non-empty and a match is not found.
*
* Side effects:
*
* Call context:
*	May be called in interrupt or non-interrupt context
*
* STATUS:
*
* Comment:
*	Based largely on p80211conv.c of the linux-wlan-ng project
*
*----------------------------------------------------------------*/

static inline int proto_is_stt(unsigned int proto)
{
	/* Always return found for now.  This is the behavior used by the */
	/*  Zoom Win95 driver when 802.1h mode is selected */
	/* TODO: If necessary, add an actual search we'll probably
		 need this to match the CMAC's way of doing things.
		 Need to do some testing to confirm.
	*/

	if (proto == 0x80f3)  /* APPLETALK */
		return 1;

	return 0;
/*	return ((prottype == ETH_P_AARP) || (prottype == ETH_P_IPX)); */
}

/* Helpers */

static inline void store_llc_snap(struct wlan_llc *llc)
{
	llc->dsap = 0xaa;	/* SNAP, see IEEE 802 */
	llc->ssap = 0xaa;
	llc->ctl = 0x03;
}
static inline int llc_is_snap(const struct wlan_llc *llc)
{
	return (llc->dsap == 0xaa)
	&& (llc->ssap == 0xaa)
	&& (llc->ctl == 0x03);
}
static inline void store_oui_rfc1042(struct wlan_snap *snap)
{
	snap->oui[0] = 0;
	snap->oui[1] = 0;
	snap->oui[2] = 0;
}
static inline int oui_is_rfc1042(const struct wlan_snap *snap)
{
	return (snap->oui[0] == 0)
	&& (snap->oui[1] == 0)
	&& (snap->oui[2] == 0);
}
static inline void store_oui_8021h(struct wlan_snap *snap)
{
	snap->oui[0] = 0;
	snap->oui[1] = 0;
	snap->oui[2] = 0xf8;
}
static inline int oui_is_8021h(const struct wlan_snap *snap)
{
	return (snap->oui[0] == 0)
	&& (snap->oui[1] == 0)
	&& (snap->oui[2] == 0xf8);
}

/*----------------------------------------------------------------
* acx_ether_to_txdesc
*
* Uses the contents of the ether frame to build the elements of 
* the 802.11 frame.
*
* We don't actually set up the frame header here.  That's the 
* MAC's job.  We're only handling conversion of DIXII or 802.3+LLC 
* frames to something that works with 802.11.
*
* Arguments:
*
* Returns:
*
* Side effects:
*
* Call context:
*
* STATUS: 
*	FINISHED
*
* Comment: 
*	Based largely on p80211conv.c of the linux-wlan-ng project
*
*----------------------------------------------------------------*/

int acx_ether_to_txdesc(wlandevice_t *priv,
			 struct txdescriptor *tx_desc,
			 const struct sk_buff *skb)
{
	struct txhostdescriptor *header;
	struct txhostdescriptor *payload;
	union p80211_hdr *w_hdr;
	struct wlan_ethhdr *e_hdr; 
	struct wlan_llc *e_llc;
	struct wlan_snap *e_snap;
	const u8 *a1,*a2,*a3;
	int header_len,payload_len;
	u16 proto;		/* protocol type or data length, depending on whether DIX or 802.3 ethernet format */
	u16 fc;

	FN_ENTER;

	if (unlikely(0 == skb->len)) {
		acxlog(L_DEBUG, "zero-length skb!\n");
		FN_EXIT1(NOT_OK);
		return NOT_OK;
	}

	header = tx_desc->fixed_size.s.host_desc;
	if (unlikely((unsigned long)0xffffffff == (unsigned long)header)) /* FIXME: happens on card eject; better method? */
		return NOT_OK;
	payload = header + 1;

	switch (priv->mode) {
	case ACX_MODE_MONITOR:
		/* FIXME: skb size check... */
		header->length = 0;
		header->data_offset = 0;
		memcpy(payload->data, skb->data, skb->len);
		payload->length = cpu_to_le16(skb->len);
		payload->data_offset = 0;
		tx_desc->total_length = cpu_to_le16(skb->len);
		FN_EXIT0();
		return OK;
	}

	/* step 1: classify ether frame, DIX or 802.3? */
	e_hdr = (wlan_ethhdr_t *)skb->data;
	proto = ntohs(e_hdr->type);
	if (proto <= 1500) {
	        acxlog(L_DEBUG, "tx: 802.3 len: %d\n", skb->len);
                /* codes <= 1500 reserved for 802.3 lengths */
		/* it's 802.3, pass ether payload unchanged, */
		/* trim off ethernet header and copy payload to tx_desc */
		header_len = WLAN_HDR_A3_LEN;
		/* TODO: must be equal to skb->len - sizeof(wlan_ethhdr_t), no? */
		/* then we can do payload_len = ... after this big if() */
		payload_len = proto;
	} else {
		/* it's DIXII, time for some conversion */
		/* Create 802.11 packet. Header also contains llc and snap. */

		acxlog(L_DEBUG, "tx: DIXII len: %d\n", skb->len);

		/* size of header is 802.11 header + llc + snap */
		header_len = WLAN_HDR_A3_LEN + sizeof(wlan_llc_t) + sizeof(wlan_snap_t);
		/* llc is located behind the 802.11 header */
		e_llc = (wlan_llc_t*)(header->data + WLAN_HDR_A3_LEN);
		/* snap is located behind the llc */
		e_snap = (wlan_snap_t*)((u8*)e_llc + sizeof(wlan_llc_t));
			
		/* setup the LLC header */
		store_llc_snap(e_llc);

		/* setup the SNAP header */
		e_snap->type = htons(proto);
		if (proto_is_stt(proto)) {
			store_oui_8021h(e_snap);
		} else {
			store_oui_rfc1042(e_snap);
		}
		/* trim off ethernet header and copy payload to tx_desc */
		payload_len = skb->len - sizeof(wlan_ethhdr_t);
	}
	/* TODO: can we just let acx DMA payload from skb instead? */
	memcpy(payload->data, skb->data + sizeof(wlan_ethhdr_t), payload_len);
	payload->length = cpu_to_le16(payload_len);
	header->length = cpu_to_le16(header_len);
	payload->data_offset = 0;
	header->data_offset = 0;
	
	/* calculate total tx_desc length */
	tx_desc->total_length = cpu_to_le16(payload_len + header_len);

	/* Set up the 802.11 header */
	w_hdr = (p80211_hdr_t*)header->data;

	/* It's a data frame */
	fc = (WF_FTYPE_DATAi | WF_FSTYPE_DATAONLYi);

	switch (priv->mode) {
	case ACX_MODE_0_ADHOC:
		a1 = e_hdr->daddr;
		a2 = priv->dev_addr;
		a3 = priv->bssid;
		break;
	case ACX_MODE_2_STA:
		SET_BIT(fc, WF_FC_TODSi);
		a1 = priv->bssid;
		a2 = priv->dev_addr;
		a3 = e_hdr->daddr;
		break;
	case ACX_MODE_3_AP:
		SET_BIT(fc, WF_FC_FROMDSi);
		a1 = e_hdr->daddr;
		a2 = priv->bssid;
		a3 = e_hdr->saddr;
		break;
	default:
		acxlog(L_STD, "Error: Converting eth to wlan in unknown mode\n");
		goto fail;
	}
	MAC_COPY(w_hdr->a3.a1, a1);
	MAC_COPY(w_hdr->a3.a2, a2);
	MAC_COPY(w_hdr->a3.a3, a3);

	if (priv->wep_enabled)
		SET_BIT(fc, WF_FC_ISWEPi);
		
	w_hdr->a3.fc = fc;
	w_hdr->a3.dur = 0;
	w_hdr->a3.seq = 0;

#if DEBUG_CONVERT
	if (debug & L_DATA) {
		int i;
		printk("Original eth frame [%d]: ", skb->len);
		for (i = 0; i < skb->len; i++)
			printk(" %02x", ((u8 *) skb->data)[i]);
			printk("\n");

		printk("802.11 header [%d]: ", header_len));
		for (i = 0; i < header_len; i++)
			printk(" %02x", header->data[i]);
		printk("\n");

		printk("802.11 payload [%d]: ", payload_len);
		for (i = 0; i < payload_len); i++)
			printk(" %02x", payload->data[i]);
		printk("\n");
	}
#endif

fail:
	FN_EXIT0();
	return OK;
}

/*----------------------------------------------------------------
* acx_rxdesc_to_ether
*
* Uses the contents of a received 802.11 frame to build an ether
* frame.
*
* This function extracts the src and dest address from the 802.11
* frame to use in the construction of the eth frame.
*
* STATUS: FINISHED
*
* Based largely on p80211conv.c of the linux-wlan-ng project
*----------------------------------------------------------------*/

struct sk_buff *acx_rxdesc_to_ether(wlandevice_t *priv, const struct
		rxhostdescriptor *rx_desc)
{
	union p80211_hdr *w_hdr;
	struct wlan_ethhdr *e_hdr;
	struct wlan_llc *e_llc;
	struct wlan_snap *e_snap;
	struct sk_buff *skb;
	const u8 *daddr;
	const u8 *saddr;
	const u8 *e_payload;
	int buflen;
	int payload_length;
	unsigned int payload_offset;
	u16 fc;

	FN_ENTER;

	/* This looks complex because it must handle possible
	** phy header in rxbuff */
	w_hdr = acx_get_p80211_hdr(priv, rx_desc->data);
	payload_offset = WLAN_HDR_A3_LEN; /* it is relative to w_hdr */
	payload_length = RXBUF_BYTES_USED(rx_desc->data) /* entire rxbuff... */
		- ((u8*)w_hdr - (u8*)rx_desc->data) /* minus space before 802.11 frame */
		- WLAN_HDR_A3_LEN; /* minus 802.11 header */

	/* setup some vars for convenience */
	fc = w_hdr->a3.fc;
	switch (WF_FC_FROMTODSi & fc) {
	case 0:
		daddr = w_hdr->a3.a1;
		saddr = w_hdr->a3.a2;
		break;
	case WF_FC_FROMDSi:
		daddr = w_hdr->a3.a1;
		saddr = w_hdr->a3.a3;
		break;
	case WF_FC_TODSi:
		daddr = w_hdr->a3.a3;
		saddr = w_hdr->a3.a2;
		break;
	default: /* WF_FC_FROMTODSi */
		payload_offset += (WLAN_HDR_A4_LEN - WLAN_HDR_A3_LEN);
		payload_length -= (WLAN_HDR_A4_LEN - WLAN_HDR_A3_LEN);
		if (unlikely(0 > payload_length)) {
			acxlog(L_STD, "A4 frame too short!\n");
			FN_EXIT1((int)NULL);
			return NULL;
		}
		daddr = w_hdr->a4.a3;
		saddr = w_hdr->a4.a4;
	}
	
	if ((WF_FC_ISWEPi & fc) && (CHIPTYPE_ACX100 == priv->chip_type)) {
		/* chop off the IV+ICV WEP header and footer */
		acxlog(L_DATA | L_DEBUG, "rx: it's a WEP packet, chopping off IV and ICV.\n");
		payload_length -= 8;
		payload_offset += 4;
	}

	e_hdr = (wlan_ethhdr_t*) ((u8*) w_hdr + payload_offset);

	e_llc = (wlan_llc_t*) e_hdr;
	e_snap = (wlan_snap_t*) (e_llc+1);
	e_payload = (u8*) (e_snap+1);

	acxlog(L_DATA, "rx: payload_offset %i, payload_length %i\n", payload_offset, payload_length);
	acxlog(L_XFER | L_DATA,
		"rx: frame info: llc.dsap %X, llc.ssap %X, llc.ctl %X, snap.oui %02X%02X%02X, snap.type %X\n",
		e_llc->dsap, e_llc->ssap,
		e_llc->ctl, e_snap->oui[0],
		e_snap->oui[1], e_snap->oui[2],
		e_snap->type);

	/* Test for the various encodings */
	if ((payload_length >= sizeof(wlan_ethhdr_t))
	 && ((e_llc->dsap != 0xaa) || (e_llc->ssap != 0xaa))
	 && (   (mac_is_equal(daddr, e_hdr->daddr))
	     || (mac_is_equal(saddr, e_hdr->saddr))
	    )
	) {
		acxlog(L_DEBUG | L_DATA, "rx: 802.3 ENCAP len: %d\n", payload_length);
		/* 802.3 Encapsulated */
		/* Test for an overlength frame */

		if (unlikely(payload_length > WLAN_MAX_ETHFRM_LEN)) {
			/* A bogus length ethfrm has been encap'd. */
			/* Is someone trying an oflow attack? */
			acxlog(L_STD, "rx: ENCAP frame too large (%d > %d)\n", 
				payload_length, WLAN_MAX_ETHFRM_LEN);
			FN_EXIT1((int)NULL);
			return NULL;
		}

		/* allocate space and setup host buffer */
		buflen = payload_length;
		/* FIXME: implement skb ring buffer similar to
		 * xircom_tulip_cb.c? */
		skb = dev_alloc_skb(buflen + 2);	/* +2 is attempt to align IP header */
		if (unlikely(!skb)) {
			acxlog(L_STD, "rx: FAILED to allocate skb\n");
			FN_EXIT1((int)NULL);
			return NULL;
		}
		skb_reserve(skb, 2);
		skb_put(skb, buflen);		/* make room */

		/* now copy the data from the 80211 frame */
		memcpy(skb->data, e_hdr, payload_length);	/* copy the data */

	} else if ( (payload_length >= sizeof(wlan_llc_t)+sizeof(wlan_snap_t)) && llc_is_snap(e_llc) ) {
		/* it's a SNAP */

		if ( !oui_is_rfc1042(e_snap) || (proto_is_stt(ieee2host16(e_snap->type)) /* && (ethconv == WLAN_ETHCONV_8021h) */)) {
			acxlog(L_DEBUG | L_DATA, "rx: SNAP+RFC1042 len: %d\n", payload_length);
			/* it's a SNAP + RFC1042 frame && protocol is in STT */
			/* build 802.3 + RFC1042 */

			/* Test for an overlength frame */
			if (unlikely(payload_length + WLAN_ETHHDR_LEN > WLAN_MAX_ETHFRM_LEN)) {
				/* A bogus length ethfrm has been sent. */
				/* Is someone trying an oflow attack? */
				acxlog(L_STD, "rx: SNAP frame too large (%d > %d)\n", 
					payload_length, WLAN_MAX_ETHFRM_LEN);
				FN_EXIT1((int)NULL);
				return NULL;
			}

			/* allocate space and setup host buffer */
			buflen = payload_length + WLAN_ETHHDR_LEN;
			skb = dev_alloc_skb(buflen + 2);	/* +2 is attempt to align IP header */
			if (unlikely(!skb)) {
				acxlog(L_STD, "rx: FAILED to allocate skb\n");
				FN_EXIT1((int)NULL);
				return NULL;
			}
			skb_reserve(skb, 2);
			skb_put(skb, buflen);		/* make room */

			/* create 802.3 header */
			e_hdr = (wlan_ethhdr_t*) skb->data;
			MAC_COPY(e_hdr->daddr, daddr);
			MAC_COPY(e_hdr->saddr, saddr); 
			e_hdr->type = htons(payload_length);

			/* Now copy the data from the 80211 frame.
			   Make room in front for the eth header, and keep the 
			   llc and snap from the 802.11 payload */
			memcpy(skb->data + WLAN_ETHHDR_LEN, 
			       e_llc, 
			       payload_length);
		       
		} else {
			acxlog(L_DEBUG | L_DATA, "rx: 802.1h/RFC1042 len: %d\n", payload_length);
			/* it's an 802.1h frame (an RFC1042 && protocol is not in STT) */
			/* build a DIXII + RFC894 */
			
			/* Test for an overlength frame */
			if (unlikely(payload_length - sizeof(wlan_llc_t) - sizeof(wlan_snap_t) + WLAN_ETHHDR_LEN > WLAN_MAX_ETHFRM_LEN)) {
				/* A bogus length ethfrm has been sent. */
				/* Is someone trying an oflow attack? */
				acxlog(L_STD, "rx: DIXII frame too large (%d > %d)\n",
						payload_length - sizeof(wlan_llc_t) - sizeof(wlan_snap_t),
						WLAN_MAX_ETHFRM_LEN - WLAN_ETHHDR_LEN);
				FN_EXIT1((int)NULL);
				return NULL;
			}
	
			/* allocate space and setup host buffer */
			buflen = payload_length + WLAN_ETHHDR_LEN - sizeof(wlan_llc_t) - sizeof(wlan_snap_t);
			skb = dev_alloc_skb(buflen + 2);	/* +2 is attempt to align IP header */
			if (unlikely(!skb)) {
				acxlog(L_STD, "rx: FAILED to allocate skb\n");
				FN_EXIT1((int)NULL);
				return NULL;
			}
			skb_reserve(skb, 2);
			skb_put(skb, buflen);		/* make room */
	
			/* create 802.3 header */
			e_hdr = (wlan_ethhdr_t *) skb->data;
			MAC_COPY(e_hdr->daddr, daddr);
			MAC_COPY(e_hdr->saddr, saddr); 
			e_hdr->type = e_snap->type;
	
			/* Now copy the data from the 80211 frame.
			   Make room in front for the eth header, and cut off the 
			   llc and snap from the 802.11 payload */
			memcpy(skb->data + WLAN_ETHHDR_LEN, e_payload,
			       payload_length - sizeof(wlan_llc_t) - sizeof(wlan_snap_t));
		}

	} else {
		acxlog(L_DEBUG | L_DATA, "rx: NON-ENCAP len: %d\n", payload_length);
		/* any NON-ENCAP */
		/* it's a generic 80211+LLC or IPX 'Raw 802.3' */
		/*  build an 802.3 frame */
		/* allocate space and setup hostbuf */

		/* Test for an overlength frame */
		if (unlikely(payload_length + WLAN_ETHHDR_LEN > WLAN_MAX_ETHFRM_LEN)) {
			/* A bogus length ethfrm has been sent. */
			/* Is someone trying an oflow attack? */
			acxlog(L_STD, "rx: OTHER frame too large (%d > %d)\n",
				payload_length,
				WLAN_MAX_ETHFRM_LEN - WLAN_ETHHDR_LEN);
			FN_EXIT1((int)NULL);
			return NULL;
		}

		/* allocate space and setup host buffer */
		buflen = payload_length + WLAN_ETHHDR_LEN;
		skb = dev_alloc_skb(buflen + 2);	/* +2 is attempt to align IP header */
		if (unlikely(!skb)) {
			acxlog(L_STD, "rx: FAILED to allocate skb\n");
			FN_EXIT1((int)NULL);
			return NULL;
		}
		skb_reserve(skb, 2);
		skb_put(skb, buflen);		/* make room */

		/* set up the 802.3 header */
		e_hdr = (wlan_ethhdr_t *) skb->data;
		MAC_COPY(e_hdr->daddr, daddr);
		MAC_COPY(e_hdr->saddr, saddr);
		e_hdr->type = htons(payload_length);
		
		/* now copy the data from the 80211 frame */
		memcpy(skb->data + WLAN_ETHHDR_LEN, e_llc, payload_length);
	}

	skb->dev = priv->netdev;
	skb->protocol = eth_type_trans(skb, priv->netdev);
	
#if DEBUG_CONVERT
	if (debug & L_DATA) {
		int i;
		printk("p802.11 frame [%d]:", RXBUF_BYTES_RCVD(rx_desc->data));
		for (i = 0; i < RXBUF_BYTES_RCVD(rx_desc->data); i++)
			printk(" %02x", ((u8 *) w_hdr)[i]);
		printk("\n");

		printk("eth frame [%d]:", skb->len);
		for (i = 0; i < skb->len; i++)
			printk(" %02x", ((u8 *) skb->data)[i]);
		printk("\n");
	}
#endif

	FN_EXIT0();
	return skb;
}
