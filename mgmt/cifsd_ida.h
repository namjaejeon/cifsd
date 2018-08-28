/*
 *   Copyright (C) 2018 Samsung Electronics Co., Ltd.
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

#ifndef __CIFDS_IDA_MANAGEMENT_H__
#define __CIFDS_IDA_MANAGEMENT_H__

#include <linux/slab.h>
#include <linux/idr.h>

struct cifsd_ida {
	struct ida	map;
};

struct cifsd_ida *cifsd_ida_alloc(void);
void cifsd_ida_free(struct cifsd_ida *ida);

/*
 * 2.2.1.6.7 TID Generation
 *    The value 0xFFFF MUST NOT be used as a valid TID. All other
 *    possible values for TID, including zero (0x0000), are valid.
 *    The value 0xFFFF is used to specify all TIDs or no TID,
 *    depending upon the context in which it is used.
 */
int cifds_acquire_smb1_tid(struct cifsd_ida *ida);
int cifds_acquire_smb2_tid(struct cifsd_ida *ida);

/*
 * 2.2.1.6.8 UID Generation
 *    The value 0xFFFE was declared reserved in the LAN Manager 1.0
 *    documentation, so a value of 0xFFFE SHOULD NOT be used as a
 *    valid UID.<21> All other possible values for a UID, excluding
 *    zero (0x0000), are valid.
 */
int cifds_acquire_smb1_uid(struct cifsd_ida *ida);
int cifds_acquire_smb2_uid(struct cifsd_ida *ida);

int cifds_acquire_id(struct cifsd_ida *ida);

void cifds_release_id(struct cifsd_ida *ida, int id);
#endif /* __CIFSD_IDA_MANAGEMENT_H__ */
