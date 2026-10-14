/*
 * Copyright (C) 2012-2014 Vasily Tarasov
 * Copyright (C) 2012-2014 Geoff Kuenning
 * Copyright (C) 2012-2014 Sonam Mandal
 * Copyright (C) 2012-2014 Karthikeyani Palanisami
 * Copyright (C) 2012-2014 Philip Shilane
 * Copyright (C) 2012-2014 Sagar Trehan
 * Copyright (C) 2012-2014 Erez Zadok
 *
 * This file is released under the GPL.
 */

#include "dm-dedup-target.h"
#include "dm-dedup-hash.h"
#include <linux/atomic.h>
#include <linux/blk_types.h>
#include <linux/crypto.h>
#include <linux/scatterlist.h>
#include <linux/highmem.h>

/*
 * We pre-allocate crypto_shash tfm per slot to avoid allocation
 * in the critical path (which may sleep).
 */
static struct crypto_shash *slot_to_tfm(struct hash_desc_table *desc_table,
					 unsigned long slot)
{
	BUG_ON(slot >= DEDUP_HASH_DESC_COUNT);
	return desc_table->tfm[slot];
}

struct hash_desc_table *desc_table_init(char *hash_alg)
{
	int i = 0;
	struct hash_desc_table *desc_table;

	desc_table = kmalloc(sizeof(struct hash_desc_table), GFP_NOIO);
	if (!desc_table)
		return ERR_PTR(-ENOMEM);

	for (i = 0; i < DEDUP_HASH_DESC_COUNT; i++) {
		desc_table->free_bitmap[i] = true;
		desc_table->tfm[i] = crypto_alloc_shash(hash_alg, 0, 0);
		if (IS_ERR(desc_table->tfm[i]))
			return (struct hash_desc_table *)desc_table->tfm[i];
	}

	atomic_long_set(&(desc_table->slot_counter), 0);

	return desc_table;
}

void desc_table_deinit(struct hash_desc_table *desc_table)
{
	int i = 0;

	for (i = 0; i < DEDUP_HASH_DESC_COUNT; i++) {
		crypto_free_shash(desc_table->tfm[i]);
	}

	kfree(desc_table);
	desc_table = NULL;
}

static int get_next_slot(struct hash_desc_table *desc_table)
{
	unsigned long num = 0;
	int count = 0;

	do {
		if (count == DEDUP_HASH_DESC_COUNT)
			return -EBUSY;

		count++;
		num = atomic_long_inc_return(&(desc_table->slot_counter));
		num = num % DEDUP_HASH_DESC_COUNT;

	} while (!desc_table->free_bitmap[num]);

	/* XXX: Possibility of race condition here. As checking of bitmap
	 *	and its setting is not happening in same step. But it will
	 *	work for now, as we declare atleast twice more hash_desc
	 *	then number of threads.
	 */
	desc_table->free_bitmap[num] = false;

	return num;
}

static void put_slot(struct hash_desc_table *desc_table, unsigned long slot)
{
	BUG_ON(slot >= DEDUP_HASH_DESC_COUNT);
	BUG_ON(desc_table->free_bitmap[slot]);
	desc_table->free_bitmap[slot] = true;
}

unsigned int get_hash_digestsize(struct hash_desc_table *desc_table)
{
	unsigned long slot;
	struct crypto_shash *tfm;

	slot = get_next_slot(desc_table);
	tfm = slot_to_tfm(desc_table, slot);

	return crypto_shash_digestsize(tfm);
}

int compute_hash_bio(struct hash_desc_table *desc_table,
			struct bio *bio, char *hash)
{
	struct shash_desc *desc;
	int ret = 0;
	unsigned long slot;
	struct bio_vec bvec;
	struct bvec_iter iter;
	struct crypto_shash *tfm;
	unsigned int desc_size;

	slot = get_next_slot(desc_table);
	tfm = slot_to_tfm(desc_table, slot);

	desc_size = crypto_shash_descsize(tfm) + sizeof(*desc);
	desc = kmalloc(desc_size, GFP_NOIO);
	if (!desc) {
		put_slot(desc_table, slot);
		return -ENOMEM;
	}

	desc->tfm = tfm;

	ret = crypto_shash_init(desc);
	if (ret)
		goto out;

	__bio_for_each_segment(bvec, bio, iter, bio->bi_iter) {
		void *addr = page_address(bvec.bv_page) + bvec.bv_offset;
		ret = crypto_shash_update(desc, addr, bvec.bv_len);
		if (ret)
			goto out;
	}

	ret = crypto_shash_final(desc, hash);

out:
	kfree(desc);
	put_slot(desc_table, slot);
	return ret;
}
