/* include/p80211msg.h
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

#ifndef __ACX_P80211MSG_H
#define __ACX_P80211MSG_H

/*================================================================*/
/* System Includes */

/*================================================================*/
/* Project Includes */

#ifndef __ACX_WLAN_COMPAT_H
#include <wlan/wlan_compat.h>
#endif

/*================================================================*/
/* Constants */

#define MSG_BUFF_LEN		4000
#define WLAN_DEVNAMELEN_MAX	16

/*================================================================*/
/* Macros */

/*================================================================*/
/* Types */

/*--------------------------------------------------------------------*/
/*----- Message Structure Types --------------------------------------*/

/*--------------------------------------------------------------------*/
/* Prototype msg type */

__WLAN_PRAGMA_PACK1__ typedef struct p80211msg {
	UINT32 msgcode __WLAN_ATTRIB_PACK__;
	UINT32 msglen __WLAN_ATTRIB_PACK__;
	UINT8 devname[WLAN_DEVNAMELEN_MAX] __WLAN_ATTRIB_PACK__;
} __WLAN_ATTRIB_PACK__ p80211msg_t;
__WLAN_PRAGMA_PACKDFLT__ __WLAN_PRAGMA_PACK1__ typedef struct p80211msgd {
	UINT32 msgcode __WLAN_ATTRIB_PACK__;
	UINT32 msglen __WLAN_ATTRIB_PACK__;
	UINT8 devname[WLAN_DEVNAMELEN_MAX] __WLAN_ATTRIB_PACK__;
	UINT8 args[0] __WLAN_ATTRIB_PACK__;
} __WLAN_ATTRIB_PACK__ p80211msgd_t;
__WLAN_PRAGMA_PACKDFLT__
/*================================================================*/
/* Extern Declarations */
/*================================================================*/
/* Function Declarations */
#endif /* __ACX_P80211MSG_H */
