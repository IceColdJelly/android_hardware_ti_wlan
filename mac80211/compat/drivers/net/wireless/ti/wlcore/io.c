/*
 * This file is part of wl1271
 *
 * Copyright (C) 2008-2010 Nokia Corporation
 *
 * Contact: Luciano Coelho <luciano.coelho@nokia.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>
#include <linux/interrupt.h>

#include "wlcore.h"
#include "debug.h"
#include "wl12xx_80211.h"
#include "io.h"
#include "tx.h"

bool wl1271_set_block_size(struct wl1271 *wl)
{
	if (wl->if_ops->set_block_size) {
		wl->if_ops->set_block_size(wl->dev, WL12XX_BUS_BLOCK_SIZE);
		return true;
	}

	return false;
}

void wlcore_disable_interrupts(struct wl1271 *wl)
{
	disable_irq(wl->irq);
}
EXPORT_SYMBOL_GPL(wlcore_disable_interrupts);

void wlcore_enable_interrupts(struct wl1271 *wl)
{
	enable_irq(wl->irq);
}
EXPORT_SYMBOL_GPL(wlcore_enable_interrupts);

int wlcore_translate_addr(struct wl1271 *wl, int addr)
{
	/*
	 * To translate, first check to which window of addresses the
	 * particular address belongs. Then subtract the starting address
	 * of that window from the address. Then, add offset of the
	 * translated region.
	 *
	 * The translated regions occur next to each other in physical device
	 * memory, so just add the sizes of the preceding address regions to
	 * get the offset to the new region.
	 */
	if ((addr >= wl->curr_part.mem.start) &&
	    (addr < wl->curr_part.mem.start + wl->curr_part.mem.size))
		return addr - wl->curr_part.mem.start;
	else if ((addr >= wl->curr_part.reg.start) &&
		 (addr < wl->curr_part.reg.start + wl->curr_part.reg.size))
		return addr - wl->curr_part.reg.start + wl->curr_part.mem.size;
	else if ((addr >= wl->curr_part.mem2.start) &&
		 (addr < wl->curr_part.mem2.start + wl->curr_part.mem2.size))
		return addr - wl->curr_part.mem2.start + wl->curr_part.mem.size +
			wl->curr_part.reg.size;
	else if ((addr >= wl->curr_part.mem3.start) &&
		 (addr < wl->curr_part.mem3.start + wl->curr_part.mem3.size))
		return addr - wl->curr_part.mem3.start + wl->curr_part.mem.size +
			wl->curr_part.reg.size + wl->curr_part.mem2.size;

	WARN(1, "HW address 0x%x out of range", addr);
	return 0;
}
EXPORT_SYMBOL_GPL(wlcore_translate_addr);

/* Set the partitions to access the chip addresses
 *
 * To simplify driver code, a fixed (virtual) memory map is defined for
 * register and memory addresses. Because in the chipset, in different stages
 * of operation, those addresses will move around, an address translation
 * mechanism is required.
 *
 * There are four partitions (three memory and one register partition),
 * which are mapped to two different areas of the hardware memory.
 *
 *                                Virtual address
 *                                     space
 *
 *                                    |    |
 *                                 ...+----+--> mem.start
 *          Physical address    ...   |    |
 *               space       ...      |    | [PART_0]
 *                        ...         |    |
 *  00000000  <--+----+...         ...+----+--> mem.start + mem.size
 *               |    |         ...   |    |
 *               |MEM |      ...      |    |
 *               |    |   ...         |    |
 *  mem.size  <--+----+...            |    | {unused area)
 *               |    |   ...         |    |
 *               |REG |      ...      |    |
 *  mem.size     |    |         ...   |    |
 *      +     <--+----+...         ...+----+--> reg.start
 *  reg.size     |    |   ...         |    |
 *               |MEM2|      ...      |    | [PART_1]
 *               |    |         ...   |    |
 *                                 ...+----+--> reg.start + reg.size
 *                                    |    |
 *
 */
void wlcore_set_partition(struct wl1271 *wl,
			  const struct wlcore_partition_set *p)
{
	/* copy partition info */
	memcpy(&wl->curr_part, p, sizeof(*p));

	wl1271_debug(DEBUG_IO, "mem_start %08X mem_size %08X",
		     p->mem.start, p->mem.size);
	wl1271_debug(DEBUG_IO, "reg_start %08X reg_size %08X",
		     p->reg.start, p->reg.size);
	wl1271_debug(DEBUG_IO, "mem2_start %08X mem2_size %08X",
		     p->mem2.start, p->mem2.size);
	wl1271_debug(DEBUG_IO, "mem3_start %08X mem3_size %08X",
		     p->mem3.start, p->mem3.size);

	wl1271_raw_write32(wl, HW_PART0_START_ADDR, p->mem.start);
	wl1271_raw_write32(wl, HW_PART0_SIZE_ADDR, p->mem.size);
	wl1271_raw_write32(wl, HW_PART1_START_ADDR, p->reg.start);
	wl1271_raw_write32(wl, HW_PART1_SIZE_ADDR, p->reg.size);
	wl1271_raw_write32(wl, HW_PART2_START_ADDR, p->mem2.start);
	wl1271_raw_write32(wl, HW_PART2_SIZE_ADDR, p->mem2.size);
	/*
	 * We don't need the size of the last partition, as it is
	 * automatically calculated based on the total memory size and
	 * the sizes of the previous partitions.
	 */
	wl1271_raw_write32(wl, HW_PART3_START_ADDR, p->mem3.start);
}
EXPORT_SYMBOL_GPL(wlcore_set_partition);

void wlcore_select_partition(struct wl1271 *wl, u8 part)
{
	wl1271_debug(DEBUG_IO, "setting partition %d", part);

	wlcore_set_partition(wl, &wl->ptable[part]);
}
EXPORT_SYMBOL_GPL(wlcore_select_partition);

void wl1271_io_reset(struct wl1271 *wl)
{
	if (wl->if_ops->reset)
		wl->if_ops->reset(wl->dev);
}

void wl1271_io_init(struct wl1271 *wl)
{
	if (wl->if_ops->init)
		wl->if_ops->init(wl->dev);
}