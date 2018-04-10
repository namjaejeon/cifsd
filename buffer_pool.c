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

#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "glob.h"
#include "buffer_pool.h"

static struct kmem_cache *work_cache;
static struct kmem_cache *filp_cache;

/*
 * A simple kvmalloc()/kvfree() implemenation.
 */
static inline void *__alloc(size_t size, gfp_t flags)
{
	gfp_t kmalloc_flags = flags;
	void *ret;

	/*
	 * We want to attempt a large physically contiguous block first because
	 * it is less likely to fragment multiple larger blocks and therefore
	 * contribute to a long term fragmentation less than vmalloc fallback.
	 * However make sure that larger requests are not too disruptive - no
	 * OOM killer and no allocation failure warnings as we have a fallback.
	 */
	if (size > PAGE_SIZE)
		kmalloc_flags |= __GFP_NOWARN;

	ret = kmalloc(size, kmalloc_flags);

	/*
	 * It doesn't really make sense to fallback to vmalloc for sub page
	 * requests
	 */
	if (ret || size <= PAGE_SIZE)
		return ret;

	return __vmalloc(size, flags, PAGE_KERNEL);
}

static inline void __free(void *addr)
{
	if (is_vmalloc_addr(addr))
		vfree(addr);
	else
		kfree(addr);

}

void cifsd_free_request(void *addr)
{
	__free(addr);
}

void *cifsd_alloc_request(size_t size)
{
	return __alloc(size, GFP_KERNEL | __GFP_ZERO);
}

void cifsd_free_response(void *buffer)
{
	__free(buffer);
}

void *cifsd_alloc_response(size_t size)
{
	return __alloc(size, GFP_KERNEL | __GFP_ZERO);
}

void *cifsd_realloc_response(void *ptr, size_t old_sz, size_t new_sz)
{
	size_t sz = min(old_sz, new_sz);
	void *nptr;

	nptr = cifsd_alloc_response(new_sz);
	if (!nptr)
		return ptr;
	memcpy(nptr, ptr, sz);
	cifsd_free_response(ptr);
	return nptr;
}

struct smb_work *cifsd_alloc_work_struct(void)
{
	return kmem_cache_zalloc(work_cache, GFP_KERNEL);
}

void cifsd_free_work_struct(struct smb_work *work)
{
	kmem_cache_free(work_cache, work);
}

void cifsd_free_file_struct(void *filp)
{
	kmem_cache_free(filp_cache, filp);
}

void *cifsd_alloc_file_struct(void)
{
	return kmem_cache_zalloc(filp_cache, GFP_KERNEL);
}

void cifsd_destroy_buffer_pools(void)
{
	kmem_cache_destroy(work_cache);
	kmem_cache_destroy(filp_cache);
}

int cifsd_init_buffer_pools(void)
{
	work_cache = kmem_cache_create("cifsd_work_cache",
					sizeof(struct smb_work), 0,
					SLAB_HWCACHE_ALIGN, NULL);
	if (work_cache == NULL)
		goto out;

	filp_cache = kmem_cache_create("cifsd_file_cache",
					sizeof(struct cifsd_file), 0,
					SLAB_HWCACHE_ALIGN, NULL);
	if (filp_cache == NULL)
		goto out;

	return 0;

out:
	cifsd_err("failed to allocate memory\n");
	cifsd_destroy_buffer_pools();
	return -ENOMEM;
}
