/*
 * file : gzd-stor.c
 * desc : linux storage device driver for the Gen-Z demo PCIe device
 *
 * Author:  Jim Hull <jim.hull@hpe.com>
 *          Betty Dall <betty.dall@hpe.com>
 *	    JS Ma <majinsuk@etri.re.kr>
 *	    SW SOK <swsok@etri.re.kr>
 *
 * Copyright:
 *     © Copyright 2016-2017 Hewlett Packard Enterprise Development LP
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the
 * Free Software Foundation, Inc.
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include <linux/module.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/cdev.h>
#include <linux/blkdev.h>
#include <linux/interrupt.h>
#include <linux/ratelimit.h>
#include <linux/pci.h>
#include <linux/dma-direction.h>
#include "gzd.h"
#include <linux/blk-mq.h>
#include <linux/nodemask.h>

#define GZD_STOR_DRV_NAME "gzd-stor"
#define GZD_CDEV_NAME "gzc"
#define GZD_BDEV_NAME "gzb"

enum {
	GZD_Q_BIO = 0,
	GZD_Q_RQ = 1,
};

enum {
	GZD_IRQ_NONE = 0,
	GZD_IRQ_SOFTIRQ = 1,
};

static uint num_cdevs = 1;
module_param(num_cdevs, uint, S_IRUGO);
MODULE_PARM_DESC(num_cdevs, "number of char devs per media");

static uint num_bdevs = 1;
module_param(num_bdevs, uint, S_IRUGO);
MODULE_PARM_DESC(num_bdevs, "number of block devs per media");

static uint bdev_percent = 50;
module_param(bdev_percent, uint, S_IRUGO);
MODULE_PARM_DESC(bdev_percent, "percent of media size for block devs");

static uint zero_bdevs[MAX_MEDIA] = { 0 };

static int zero_bdevs_argc;
module_param_array(zero_bdevs, uint, &zero_bdevs_argc, S_IRUGO);
MODULE_PARM_DESC(zero_bdevs, "enable to zero block devs at driver init time");

struct gzd_stor_card_data;

struct gzd_cdev {
	struct mutex lock;
	struct cdev cdev;
	dev_t devt;
	phys_addr_t mmio_addr;
	void __iomem *base_addr;
	size_t size;
	struct device *device;
	struct gzd_stor_card_data *card_data;
};

struct gzd_bdev {
	spinlock_t lock;
	size_t size;		/* Device size (bytes) */
	uint32_t cid;
	uint64_t base_zaddr;
	struct request_queue *queue;
	struct gendisk *gd;
	struct gzd_stor_card_data *card_data;
	spinlock_t queue_lock;
	struct request *req;
};

static int queue_mode = GZD_Q_RQ;
int debug_trap = 0;

struct gzd_stor_card_data {
	struct list_head list;
	struct mutex lock;
	void *hw;
	struct gzd_core_info *core_info;
	uint cdev_start_minor;
	uint bdev_start_minor;
	uint num_cdevs;
	uint num_bdevs;
	wait_queue_head_t block_io_queue;
	int block_io_ready;
	struct gzd_cdev *cdev;
	struct gzd_bdev *bdev;
};

LIST_HEAD(gzd_stor_card_list);
DEFINE_MUTEX(gzd_stor_lock);	/* used only during initialization */

static int gzd_stor_probe(void *arg);
static void gzd_stor_remove(void *arg);

static struct gzd_driver gzd_stor_driver = {
	.name = GZD_STOR_DRV_NAME,
	.probe = gzd_stor_probe,
	.remove = gzd_stor_remove,
};

/* ============================================================
 *                    THE BDEV FILE OPS
 * ============================================================
*/

struct bdev_bio {
	struct bio *bio;
	struct request *req;
	int size;
	int remaining;
	dma_addr_t mem;
	int dma_dir;
	int status;
};
struct tag_dma_info {
	dma_addr_t mem;
	int len;
	int dma_dir;
	int remaining;
	int unmap;
};
struct tag_info {
	struct bdev_bio *bbio;
	struct tag_dma_info *tag_dma;
};

#define MAX_TAG	1024
static struct tag_info bdev_tag_data[MAX_TAG] = { {0} };

static uint bdev_get_tag(struct bdev_bio *bbio, struct tag_dma_info *tag_dma);
static void bdev_end_tag(uint tag);
static int bdev_bio_chunk_done(struct device *dev, struct tag_info *ti,
			       int bytes_completed, int status);
static struct bdev_bio *bdev_get_bbio_req(struct bio *bio, struct request *req,
					  int size);
static void bdev_free_bbio(struct bdev_bio *bbio);
static struct tag_dma_info *bdev_get_tag_dma(dma_addr_t mem, int len,
					     int dma_dir, int unmap);
static void bdev_free_tag_dma(struct tag_dma_info *tag_dma);
static int bdev_major;
static uint gzd_bdev_start_minor;

static uint bdev_current_tag;
DEFINE_SPINLOCK(bdev_tag_lock);

#define CHUNK 2048
#define GZD_BLOCK_SIZE	512
#define MAX_BIO_REQ_ID	32768
#define GZD_BDEV_MINORS 16

/* Block Control register */
#define	BIO_COMP_REQ_ID	0x00FF0000	/* bits 47:32 */
#define BIO_DONE	0x00000100	/* bit 8 */

/*
 * We can tweak our hardware sector size, but the kernel talks to us
 * in terms of small sectors, always.
 */
#define KERNEL_SECTOR_SHIFT	9
#define KERNEL_SECTOR_SIZE	(1<<KERNEL_SECTOR_SHIFT)

static int gzd_bdev_open(struct block_device *bdev, fmode_t fm)
{
	return 0;
}

static void gzd_bdev_release(struct gendisk *bgen, fmode_t fm)
{
}

static int gzd_bdev_ioctl(struct block_device *bdev, fmode_t fm,
			  unsigned int ioctl, unsigned long data)
{
	return 0;
}

static size_t gzd_stor_bdev_size(size_t media_size,
				 uint bdev_percent, uint num_bdevs)
{
	ulong size;

	size = media_size * bdev_percent / 100 / num_bdevs;
	size = rounddown(size, PAGE_SIZE);

	return size;
}

#define GZD_WRITE 0
#define GZD_READ 1

static irqreturn_t bdev_irq_handler(int irq, void *data)
{
	int ret, handled = 0;
	uint req_id;
	struct gzd_stor_card_data *card_data =
	    (struct gzd_stor_card_data *)data;
	int more, remaining, wakeup = 0;
	struct bdev_bio *bbio;
	uint free_reqs;
	struct pci_dev *pdev;
	pdev = card_data->core_info->pdev;

	do {
		more = 0;
		ret = gzd_stor_driver.block_io_response(card_data->hw,
							&req_id, &free_reqs);
		if (ret < 0) {
			pr_err
			    ("%s: block_io_response returned error: %d for req_id %u\n",
			     __func__, ret, req_id);
		}
		if (free_reqs > 0)
			wakeup = 1;
		if (ret == 0 || ret == -EINVAL || ret == -ENOSPC) 
			break;	/* no more responses */
		
		more = handled = 1;
		/* Complete the block transfer. */
		bbio = bdev_tag_data[req_id].bbio;
		/* Mark completion of a chunk in our bbio */
		remaining = bdev_bio_chunk_done(&pdev->dev,
						&bdev_tag_data[req_id],
						min(bbio->remaining, CHUNK),
						ret);
		if (remaining == 0) {
			if (bbio->bio) {
				if (bbio->status == 0) {	/* no error */
					blk_end_request(bbio->req, 0,
							blk_rq_cur_bytes
							(bbio->req));

				} else {	/* -EIO */
					pr_debug("irq handler1 error\n");
					blk_end_request_all(bbio->req, -EIO);
				}
			}
			bdev_free_bbio(bbio);
		} else if (remaining == -1) {
			if (bbio->bio) {
				pr_debug("irq handler2 error\n");
				// bio_io_error(bbio->bio);      /*   -EIO   */
				blk_end_request_all(bbio->req, -EIO);

			}
			bdev_free_bbio(bbio);
		}
		bdev_end_tag(req_id);
	} while (more);

	card_data->block_io_ready = free_reqs;
	if (wakeup) 
		wake_up_interruptible(&card_data->block_io_queue);
	

	return (handled) ? IRQ_HANDLED : IRQ_NONE;
}

static uint bdev_get_tag(struct bdev_bio *bbio, struct tag_dma_info *tag_dma)
{
	uint tag;
	int i;
	ulong flags;

	spin_lock_irqsave(&bdev_tag_lock, flags);
	for (i = 0; i < MAX_TAG; i++) {
		tag = bdev_current_tag++;
		/* Check for wrapping */
		if (bdev_current_tag == MAX_TAG) 
			bdev_current_tag = 0;
		
		/* Make sure this is an unused tag. */
		if (bdev_tag_data[tag].bbio || bdev_tag_data[tag].tag_dma)
			continue;
		else
			break;
	}
	BUG_ON(i == MAX_TAG);
	bdev_tag_data[tag].bbio = bbio;
	bdev_tag_data[tag].tag_dma = tag_dma;
	spin_unlock_irqrestore(&bdev_tag_lock, flags);
	return tag;
}

static void bdev_end_tag(uint tag)
{
	ulong flags;

	spin_lock_irqsave(&bdev_tag_lock, flags);
	bdev_tag_data[tag].bbio = 0;
	bdev_tag_data[tag].tag_dma = 0;
	spin_unlock_irqrestore(&bdev_tag_lock, flags);
}

static struct bdev_bio *bdev_get_bbio_req(struct bio *bio, struct request *req,
					  int size)
{
	struct bdev_bio *bbio;

	bbio = kzalloc(sizeof(struct bdev_bio), GFP_KERNEL);
	if (!bbio)
		return NULL;
	bbio->bio = bio;
	bbio->req = req;
	bbio->size = size;
	bbio->remaining = size;

	return bbio;
}

static void bdev_free_bbio(struct bdev_bio *bbio)
{
	kfree(bbio);
}

static struct tag_dma_info *bdev_get_tag_dma(dma_addr_t mem, int len,
					     int dma_dir, int unmap)
{
	struct tag_dma_info *tag_dma;

	tag_dma = kzalloc(sizeof(struct tag_dma_info), GFP_KERNEL);
	if (!tag_dma)
		return NULL;
	tag_dma->mem = mem;
	tag_dma->len = len;
	tag_dma->dma_dir = dma_dir;
	tag_dma->remaining = len;
	tag_dma->unmap = unmap;

	return tag_dma;
}

static void bdev_free_tag_dma(struct tag_dma_info *tag_dma)
{
	kfree(tag_dma);
}

static int bdev_bio_chunk_done(struct device *dev,
			       struct tag_info *ti, int bytes_completed,
			       int status)
{
	struct bdev_bio *bbio = ti->bbio;
	struct tag_dma_info *tag_dma = ti->tag_dma;

	/* Update the remaining bytes in the dma buffer. */
	if (tag_dma) {
		tag_dma->remaining -= bytes_completed;
		if (tag_dma->remaining <= 0) {
			if (tag_dma->unmap) {
				/* all the transfers for the dma buffer are done. */
				dma_sync_single_for_cpu(dev, tag_dma->mem,
							tag_dma->len,
							tag_dma->dma_dir);
				dma_unmap_page(dev, tag_dma->mem, tag_dma->len,
					       tag_dma->dma_dir);
			}
			bdev_free_tag_dma(tag_dma);
			ti->tag_dma = NULL;
		}
	}

	if (status < 0)		/* record error */
		bbio->status = status;
	if (bbio->remaining < bytes_completed)
		/* weird error */
		return -1;
	bbio->remaining -= bytes_completed;
	if (bbio->remaining == 0)
		return 0;
	else
		return bbio->remaining;
}

#define GZD_TIMEOUT (9 * HZ)	/* 9 seconds */

/*
 * Process a single bio_vec of a bio.
 */
static int bdev_do_bvec(struct gzd_bdev *dev, struct page *page,
			unsigned int len, unsigned int off, int rw,
			sector_t sector, struct bdev_bio *bbio)
{
	dma_addr_t mem;
	int chunk;
	uint chunk_sz;
	uint32_t dcid = dev->cid;
	uint64_t genz_addr;
	int tag;
	int retry, ret = 0;
	struct pci_dev *pdev;
	int dma_dir;
	struct tag_dma_info *tag_dma;
	long err;

	genz_addr = (uint64_t) dev->base_zaddr + (sector * KERNEL_SECTOR_SIZE);
	pdev = dev->card_data->core_info->pdev;
	dma_dir = ((rw == READ) ? DMA_FROM_DEVICE : DMA_TO_DEVICE);

	mem = dma_map_page(&pdev->dev, page, off, len, dma_dir);
	if (dma_mapping_error(&pdev->dev, mem)) {
		pr_err("%s: dma_map_page failed\n", __func__);
		ret = -1;
		goto out;
	}

	tag_dma = bdev_get_tag_dma(mem, len, dma_dir, 1);
	dma_sync_single_for_device(&pdev->dev, mem, len, dma_dir);

	chunk_sz = min(len, (uint) CHUNK);
	if (chunk_sz != 2048)
		pr_debug("small chunk size input->%d\n", chunk_sz);
	/* Break the page into chunks for gzd hw */
	for (chunk = 0; chunk < len; chunk += chunk_sz) {
		tag = bdev_get_tag(bbio, tag_dma);
		retry = 0;
retry:
		if (debug_trap == 1)
			pr_debug("traped chunk size:%d\n", chunk_sz);

		ret =
		    gzd_stor_driver.block_io_request((void *)dev->card_data->hw,
						     (dma_addr_t) mem + chunk,
						     genz_addr + chunk,
						     chunk_sz, tag, dcid,
						     ((rw ==
						       READ) ? GZD_READ :
						      GZD_WRITE));

		if (ret == -EBUSY) {
			/* sleep for a bit to clear the fifo */
			pr_debug
			    ("%s: sleeping bio_rq EBUSY, tag %d\n",
			     __func__, tag);
			err =
			    wait_event_interruptible_timeout(dev->
							     card_data->block_io_queue,
							     dev->
							     card_data->block_io_ready,
							     GZD_TIMEOUT);

			if (err == 0) {	/* timeout expired */
				pr_err("%s: timeout expired, tag %d\n",
				       __func__, tag);
				ret = -EIO;
			} else {
				retry = 1;
				goto retry;
			}
		}
		if (ret) {
			pr_err
			    ("%s: block_io_request failed with return %d tag %d\n",
			     __func__, ret, tag);
		}
	}
out:
	return ret;
}

static int bdev_make_request_queue(struct request *req)
{

	struct gzd_bdev *dev = req->bio->bi_disk->private_data;
	struct bio_vec bvec;
	struct req_iterator iter;
	sector_t sector;
	unsigned int len;
	struct bdev_bio *bbio;
	int bvec_ret = 0;
	int rw;
/*
	if (req->rq_disk->private_data != req->q->queuedata) {
		pr_debug("\nrequest error\n\n");
		goto io_error;
	}
*/
	if (rq_end_sector(req) > get_capacity(req->bio->bi_disk)) {
		pr_err("%s: end_sector > capacity\n", __func__);
		goto io_error;
	}
	rq_for_each_segment(bvec, req, iter) {
		sector = iter.iter.bi_sector;
		bbio = bdev_get_bbio_req(iter.bio, req, iter.iter.bi_size);
		if (bbio == NULL) {
			pr_err("%s: bdev_get_bbio error\n", __func__);
			goto io_error;
		}
		rw = rq_data_dir(req);
		len = bvec.bv_len;
		bvec_ret = bdev_do_bvec(dev, bvec.bv_page, len,
					bvec.bv_offset, rw, sector, bbio);
		if (bvec_ret)
			goto io_error;
	}
	return 0;

io_error:
	pr_err("%s:  bvec_ret(in rq loop) error !!!\n", __func__);
	return -EIO;

}

static const struct block_device_operations gzd_bdev_ops = {
	.owner = THIS_MODULE,
	.open = gzd_bdev_open,
	.release = gzd_bdev_release,
	.ioctl = gzd_bdev_ioctl,
};

static int irqmode = GZD_IRQ_NONE;

static void gzd_request_queue_fn(struct request_queue *queue)
{
	struct request *req;

//      pr_debug("start:gzd_request_queue_fn: q=%p irqmode=%d\n", queue, irqmode);
	while ((req = blk_fetch_request(queue)) != NULL) {
		spin_unlock_irq(queue->queue_lock);
		switch (req_op(req)) {
		case REQ_OP_FLUSH:
			pr_debug("req_op is flush\n");
			break;

		case REQ_OP_WRITE:
		case REQ_OP_READ:
			{
				pr_debug("req_op is read or write\n");

				switch (irqmode) {
				case GZD_IRQ_NONE:
					debug_trap = 0;
					if (blk_rq_cur_bytes(req) > 4096) {
						pr_debug
						    ("total rq size %d bytes",
						     blk_rq_cur_bytes(req));
						debug_trap = 1;
					}
					bdev_make_request_queue(req);
					if (blk_rq_cur_bytes(req) != 4096) {
						pr_debug
						    ("total rq size %d bytes",
						     blk_rq_sectors(req) << 9);
						debug_trap = 1;
					}
					break;
				}
				break;
			}
		}
		spin_lock_irq(queue->queue_lock);
	}

}

static int gzd_write_page(struct gzd_bdev *dev, dma_addr_t mem,
			  uint64_t genz_addr)
{
	struct bdev_bio *bbio;
	struct tag_dma_info *tag_dma;
	uint32_t dcid = dev->cid;
	struct pci_dev *pdev;
	int tag;
	int retry, ret;
	long err;
	uint64_t chunk;

	pr_debug("%s %s: genz_addr=0x%llx\n", __func__, dev->gd->disk_name,
		 genz_addr);
	pdev = dev->card_data->core_info->pdev;
	bbio = bdev_get_bbio_req(NULL, NULL, PAGE_SIZE);
	tag_dma = bdev_get_tag_dma(mem, PAGE_SIZE, DMA_TO_DEVICE, 0);
	for (chunk = 0; chunk < PAGE_SIZE; chunk += CHUNK) {
		tag = bdev_get_tag(bbio, tag_dma);
		retry = 0;
retry:
		pr_debug
		    ("%s: block_io_request host_addr 0x%p, genz_addr 0x%p, size %d, tag %d, dcid 0x%x, write\n",
		     __func__, (void *)mem, (void *)genz_addr + chunk, CHUNK,
		     tag, dcid);
		ret =
		    gzd_stor_driver.block_io_request((void *)dev->card_data->hw,
						     (dma_addr_t) mem,
						     genz_addr + chunk, CHUNK,
						     tag, dcid, GZD_WRITE);
		if (ret == -EBUSY) {
			/* sleep for a bit to clear the fifo */
			pr_debug
			    ("%s: sleeping due to block_io_request EBUSY, tag %d\n",
			     __func__, tag);
			err =
			    wait_event_interruptible_timeout(dev->
							     card_data->block_io_queue,
							     dev->
							     card_data->block_io_ready,
							     GZD_TIMEOUT);
			if (err == 0) {	/* timeout expired */
				dev_err(&pdev->dev,
					"%s: timeout expired, tag %d\n",
					__func__, tag);
				ret = -EIO;
				break;
			} else {
				retry++;
				goto retry;
			}
		} else if (ret < 0) {
			dev_err(&pdev->dev,
				"%s: block_io_request returned %d for genz_addr 0x%llx\n",
				__func__, ret, genz_addr + chunk);
			break;
		}
	}
	pr_debug("%s %s: done, ret=%d\n", __func__, dev->gd->disk_name, ret);
	return ret;
}

static void gzd_zero_disk(struct gzd_bdev *dev, uint zero_mode)
{
	struct page *zero_page;
	void *zeros;
	dma_addr_t mem;
	size_t total_size = dev->size;
	size_t zero_size;
	struct pci_dev *pdev;
	int ret;
	uint64_t genz_addr, start_addr, end_addr;

	/* zero entire disk or 2MiB at start & end of disk */
	zero_size = (zero_mode == 1) ? total_size : min(MB(4), total_size);
	pr_debug("%s %s: zero_size=%zu\n", __func__,
		 dev->gd->disk_name, zero_size);
	pdev = dev->card_data->core_info->pdev;
	zero_page = alloc_page(GFP_KERNEL);
	if (!zero_page) {
		pr_err("%s %s: alloc_page error\n", __func__,
		       dev->gd->disk_name);
		return;
	}
	zeros = page_address(zero_page);
	memset(zeros, 0, PAGE_SIZE);
	mem = dma_map_page(&pdev->dev, zero_page, 0, PAGE_SIZE, DMA_TO_DEVICE);
	if (dma_mapping_error(&pdev->dev, mem)) {
		pr_err("%s %s: dma_map_page failed\n", __func__,
		       dev->gd->disk_name);
		return;
	}
	/* zero start of disk */
	start_addr = dev->base_zaddr;
	end_addr = start_addr + (zero_size / 2);
	for (genz_addr = start_addr; genz_addr < end_addr;
	     genz_addr += PAGE_SIZE) {
		ret = gzd_write_page(dev, mem, genz_addr);
		if (ret) {
			pr_err
			    ("%s %s: gzd_write_page failed, genz_addr=0x%llx, ret=%d\n",
			     __func__, dev->gd->disk_name, genz_addr, ret);
			break;
		}
	}
	/* zero end of disk */
	start_addr = dev->base_zaddr + total_size - (zero_size / 2);
	end_addr = start_addr + (zero_size / 2);
	for (genz_addr = start_addr; genz_addr < end_addr;
	     genz_addr += PAGE_SIZE) {
		ret = gzd_write_page(dev, mem, genz_addr);
		if (ret) {
			pr_err
			    ("%s %s: gzd_write_page failed, genz_addr=0x%llx, ret=%d\n",
			     __func__, dev->gd->disk_name, genz_addr, ret);
			break;
		}
	}
	/* Revisit: There's no guarantee that the DMA is done when we get here */
	dma_unmap_page(&pdev->dev, mem, PAGE_SIZE, DMA_TO_DEVICE);
	__free_pages(zero_page, 0);
	pr_debug("%s %s: done\n", __func__, dev->gd->disk_name);
}

static int gzd_stor_construct_bdev(struct gzd_bdev *dev,
				   size_t size, ulong offset,
				   int mindex, int bindex,
				   struct gzd_stor_card_data *card_data,
				   uint32_t cid, uint zero_mode)
{
	uint card_id = card_data->core_info->card_id;
	struct gzd_media_info *media_info =
	    &card_data->core_info->media_info[mindex];
	int err = 0;
	int depth;

	spin_lock_init(&dev->lock);
	spin_lock_init(&dev->queue_lock);

	dev->card_data = card_data;
	dev->size = size;
	dev->cid = cid;

	pr_debug("create queue: this device -%p queue-mode-%d\n", dev,
		 queue_mode);

//  Request-mode
	if (queue_mode == GZD_Q_RQ) {
		pr_debug("Current GZD_Q_RQ mode\n");

		dev->queue = blk_init_queue(gzd_request_queue_fn,
					    &dev->queue_lock);

		if (!dev->queue) {
			pr_debug("failed to create RQ-queue\n");
			goto out_free;
		}
	}
	blk_queue_logical_block_size(dev->queue, GZD_BLOCK_SIZE);
	dev->queue->queuedata = dev;
	depth = card_data->core_info->req_fifo_depth;

	err = blk_queue_init_tags(dev->queue, depth, NULL, BLK_TAG_ALLOC_FIFO);
	if (err) {
		pr_err
		    ("gzd_stor_construct_bdev: blk_queue_init_tags returned %d\n",
		     err);
		blk_cleanup_queue(dev->queue);
		dev->queue = NULL;
		return -1;
	}

	pr_debug("gzd_stor_construct_bdev: queue depth set to %d\n", depth);

	blk_queue_max_segments(dev->queue, 1);
	blk_queue_max_hw_sectors(dev->queue, PAGE_SIZE / KERNEL_SECTOR_SIZE);
	blk_queue_max_segment_size(dev->queue, PAGE_SIZE);
	/* GZD does not need bouncing. */
	blk_queue_bounce_limit(dev->queue, BLK_BOUNCE_ANY);

	dev->gd = alloc_disk(GZD_BDEV_MINORS);
	if (!dev->gd) {
		err = PTR_ERR(dev->gd);
		return err;
	}
	dev->gd->major = bdev_major;
	dev->gd->first_minor = card_data->bdev_start_minor +
	    ((mindex * num_bdevs + bindex) * GZD_BDEV_MINORS);
	dev->gd->fops = &gzd_bdev_ops;

// queue registration to gendisk
	dev->gd->queue = dev->queue;
// it is private data
	dev->gd->private_data = dev;
// storage offset, not page offset
	dev->base_zaddr = offset;

	scnprintf(dev->gd->disk_name, 32, GZD_BDEV_NAME "%u_%d_%d",
		  card_id, media_info->media_num, bindex);
	pr_info("%s: first_minor=%d, base_zaddr=0x%llx\n",
		dev->gd->disk_name, dev->gd->first_minor,
		(long long int)offset);
	set_capacity(dev->gd, size / KERNEL_SECTOR_SIZE);
	pr_info("%s: set capacity to %zu 512 byte sectors\n",
		dev->gd->disk_name, size / KERNEL_SECTOR_SIZE);

	if (zero_mode)
		gzd_zero_disk(dev, zero_mode);

	add_disk(dev->gd);
	return 0;

out_free:
	blk_cleanup_queue(dev->queue);
	return NULL;
}

/* ============================================================
 *                    THE CDEV FILE OPS
 * ============================================================ */

static uint gzd_stor_cdev_major;
static uint gzd_cdev_start_minor;
static struct class *gzd_cdev_class;

static int gzd_cdev_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct gzd_cdev *dev = (struct gzd_cdev *)filp->private_data;
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;

	if ((offset + (vma->vm_end - vma->vm_start)) > dev->size)
		return -EINVAL;

	offset += (unsigned long)dev->mmio_addr;
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	if (io_remap_pfn_range(vma, vma->vm_start,
			       offset >> PAGE_SHIFT,
			       vma->vm_end - vma->vm_start, vma->vm_page_prot))
		return -EAGAIN;

	return 0;
}

static ssize_t gzd_cdev_read(struct file *filp, char __user *buf,
			     size_t count, loff_t *fpos)
{
	struct gzd_cdev *dev = (struct gzd_cdev *)filp->private_data;
	uint64_t *media, val;
	ssize_t ret = 0;
	size_t remaining;

	if (*fpos >= dev->size)
		goto out;

	/* Revisit: support 4-byte alignment? */
	if (count % 8 || *fpos % 8) {
		ret = -EINVAL;
		goto out;
	}

	if (*fpos + count > dev->size)
		count = dev->size - *fpos;

	media = (uint64_t *) (dev->base_addr + *fpos);

	/* we must guarantee only 4- or 8-byte reads of media, which
	 * copy_to_user does not do, so we copy from media to an
	 * intermediate kernel buffer and then to the user
	 */
	for (remaining = count; remaining; buf += 8, media++, remaining -= 8) {
		/* Revisit: could make more efficient using __copy_to_user */
		/* Revisit: could make much more efficient by unrolling */
		val = *media;
		if (copy_to_user(buf, &val, 8)) {
			ret = -EFAULT;
			goto out;
		}
	}

	*fpos += count;
	ret = count;

out:
	return ret;
}

static ssize_t gzd_cdev_write(struct file *filp,
			      const char __user *buf, size_t count,
			      loff_t *fpos)
{
	struct gzd_cdev *dev = (struct gzd_cdev *)filp->private_data;
	uint64_t *media, val;
	ssize_t ret = 0;
	size_t remaining;

	if (*fpos >= dev->size)
		/* Revisit: should this return -EFBIG? */
		goto out;

	/* Revisit: support 4-byte alignment? */
	if (count % 8 || *fpos % 8) {
		ret = -EINVAL;
		goto out;
	}

	if (*fpos + count > dev->size)
		count = dev->size - *fpos;

	media = (uint64_t *) (dev->base_addr + *fpos);

	/* we must guarantee only 4- or 8-byte writes of media, which
	 * copy_from_user does not do, so we copy to an intermediate
	 * kernel buffer and then to the media
	 */
	for (remaining = count; remaining; buf += 8, media++, remaining -= 8) {
		/* Revisit: could make more efficient using __copy_from_user */
		/* Revisit: could make much more efficient by unrolling */
		if (copy_from_user(&val, buf, 8)) {
			ret = -EFAULT;
			goto out;
		}
		*media = val;
	}

	*fpos += count;
	ret = count;

out:
	return ret;
}

static loff_t gzd_cdev_llseek(struct file *filp, loff_t off, int whence)
{
	struct gzd_cdev *dev = (struct gzd_cdev *)filp->private_data;

	return generic_file_llseek_size(filp, off, whence, dev->size,
					dev->size);
}

static int gzd_cdev_open(struct inode *inode, struct file *filp)
{
	struct gzd_cdev *dev;

	dev = container_of(inode->i_cdev, struct gzd_cdev, cdev);

	/* store a pointer to our gzd_cdev for other file op functions */
	filp->private_data = dev;
	return 0;
}

static int gzd_cdev_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static const struct file_operations gzd_cdev_ops = {
	.owner = THIS_MODULE,
	.open = gzd_cdev_open,
	.mmap = gzd_cdev_mmap,
	.read = gzd_cdev_read,
	.write = gzd_cdev_write,
	.llseek = gzd_cdev_llseek,
	.release = gzd_cdev_release,
};

static ssize_t gzd_cdev_show_size(struct device *ddev,
				  struct device_attribute *attr, char *buf)
{
	struct gzd_cdev *dev = ddev->driver_data;

	return scnprintf(buf, PAGE_SIZE - 2, "%lu\n", dev->size);
}

static DEVICE_ATTR(size, S_IRUGO, gzd_cdev_show_size, NULL);

static int gzd_stor_construct_cdev(struct gzd_cdev *dev,
				   size_t size, ulong offset,
				   int mindex, int cindex,
				   struct gzd_stor_card_data *card_data)
{
	uint minor = card_data->cdev_start_minor + mindex * num_cdevs + cindex;
	uint card_id = card_data->core_info->card_id;
	dev_t devt = MKDEV(gzd_stor_cdev_major, minor);
	struct gzd_media_info *media_info =
	    &card_data->core_info->media_info[mindex];
	struct device *device;
	int err = 0;

	mutex_init(&dev->lock);

	cdev_init(&dev->cdev, &gzd_cdev_ops);
	dev->cdev.owner = THIS_MODULE;
	err = cdev_add(&dev->cdev, devt, 1);
	if (err)
		goto destroy;

	device = device_create(gzd_cdev_class, NULL, devt, dev,
			       GZD_CDEV_NAME "%u_%d_%d", card_id,
			       media_info->media_num, cindex);
	if (IS_ERR(device)) {
		err = PTR_ERR(device);
		goto del;
	}
	dev->devt = devt;
	dev->device = device;
	dev->card_data = card_data;
	dev->size = size;
	dev->mmio_addr = media_info->mmio_addr + offset;
	dev->base_addr = media_info->base_addr + offset;

	device_create_file(device, &dev_attr_size);

	return 0;

del:
	cdev_del(&dev->cdev);
destroy:
	mutex_destroy(&dev->lock);

	return err;
}

static void gzd_stor_destroy_cdev(struct gzd_cdev *dev)
{
	mutex_lock(&dev->lock);
	device_remove_file(dev->device, &dev_attr_size);
	device_destroy(gzd_cdev_class, dev->devt);
	cdev_del(&dev->cdev);
	mutex_unlock(&dev->lock);
	mutex_destroy(&dev->lock);
}

static size_t gzd_stor_cdev_size(size_t media_size,
				 uint cdev_percent, uint num_cdevs)
{
	ulong size;

	size = media_size * cdev_percent / 100 / num_cdevs;
	size = rounddown(size, PAGE_SIZE);

	return size;
}

static int gzd_cdev_probe(struct gzd_stor_card_data *card_data,
			  struct gzd_core_info *info)
{
	uint total_cdevs = 0;
	uint cdev_percent = 100 - bdev_percent;
	int i, j, err = 0;
	dev_t dev;
	size_t cdev_size;
	ulong offset;

	mutex_lock(&gzd_stor_lock);

	if (cdev_percent > 0)
		total_cdevs = info->num_media * num_cdevs;

	if (total_cdevs > 0) {
		/* obtain major and a range of minor numbers */
		err = alloc_chrdev_region(&dev, gzd_cdev_start_minor,
					  total_cdevs, GZD_CDEV_NAME);
		if (err < 0) {
			pr_err("alloc_chrdev_region error %d\n", err);
			goto unlock;
		}
		gzd_stor_cdev_major = MAJOR(dev);
		card_data->cdev =
		    kzalloc(total_cdevs * sizeof(struct gzd_cdev), GFP_KERNEL);
		if (!card_data->cdev) {
			err = -ENOMEM;
			goto unlock;
		}
	}

	card_data->num_cdevs = total_cdevs;
	card_data->cdev_start_minor = gzd_cdev_start_minor;
	gzd_cdev_start_minor += total_cdevs;
	mutex_unlock(&gzd_stor_lock);

	if (total_cdevs > 0) {
		for (i = 0; i < info->num_media; i++) {
			cdev_size =
			    gzd_stor_cdev_size(info->media_info[i].size,
					       cdev_percent, num_cdevs);
			pr_info("cdev_size %lld\n", (long long int)cdev_size);
			offset = 0;	/* cdevs start at LZA 0 on each media */
			for (j = 0; j < num_cdevs; j++) {
				err =
				    gzd_stor_construct_cdev
				    (&card_data->cdev
				     [i * num_cdevs + j], cdev_size,
				     offset, i, j, card_data);
				if (err)
					goto fail;	/* Revisit: other cleanup */
				offset += cdev_size;
			}
			/* bdevs start after cdevs on each media */
			pr_info("media_info[%d].bdevs_offset = %lld\n",
				i, (long long int)offset);
			info->media_info[i].bdevs_offset = offset;
		}
	}

	return 0;

unlock:
	mutex_unlock(&gzd_stor_lock);
fail:
	return err;
}

static int gzd_bdev_probe(struct gzd_stor_card_data *card_data,
			  struct gzd_core_info *info)
{
	uint total_bdevs = 0;
	int i, j, err = 0;
	size_t bdev_size;
	ulong offset;
	uint cid, zero_mode;

	mutex_lock(&gzd_stor_lock);

	if (bdev_percent > 0)
		total_bdevs = info->num_media * num_bdevs;

	if (total_bdevs > 0) {
		/* Use dynamic major number assignment by passing 0 */
		if (gzd_bdev_start_minor == 0)	/* only on first probe */
			bdev_major = register_blkdev(0, GZD_BDEV_NAME);
		if (bdev_major <= 0) {
			pr_err
			    ("register_blkdev: unable to get major number %d\n",
			     bdev_major);
			goto unlock;
		}
		card_data->bdev =
		    kzalloc(total_bdevs * sizeof(struct gzd_bdev), GFP_KERNEL);
		if (!card_data->bdev) {
			err = -ENOMEM;
			goto unlock;
		}
	}

	card_data->num_bdevs = total_bdevs;
	card_data->bdev_start_minor = gzd_bdev_start_minor;
	gzd_bdev_start_minor += (total_bdevs * GZD_BDEV_MINORS);
	mutex_unlock(&gzd_stor_lock);

	if (total_bdevs > 0) {
		/* Register blockdevice irq handler */
		if (gzd_stor_driver.request_irq != NULL) {

			gzd_stor_driver.request_irq(card_data->hw,
						    BlockRWInt,
						    bdev_irq_handler, 0,
						    GZD_STOR_DRV_NAME,
						    card_data);
		}

		init_waitqueue_head(&card_data->block_io_queue);

		for (i = 0; i < info->num_media; i++) {

			bdev_size =
			    gzd_stor_bdev_size(info->media_info[i].size,
					       bdev_percent, num_bdevs);

			cid = info->media_info[i].cid;
			/* bdevs start after cdevs on each media */
			offset = info->media_info[i].bdevs_offset;
			zero_mode = (i < zero_bdevs_argc) ? zero_bdevs[i] : 0;

			for (j = 0; j < num_bdevs; j++) {

				err =
				    gzd_stor_construct_bdev
				    (&card_data->bdev
				     [i * num_bdevs + j], bdev_size,
				     offset, i, j, card_data, cid, zero_mode);

				if (err)
					goto fail;	/* Revisit: other cleanup */
				offset += bdev_size;
			}
		}
	}

	return 0;

unlock:
	mutex_unlock(&gzd_stor_lock);
fail:
	return -1;
}

/* probe will be called once per card */
static int gzd_stor_probe(void *arg)
{
	struct gzd_stor_card_data *card_data;
	struct gzd_core_info *info;
	int i, err;

	card_data = kzalloc(sizeof(*card_data), GFP_KERNEL);
	if (!card_data)
		return -ENOMEM;

	mutex_init(&card_data->lock);
	bdev_percent = min(bdev_percent, 100u);	/* range 0 - 100 */
	card_data->hw = arg;
	info = gzd_stor_driver.info(arg);
	card_data->core_info = info;
	pr_info("gzd_core_info: card_id=%u, num_cards=%u, num_media=%u "
		"genz_subnets=%u, req_fifo_depth=%u, resp_fifo_depth=%u\n",
		info->card_id, info->num_cards, info->num_media,
		info->genz_subnets, info->req_fifo_depth,
		info->resp_fifo_depth);
	for (i = 0; i < info->num_cards; i++)
		pr_info("gzd_core_info:   card_ids[%d]=%u\n",
			i, info->card_ids[i]);
	for (i = 0; i < info->num_media; i++)
		pr_info
		    ("gzd_core_info:   media_size[%d]=%lu, base_addr=%pa\n",
		     i, info->media_info[i].size,
		     &info->media_info[i].mmio_addr);
	err = gzd_cdev_probe(card_data, info);
	if (err)
		goto fail;
	err = gzd_bdev_probe(card_data, info);
	if (err)
		goto fail;
// queue
	list_add_tail(&card_data->list, &gzd_stor_card_list);
	return 0;

	/* Revisit: undo all of above */
fail:
	return err;
}

static struct gzd_stor_card_data *gzd_stor_find_card_data(void *arg)
{
	struct gzd_stor_card_data *ptr, *next;

	list_for_each_entry_safe(ptr, next, &gzd_stor_card_list, list) {
		if (ptr->hw == arg)
			return ptr;
	}

	pr_err("invalid gzd_stor hw pointer: %p\n", arg);
	return 0;
}

static void gzd_stor_remove(void *arg)
{
	struct gzd_stor_card_data *card_data;
	int i;

	mutex_lock(&gzd_stor_lock);
	card_data = gzd_stor_find_card_data(arg);
	if (!card_data)
		goto unlock;

	for (i = 0; i < card_data->num_cdevs; i++)
		gzd_stor_destroy_cdev(&card_data->cdev[i]);
	unregister_chrdev_region(MKDEV(gzd_stor_cdev_major,
				       card_data->cdev_start_minor),
				 card_data->num_cdevs);
	kfree(card_data->cdev);
	for (i = 0; i < card_data->num_bdevs; i++) {
		del_gendisk(card_data->bdev[i].gd);
		//put_disk(card_data->bdev[i].gd);
		blk_cleanup_queue(card_data->bdev[i].queue);
	}
	/* free block device irq handler */
	if (card_data->num_bdevs > 0 && gzd_stor_driver.free_irq != NULL) {
		gzd_stor_driver.free_irq(card_data->hw, 0, card_data);
	}
	list_del_init(&card_data->list);
	if (card_data->num_bdevs > 0 && list_empty(&gzd_stor_card_list))	/* only on last remove */
		unregister_blkdev(bdev_major, GZD_BDEV_NAME);
	kfree(card_data);

unlock:
	mutex_unlock(&gzd_stor_lock);
}

static int __init gzd_stor_init(void)
{
	/* populate cdev sysfs entries */
	gzd_cdev_class = class_create(THIS_MODULE, GZD_CDEV_NAME);
	if (IS_ERR(gzd_cdev_class)) {
		pr_err("cdev class_create error\n");
		return PTR_ERR(gzd_cdev_class);
	}

	gzd_core_register_driver(&gzd_stor_driver);

	pr_debug("gzd_stor_init: block_io_request function is 0x%p\n",
		 gzd_stor_driver.block_io_request);

	return 0;
}

static void __exit gzd_stor_fini(void)
{
	gzd_core_unregister_driver(&gzd_stor_driver);
	class_destroy(gzd_cdev_class);
}

module_init(gzd_stor_init);
module_exit(gzd_stor_fini);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Storage driver for the Gen-Z demo pci device");
