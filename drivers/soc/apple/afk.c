// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2022 Sven Peter <sven@svenpeter.dev> */

#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/soc/apple/rtkit.h>

#include "afk.h"
#ifdef AFK_TRACE
#include "trace.h"
#else
#define trace_afk_recv_handle(ep, channel, type, data_size, ehdr, eshdr) ((void)0)
#define trace_afk_recv_qe(ep, rptr, magic, size) ((void)0)
#define trace_afk_send_rwptr_pre(ep, rptr, wptr) ((void)0)
#define trace_afk_recv_rwptr_pre(ep, rptr, wptr) ((void)0)
#define trace_afk_send_rwptr_post(ep, rptr, wptr) ((void)0)
#define trace_afk_recv_rwptr_post(ep, rptr, wptr) ((void)0)
#define trace_afk_getbuf(ep, size, tag) ((void)0)
#endif

struct afk_receive_message_work {
	struct apple_dcp_afkep *ep;
	u64 message;
	struct work_struct work;
};

#define RBEP_TYPE GENMASK(63, 48)

enum rbep_msg_type {
	RBEP_INIT = 0x80,
	RBEP_INIT_ACK = 0xa0,
	RBEP_GETBUF = 0x89,
	RBEP_GETBUF_ACK = 0xa1,
	RBEP_INIT_TX = 0x8a,
	RBEP_INIT_RX = 0x8b,
	RBEP_INIT_RXTX_ACK = 0x8c,
	RBEP_START = 0xa3,
	RBEP_START_ACK = 0x86,
	RBEP_SEND = 0xa2,
	RBEP_RECV = 0x85,
	RBEP_SHUTDOWN = 0xc0,
	RBEP_SHUTDOWN_ACK = 0xc1,
};

#define BLOCK_SHIFT 6
#define ROUNDTRIP_BUF_SIZE 0x1000

#define GETBUF_SIZE GENMASK(31, 16)
#define GETBUF_TAG GENMASK(15, 0)
#define GETBUF_ACK_DVA GENMASK(47, 0)

#define INITRB_OFFSET GENMASK(47, 32)
#define INITRB_SIZE GENMASK(31, 16)
#define INITRB_TAG GENMASK(15, 0)

#define SEND_WPTR GENMASK(31, 0)

static void afk_send(struct apple_dcp_afkep *ep, u64 message)
{
	// TODO replaced call to dcp_send_message() to call rtkit directly
	// this removes the trace call to trace_dcp_send_msg(). we could make
	// this call an afk ep op (fn ptr) instead.
	apple_rtkit_send_message(ep->rtk, ep->endpoint, message, NULL, true);
}

struct apple_dcp_afkep *afk_init(struct device *dev, struct apple_rtkit *rtk,
			void *priv, u32 endpoint, const struct apple_epic_service_ops *ops,
			const struct apple_afk_epic_ops *ep_ops)
{
	struct apple_dcp_afkep *afkep;
	int ret;

	afkep = devm_kzalloc(dev, sizeof(*afkep), GFP_KERNEL);
	if (!afkep)
		return ERR_PTR(-ENOMEM);

	afkep->dev = dev;
	afkep->rtk = rtk;
	afkep->priv = priv;
	afkep->ep_ops = ep_ops;
	afkep->ops = ops;
	afkep->endpoint = endpoint;
	afkep->wq = alloc_ordered_workqueue("apple-dcp-afkep%02x",
					    WQ_MEM_RECLAIM, endpoint);
	if (!afkep->wq) {
		ret = -ENOMEM;
		goto out_free_afkep;
	}

	// TODO: devm_ for wq

	init_completion(&afkep->started);
	init_completion(&afkep->stopped);
	spin_lock_init(&afkep->lock);

	return afkep;

out_free_afkep:
	devm_kfree(dev, afkep);
	return ERR_PTR(ret);
}

int afk_start(struct apple_dcp_afkep *ep)
{
	int ret;

	reinit_completion(&ep->started);
	apple_rtkit_start_ep(ep->rtk, ep->endpoint);
	afk_send(ep, FIELD_PREP(RBEP_TYPE, RBEP_INIT));

	ret = wait_for_completion_timeout(&ep->started, msecs_to_jiffies(1000));
	if (ret <= 0)
		return -ETIMEDOUT;
	else
		return 0;
	return 0;
}

int afk_start_bulk(struct apple_dcp_afkep **eps, int num)
{
	int i, ret;

	for (i = 0; i < num; i++) {
		struct apple_dcp_afkep *ep = eps[i];
		reinit_completion(&ep->started);
		apple_rtkit_start_ep(ep->rtk, ep->endpoint);
		afk_send(ep, FIELD_PREP(RBEP_TYPE, RBEP_INIT));
	}

	for (i = 0; i < num; i++) {
		struct apple_dcp_afkep *ep = eps[i];
		ret = wait_for_completion_timeout(&ep->started, msecs_to_jiffies(1000));
		if (ret <= 0) {
			dev_warn(ep->dev, "Timed out on starting endpoint %x\n",
				ep->endpoint);
		}
	}

	return 0;
}

static void afk_alloc_roundtrip(struct apple_dcp_afkep *ep, u64 message,
							   struct afk_ringbuffer *bfr)
{
	u32 size = ROUNDTRIP_BUF_SIZE;

	bfr->buf = dmam_alloc_coherent(ep->dev, size, &ep->bfr_dma,
				      GFP_KERNEL);
	if (!bfr->buf) {
		dev_err(ep->dev, "Failed to allocate %d bytes buffer\n",
			size);
		return;
	}

	bfr->buf = bfr->buf;
	bfr->bufsz = size;
	bfr->ready = true;
}

static void afk_init_roundtrip(struct apple_dcp_afkep *ep, u64 message)
{
	afk_alloc_roundtrip(ep, message, &ep->rt_rxbfr);
	afk_alloc_roundtrip(ep, message, &ep->rt_txbfr);
	afk_send(ep, FIELD_PREP(RBEP_TYPE, RBEP_INIT_ACK));
}

static void afk_getbuf(struct apple_dcp_afkep *ep, u64 message)
{
	u32 size = FIELD_GET(GETBUF_SIZE, message) << BLOCK_SHIFT;
	u16 tag = FIELD_GET(GETBUF_TAG, message);
	u64 reply;

	trace_afk_getbuf(ep, size, tag);

	if (ep->bfr) {
		dev_err(ep->dev,
			"Got GETBUF message but buffer already exists\n");
		return;
	}

	ep->bfr = dmam_alloc_coherent(ep->dev, size, &ep->bfr_dma,
				      GFP_KERNEL);
	if (!ep->bfr) {
		dev_err(ep->dev, "Failed to allocate %d bytes buffer\n",
			size);
		return;
	}

	ep->bfr_size = size;
	ep->bfr_tag = tag;

	reply = FIELD_PREP(RBEP_TYPE, RBEP_GETBUF_ACK);
	reply |= FIELD_PREP(GETBUF_ACK_DVA, ep->bfr_dma);
	afk_send(ep, reply);
}

/*
The first three blocks of the ringbuffer is reserved for exchanging bufsz,
rptr, wptr:

             bufsz      unk
00000000  00007e80 00070006 00000000 00000000 00000000 00000000 00000000 00000000
00000020  00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000
00000040  *   rptr
00000080  00000600 00000000 00000000 00000000 00000000 00000000 00000000 00000000
000000a0  00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000
000000c0  *   wptr
00000100  00000680 00000000 00000000 00000000 00000000 00000000 00000000 00000000
00000120  00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000
00000140  *

Note how each block is spread out by some blksz multiple of 0x40
(step) or the BLOCK_SHIFT. Here, blksz is 0x80. The 0th block holds the bufsize,
the 1st block holds the rptr, and the 2nd block holds the wptr. The
actual contents of the ringbuffer starts after the first three blocks,
which will be collectively called the "header".
*/

static inline u32 afk_rb_get_rptr(struct afk_ringbuffer *bfr)
{
	__le32 rptr = *(__le32 *)(bfr->hdr + bfr->blksz * 1);
	return le32_to_cpu(rptr);
}

static inline u32 afk_rb_get_wptr(struct afk_ringbuffer *bfr)
{
	__le32 wptr = *(__le32 *)(bfr->hdr + bfr->blksz * 2);
	return le32_to_cpu(wptr);
}

static inline void afk_rb_set_rptr(struct afk_ringbuffer *bfr, u32 rptr)
{
	*(__le32 *)(bfr->hdr + bfr->blksz * 1) = cpu_to_le32(rptr);
}

static inline void afk_rb_set_wptr(struct afk_ringbuffer *bfr, u32 wptr)
{
	*(__le32 *)(bfr->hdr + bfr->blksz * 2) = cpu_to_le32(wptr);
}

static void afk_init_rxtx(struct apple_dcp_afkep *ep, u64 message,
			  struct afk_ringbuffer *bfr)
{
	u32 base = FIELD_GET(INITRB_OFFSET, message) << BLOCK_SHIFT;
	u32 size = FIELD_GET(INITRB_SIZE, message) << BLOCK_SHIFT;
	u16 tag = FIELD_GET(INITRB_TAG, message);
	u32 bufsz, hdrsz, blksz, end;

	if (tag != ep->bfr_tag) {
		dev_err(ep->dev, "AFK[ep:%02x]: expected tag 0x%x but got 0x%x",
			ep->endpoint, ep->bfr_tag, tag);
		return;
	}

	if (bfr->ready) {
		dev_err(ep->dev, "AFK[ep:%02x]: buffer is already initialized\n",
			ep->endpoint);
		return;
	}

	if (base >= ep->bfr_size) {
		dev_err(ep->dev,
			"AFK[ep:%02x]: requested base 0x%x >= max size 0x%lx",
			ep->endpoint, base, ep->bfr_size);
		return;
	}

	end = base + size;
	if (end > ep->bfr_size) {
		dev_err(ep->dev,
			"AFK[ep:%02x]: requested end 0x%x > max size 0x%lx",
			ep->endpoint, end, ep->bfr_size);
		return;
	}

	bfr->hdr = ep->bfr + base;

	/* Recall the first three blocks are bufsz, rptr, wptr. bufsz is thus
	 * always located at (ep->bfr + base) + blksize * 0, or simply the
	 * ringbuffer base address.
	 */
	bufsz = le32_to_cpu(*(__le32 *)bfr->hdr);

	/* In the mailbox message we're given "size", which is the total ringbuffer
	 * size (header + body). Recall in the 0th block we found "bufsz", which is
	 * the ringbuffer *body* size. Subtract to calculate the header size.
	 */
	if (size <= bufsz) {
		dev_err(ep->dev,
			"AFK[ep:%02x]: total ringbuffer size (0x%x) > body size (0x%x)",
			ep->endpoint, size, bufsz);
		return;
	}
	hdrsz = size - bufsz;

	/* The header always has three blocks: bufsz, rptr, wptr. Divide the header
	 * size by 3 to bootstrap the ringbuffer block size.
	 */
	if (hdrsz % 3) {
		dev_err(ep->dev,
			"AFK[ep:%02x]: header size 0x%x (0x%x - 0x%x) must be multiple of 3",
			ep->endpoint, hdrsz, size, bufsz);
		return;
	}
	blksz = hdrsz / 3;

	if (blksz < (1 << BLOCK_SHIFT) || blksz % (1 << BLOCK_SHIFT)) {
		dev_err(ep->dev,
			"AFK[ep:%02x]: blksz 0x%x must be multiple of 0x%x",
			ep->endpoint, blksz, 1 << BLOCK_SHIFT);
		return;
	}

	bfr->buf = bfr->hdr + hdrsz;
	bfr->bufsz = bufsz;
	bfr->blksz = blksz;
	bfr->ready = true;

	if (ep->rxbfr.ready && ep->txbfr.ready)
		afk_send(ep, FIELD_PREP(RBEP_TYPE, RBEP_START));
}

const struct apple_epic_service_ops *
afk_match_service(struct apple_dcp_afkep *ep, const char *name)
{
	const struct apple_epic_service_ops *ops;

	if (!name[0])
		return NULL;
	if (!ep->ops)
		return NULL;

	for (ops = ep->ops; ops->name[0]; ops++) {
		if (strcmp(ops->name, name))
			continue;

		return ops;
	}

	return NULL;
}

struct apple_epic_service *afk_epic_find_service(struct apple_dcp_afkep *ep,
						 u32 channel)
{
    for (u32 i = 0; i < ep->num_channels; i++)
        if (ep->services[i].enabled && ep->services[i].channel == channel)
            return &ep->services[i];

    return NULL;
}

static void afk_recv_handle_teardown(struct apple_dcp_afkep *ep, u32 channel)
{
	struct apple_epic_service *service;
	const struct apple_epic_service_ops *ops;
	unsigned long flags;

	service = afk_epic_find_service(ep, channel);
	if (!service) {
		dev_warn(ep->dev, "AFK[ep:%02x]: teardown for disabled channel %u\n",
			 ep->endpoint, channel);
		return;
	}

	// TODO: think through what locking is necessary
	spin_lock_irqsave(&service->lock, flags);
	service->enabled = false;
	ops = service->ops;
	spin_unlock_irqrestore(&service->lock, flags);

	if (ops->teardown)
		ops->teardown(service);
}

static void afk_recv_handle_reply(struct apple_dcp_afkep *ep, u32 channel,
				  u16 tag, void *payload, size_t payload_size)
{
	struct epic_cmd *cmd = payload;
	struct apple_epic_service *service;
	unsigned long flags;
	u8 idx = tag & 0xff;
	void *rxbuf, *txbuf;
	dma_addr_t rxbuf_dma, txbuf_dma;
	size_t rxlen, txlen;

	service = afk_epic_find_service(ep, channel);
	if (!service) {
		dev_warn(ep->dev, "AFK[ep:%02x]: command reply on disabled channel %u\n",
			 ep->endpoint, channel);
		return;
	}

	if (payload_size < sizeof(*cmd)) {
		dev_err(ep->dev,
			"AFK[ep:%02x]: command reply on channel %d too small: %ld\n",
			ep->endpoint, channel, payload_size);
		return;
	}

	if (idx >= MAX_PENDING_CMDS) {
		dev_err(ep->dev,
			"AFK[ep:%02x]: command reply on channel %d out of range: %d\n",
			ep->endpoint, channel, idx);
		return;
	}

	spin_lock_irqsave(&service->lock, flags);
	if (service->cmds[idx].done) {
		dev_err(ep->dev,
			"AFK[ep:%02x]: command reply on channel %d already handled\n",
			ep->endpoint, channel);
		spin_unlock_irqrestore(&service->lock, flags);
		return;
	}

	if (tag != service->cmds[idx].tag) {
		dev_err(ep->dev,
			"AFK[ep:%02x]: command reply on channel %d has invalid tag: expected 0x%04x != 0x%04x\n",
			ep->endpoint, channel, tag, service->cmds[idx].tag);
		spin_unlock_irqrestore(&service->lock, flags);
		return;
	}

	service->cmds[idx].done = true;
	service->cmds[idx].retcode = le32_to_cpu(cmd->retcode);
	if (service->cmds[idx].free_on_ack) {
		/* defer freeing until we're no longer in atomic context */
		rxbuf = service->cmds[idx].rxbuf;
		txbuf = service->cmds[idx].txbuf;
		rxlen = service->cmds[idx].rxlen;
		txlen = service->cmds[idx].txlen;
		rxbuf_dma = service->cmds[idx].rxbuf_dma;
		txbuf_dma = service->cmds[idx].txbuf_dma;
		bitmap_release_region(service->cmd_map, idx, 0);
	} else {
		rxbuf = txbuf = NULL;
		rxlen = txlen = 0;
	}
	if (service->cmds[idx].completion)
		complete(service->cmds[idx].completion);

	spin_unlock_irqrestore(&service->lock, flags);

	if (rxbuf && rxlen)
		dma_free_coherent(ep->dev, rxlen, rxbuf, rxbuf_dma);
	if (txbuf && txlen)
		dma_free_coherent(ep->dev, txlen, txbuf, txbuf_dma);
}

struct epic_std_service_ap_call {
	__le32 unk0;
	__le32 unk1;
	__le32 type;
	__le32 len;
	__le32 magic;
	u8 _unk[48];
} __attribute__((packed));

static void afk_recv_handle_std_service(struct apple_dcp_afkep *ep, u32 channel,
					u32 type, struct epic_hdr *ehdr,
					struct epic_sub_hdr *eshdr,
					void *payload, size_t payload_size)
{
	struct apple_epic_service *service = afk_epic_find_service(ep, channel);

	if (!service) {
		dev_warn(ep->dev,
			 "AFK[ep:%02x]: std service notify on disabled channel %u\n",
			 ep->endpoint, channel);
		return;
	}

	if (type == EPIC_TYPE_NOTIFY && eshdr->category == EPIC_CAT_NOTIFY) {
		struct epic_std_service_ap_call *call = payload;
		size_t call_size;
		void *reply;
		int ret;

		if (payload_size < sizeof(*call))
			return;

		call_size = le32_to_cpu(call->len);
		if (payload_size < sizeof(*call) + call_size)
			return;

		if (!service->ops->call)
			return;
		reply = kzalloc(payload_size, GFP_KERNEL);
		if (!reply)
			return;

		ret = service->ops->call(service, le32_to_cpu(call->type),
					 payload + sizeof(*call), call_size,
					 reply + sizeof(*call), call_size);
		if (ret) {
			kfree(reply);
			return;
		}

		memcpy(reply, call, sizeof(*call));
		afk_send_epic(ep, channel, le16_to_cpu(eshdr->tag),
			      EPIC_TYPE_NOTIFY_ACK, EPIC_CAT_REPLY,
			      EPIC_SUBTYPE_STD_SERVICE, reply, payload_size);
		kfree(reply);

		return;
	}

	if (type == EPIC_TYPE_NOTIFY && eshdr->category == EPIC_CAT_REPORT) {
		if (service->ops->report)
			service->ops->report(service, le16_to_cpu(eshdr->type),
					     payload, payload_size);
		return;
	}

	dev_err(ep->dev,
		"AFK[ep:%02x]: channel %d received unhandled standard service message: %x / %x\n",
		ep->endpoint, channel, type, eshdr->category);
	print_hex_dump(KERN_INFO, "AFK: ", DUMP_PREFIX_NONE, 16, 1, payload,
				   payload_size, true);
}

static void afk_recv_handle(struct apple_dcp_afkep *ep, u32 channel, u32 type,
			    u8 *data, size_t data_size)
{
	struct apple_epic_service *service;
	struct epic_hdr *ehdr = (struct epic_hdr *)data;
	struct epic_sub_hdr *eshdr =
		(struct epic_sub_hdr *)(data + sizeof(*ehdr));
	u16 subtype = le16_to_cpu(eshdr->type);
	u8 *payload = data + sizeof(*ehdr) + sizeof(*eshdr);
	size_t payload_size;

	if (data_size < sizeof(*ehdr) + sizeof(*eshdr)) {
		dev_err(ep->dev, "AFK[ep:%02x]: payload too small: %lx\n",
			ep->endpoint, data_size);
		return;
	}
	payload_size = data_size - sizeof(*ehdr) - sizeof(*eshdr);

	trace_afk_recv_handle(ep, channel, type, data_size, ehdr, eshdr);

	service = afk_epic_find_service(ep, channel);

	if (!service) {
		if (type != EPIC_TYPE_NOTIFY && type != EPIC_TYPE_REPLY) {
			dev_err(ep->dev,
				"AFK[ep:%02x]: expected notify but got 0x%x on channel %d\n",
				ep->endpoint, type, channel);
			return;
		}
		if (eshdr->category != EPIC_CAT_REPORT) {
			dev_err(ep->dev,
				"AFK[ep:%02x]: expected report but got 0x%x on channel %d\n",
				ep->endpoint, eshdr->category, channel);
			return;
		}
		if (subtype == EPIC_SUBTYPE_TEARDOWN) {
			dev_dbg(ep->dev,
				"AFK[ep:%02x]: teardown without service on channel %d\n",
				ep->endpoint, channel);
			return;
		}
		if (subtype != EPIC_SUBTYPE_ANNOUNCE &&
		    subtype != EPIC_SUBTYPE_STD_SERVICE) { // aop uses std as announce
			dev_err(ep->dev,
				"AFK[ep:%02x]: expected announce but got 0x%x on channel %d\n",
				ep->endpoint, subtype, channel);
			return;
		}

		return ep->ep_ops->recv_handle_init(ep, subtype, channel, payload, payload_size);
	}

	if (!service) {
		dev_err(ep->dev, "AFK[ep:%02x]: channel %d has no service\n",
			ep->endpoint, channel);
		return;
	}

	if (type == EPIC_TYPE_NOTIFY && eshdr->category == EPIC_CAT_REPORT &&
	    subtype == EPIC_SUBTYPE_TEARDOWN)
		return afk_recv_handle_teardown(ep, channel);

	if (type == EPIC_TYPE_REPLY && eshdr->category == EPIC_CAT_REPLY)
		return afk_recv_handle_reply(ep, channel,
					     le16_to_cpu(eshdr->tag), payload,
					     payload_size);

	if (subtype == EPIC_SUBTYPE_STD_SERVICE)
		return afk_recv_handle_std_service(
			ep, channel, type, ehdr, eshdr, payload, payload_size);

	dev_err(ep->dev, "AFK[ep:%02x]: channel %d received unhandled message "
		"(type %x subtype %x)\n", ep->endpoint, channel, type, subtype);
	print_hex_dump(KERN_INFO, "AFK: ", DUMP_PREFIX_NONE, 16, 1, payload,
				   payload_size, true);
}

static bool afk_recv(struct apple_dcp_afkep *ep)
{
	struct afk_qe *hdr;
	u32 rptr, wptr;
	u32 magic, size, channel, type;

	if (!ep->rxbfr.ready) {
		dev_err(ep->dev, "AFK[ep:%02x]: got RECV but not ready\n",
			ep->endpoint);
		return false;
	}

	rptr = afk_rb_get_rptr(&ep->rxbfr);
	wptr = afk_rb_get_wptr(&ep->rxbfr);
	trace_afk_recv_rwptr_pre(ep, rptr, wptr);

	if (rptr == wptr)
		return false;

	if (rptr > (ep->rxbfr.bufsz - sizeof(*hdr))) {
		dev_warn(ep->dev,
			 "AFK[ep:%02x]: rptr out of bounds: 0x%x > 0x%lx\n",
			 ep->endpoint, rptr, ep->rxbfr.bufsz - sizeof(*hdr));
		return false;
	}

	dma_rmb();

	hdr = ep->rxbfr.buf + rptr;
	magic = le32_to_cpu(hdr->magic);
	size = le32_to_cpu(hdr->size);
	trace_afk_recv_qe(ep, rptr, magic, size);

	/* DCP uses magic 'IOP' both ways. AOP uses 'IOP' for TX and 'AOP' for RX.
	 * Allow both for simplicty. It's a single bit off (bit 3).
	 */
	if (magic != QE_MAGIC_IOP && magic != QE_MAGIC_AOP) {
		dev_warn(ep->dev, "AFK[ep:%02x]: invalid queue entry magic: 0x%x\n",
			 ep->endpoint, magic);
		return false;
	}

	/*
	 * If there's not enough space for the payload the co-processor inserted
	 * the current dummy queue entry and we have to advance to the next one
	 * which will contain the real data.
	*/
	if (rptr + size + sizeof(*hdr) > ep->rxbfr.bufsz) {
		rptr = 0;
		hdr = ep->rxbfr.buf + rptr;
		magic = le32_to_cpu(hdr->magic);
		size = le32_to_cpu(hdr->size);
		trace_afk_recv_qe(ep, rptr, magic, size);

		if (magic != QE_MAGIC_IOP && magic != QE_MAGIC_AOP) {
			dev_warn(ep->dev,
				 "AFK[ep:%02x]: invalid next queue entry magic: 0x%x\n",
				 ep->endpoint, magic);
			return false;
		}

		afk_rb_set_rptr(&ep->rxbfr, rptr);
	}

	if (rptr + size + sizeof(*hdr) > ep->rxbfr.bufsz) {
		dev_warn(ep->dev,
			 "AFK[ep:%02x]: queue entry out of bounds: 0x%lx > 0x%lx\n",
			 ep->endpoint, rptr + size + sizeof(*hdr), ep->rxbfr.bufsz);
		return false;
	}

	channel = le32_to_cpu(hdr->channel);
	type = le32_to_cpu(hdr->type);

	rptr = ALIGN(rptr + sizeof(*hdr) + size, 1 << BLOCK_SHIFT);
	if (WARN_ON(rptr > ep->rxbfr.bufsz))
		rptr = 0;
	if (rptr == ep->rxbfr.bufsz)
		rptr = 0;

	dma_mb();

	afk_rb_set_rptr(&ep->rxbfr, rptr);
	trace_afk_recv_rwptr_post(ep, rptr, wptr);
	/*
	 * TODO: this is theoretically unsafe since DCP could overwrite data
	 *       after the read pointer was updated above. Do it anyway since
	 *       it avoids 2 problems in the DCP tracer:
	 *       1. the tracer sees replies before the the notifies from dcp
	 *       2. the tracer tries to read buffers after they are unmapped.
	 */
	afk_recv_handle(ep, channel, type, hdr->data, size);

	return true;
}

static void afk_receive_message_worker(struct work_struct *work_)
{
	struct afk_receive_message_work *work;
	u16 type;

	work = container_of(work_, struct afk_receive_message_work, work);

	type = FIELD_GET(RBEP_TYPE, work->message);
	switch (type) {
	case RBEP_INIT_ACK:
		break;

	case RBEP_START_ACK:
		complete_all(&work->ep->started);
		break;

	case RBEP_SHUTDOWN_ACK:
		complete_all(&work->ep->stopped);
		break;

	case RBEP_INIT: // RX is used to init roundtrip bfrs
		afk_init_roundtrip(work->ep, work->message);
		break;

	case RBEP_GETBUF:
		afk_getbuf(work->ep, work->message);
		break;

	case RBEP_INIT_TX:
		afk_init_rxtx(work->ep, work->message, &work->ep->txbfr);
		break;

	case RBEP_INIT_RX:
		afk_init_rxtx(work->ep, work->message, &work->ep->rxbfr);
		break;

	case RBEP_INIT_RXTX_ACK:
		break; // noop

	case RBEP_RECV:
		while (afk_recv(work->ep))
			;
		break;

	default:
		dev_err(work->ep->dev,
			"Received unknown AFK message type: 0x%x\n", type);
	}

	kfree(work);
}

int afk_receive_message(struct apple_dcp_afkep *ep, u64 message)
{
	struct afk_receive_message_work *work;

	// TODO: comment why decoupling from rtkit thread is required here
	work = kzalloc(sizeof(*work), GFP_KERNEL);
	if (!work)
		return -ENOMEM;

	work->ep = ep;
	work->message = message;
	INIT_WORK(&work->work, afk_receive_message_worker);
	queue_work(ep->wq, &work->work);

	return 0;
}

int afk_send_epic(struct apple_dcp_afkep *ep, u32 channel, u16 tag,
		  enum epic_type etype, enum epic_category ecat, u8 stype,
		  const void *payload, size_t payload_len)
{
	u32 rptr, wptr;
	struct afk_qe *hdr, *hdr2;
	struct epic_hdr *ehdr;
	struct epic_sub_hdr *eshdr;
	unsigned long flags;
	size_t total_epic_size, total_size;
	int ret;

	spin_lock_irqsave(&ep->lock, flags);

	dma_rmb();
	rptr = afk_rb_get_rptr(&ep->txbfr);
	wptr = afk_rb_get_wptr(&ep->txbfr);
	trace_afk_send_rwptr_pre(ep, rptr, wptr);
	total_epic_size = sizeof(*ehdr) + sizeof(*eshdr) + payload_len;
	total_size = sizeof(*hdr) + total_epic_size;

	hdr = hdr2 = NULL;

	/*
	 * We need to figure out how to place the entire headers and payload
	 * into the ring buffer:
	 * - If the write pointer is in front of the read pointer we just need
	 *   enough space inbetween to store everything.
	 * - If the read pointer has already wrapper around the end of the
	 *   buffer we can
	 *    a) either store the entire payload at the writer pointer if
	 *       there's enough space until the end,
	 *    b) or just store the queue entry at the write pointer to indicate
	 *       that we need to wrap to the start and then store the headers
	 *       and the payload at the beginning of the buffer. The queue
	 *       header has to be store twice in this case.
	 * In either case we have to ensure that there's always enough space
	 * so that we don't accidentally overwrite other buffers.
	 */
	if (wptr < rptr) {
		/*
		 * If wptr < rptr we can't wrap around and only have to make
		 * sure that there's enough space for the entire payload.
		 */
		if (wptr + total_size > rptr) {
			ret = -ENOMEM;
			goto out;
		}

		hdr = ep->txbfr.buf + wptr;
		wptr += sizeof(*hdr);
	} else {
		/* We need enough space to place at least a queue entry */
		if (wptr + sizeof(*hdr) > ep->txbfr.bufsz) {
			ret = -ENOMEM;
			goto out;
		}

		/*
		 * If we can place a single queue entry but not the full payload
		 * we need to place one queue entry at the end of the ring
		 * buffer and then another one together with the entire
		 * payload at the beginning.
		 */
		if (wptr + total_size > ep->txbfr.bufsz) {
			/*
			 * Ensure there's space for the  queue entry at the
			 * beginning
			 */
			if (sizeof(*hdr) > rptr) {
				ret = -ENOMEM;
				goto out;
			}

			/*
			 * Place two queue entries to indicate we want to wrap
			 * around to the firmware.
			 */
			hdr = ep->txbfr.buf + wptr;
			hdr2 = ep->txbfr.buf;
			wptr = sizeof(*hdr);

			/* Ensure there's enough space for the entire payload */
			if (wptr + total_epic_size > rptr) {
				ret = -ENOMEM;
				goto out;
			}
		} else {
			/* We have enough space to place the entire payload */
			hdr = ep->txbfr.buf + wptr;
			wptr += sizeof(*hdr);
		}
	}
	/*
	 * At this point we're guaranteed that hdr (and possibly hdr2) point
	 * to a buffer large enough to fit the queue entry and that we have
	 * enough space at wptr to store the payload.
	 */

	hdr->magic = cpu_to_le32(QE_MAGIC_IOP);
	hdr->size = cpu_to_le32(total_epic_size);
	hdr->channel = cpu_to_le32(channel);
	hdr->type = cpu_to_le32(etype);
	if (hdr2)
		memcpy(hdr2, hdr, sizeof(*hdr));

	ehdr = ep->txbfr.buf + wptr;
	memset(ehdr, 0, sizeof(*ehdr));
	ehdr->version = 2;
	ehdr->seq = cpu_to_le16(ep->qe_seq++);
	ehdr->timestamp = cpu_to_le64(0);
	wptr += sizeof(*ehdr);

	eshdr = ep->txbfr.buf + wptr;
	memset(eshdr, 0, sizeof(*eshdr));
	eshdr->length = cpu_to_le32(payload_len);
	eshdr->version = 4;
	eshdr->category = ecat;
	eshdr->type = cpu_to_le16(stype);
	eshdr->timestamp = cpu_to_le64(0);
	eshdr->tag = cpu_to_le16(tag);
	if (ecat == EPIC_CAT_REPLY)
		eshdr->inline_len = cpu_to_le16(payload_len - 4);
	else
		eshdr->inline_len = cpu_to_le16(0);
	wptr += sizeof(*eshdr);

	memcpy(ep->txbfr.buf + wptr, payload, payload_len);
	wptr += payload_len;
	wptr = ALIGN(wptr, 1 << BLOCK_SHIFT);
	if (wptr == ep->txbfr.bufsz)
		wptr = 0;
	trace_afk_send_rwptr_post(ep, rptr, wptr);

	afk_rb_set_wptr(&ep->txbfr, wptr);
	afk_send(ep, FIELD_PREP(RBEP_TYPE, RBEP_SEND) |
			     FIELD_PREP(SEND_WPTR, wptr));
	ret = 0;

out:
	spin_unlock_irqrestore(&ep->lock, flags);
	return ret;
}

int afk_send_command(struct apple_epic_service *service, u8 type,
		     const void *payload, size_t payload_len, void *output,
		     size_t output_len, u32 *retcode)
{
	struct epic_cmd cmd;
	void *rxbuf, *txbuf;
	dma_addr_t rxbuf_dma, txbuf_dma;
	unsigned long flags;
	int ret, idx;
	u16 tag;
	struct apple_dcp_afkep *ep = service->ep;
	DECLARE_COMPLETION_ONSTACK(completion);

	rxbuf = dma_alloc_coherent(ep->dev, output_len, &rxbuf_dma,
				   GFP_KERNEL);
	if (!rxbuf)
		return -ENOMEM;
	txbuf = dma_alloc_coherent(ep->dev, payload_len, &txbuf_dma,
				   GFP_KERNEL);
	if (!txbuf) {
		ret = -ENOMEM;
		goto err_free_rxbuf;
	}

	memcpy(txbuf, payload, payload_len);

	memset(&cmd, 0, sizeof(cmd));
	cmd.retcode = cpu_to_le32(0);
	cmd.rxbuf = cpu_to_le64(rxbuf_dma);
	cmd.rxlen = cpu_to_le32(output_len);
	cmd.txbuf = cpu_to_le64(txbuf_dma);
	cmd.txlen = cpu_to_le32(payload_len);

	spin_lock_irqsave(&service->lock, flags);
	idx = bitmap_find_free_region(service->cmd_map, MAX_PENDING_CMDS, 0);
	if (idx < 0) {
		ret = -ENOSPC;
		goto err_unlock;
	}

	tag = (service->cmd_tag & 0xff) << 8;
	tag |= idx & 0xff;
	service->cmd_tag++;

	service->cmds[idx].tag = tag;
	service->cmds[idx].rxbuf = rxbuf;
	service->cmds[idx].txbuf = txbuf;
	service->cmds[idx].rxbuf_dma = rxbuf_dma;
	service->cmds[idx].txbuf_dma = txbuf_dma;
	service->cmds[idx].rxlen = output_len;
	service->cmds[idx].txlen = payload_len;
	service->cmds[idx].free_on_ack = false;
	service->cmds[idx].done = false;
	service->cmds[idx].completion = &completion;
	init_completion(&completion);

	spin_unlock_irqrestore(&service->lock, flags);

	ret = afk_send_epic(service->ep, service->channel, tag,
			    EPIC_TYPE_COMMAND, EPIC_CAT_COMMAND, type, &cmd,
			    sizeof(cmd));
	if (ret)
		goto err_free_cmd;

	ret = wait_for_completion_timeout(&completion,
					  msecs_to_jiffies(MSEC_PER_SEC));

	if (ret <= 0) {
		spin_lock_irqsave(&service->lock, flags);
		/*
		 * Check again while we're inside the lock to make sure
		 * the command wasn't completed just after
		 * wait_for_completion_timeout returned.
		 */
		if (!service->cmds[idx].done) {
			service->cmds[idx].completion = NULL;
			service->cmds[idx].free_on_ack = true;
			spin_unlock_irqrestore(&service->lock, flags);
			return -ETIMEDOUT;
		}
		spin_unlock_irqrestore(&service->lock, flags);
	}

	ret = 0;
	if (retcode)
		*retcode = service->cmds[idx].retcode;
	if (output && output_len)
		memcpy(output, rxbuf, output_len);

err_free_cmd:
	spin_lock_irqsave(&service->lock, flags);
	bitmap_release_region(service->cmd_map, idx, 0);
err_unlock:
	spin_unlock_irqrestore(&service->lock, flags);
	dma_free_coherent(ep->dev, payload_len, txbuf, txbuf_dma);
err_free_rxbuf:
	dma_free_coherent(ep->dev, output_len, rxbuf, rxbuf_dma);
	return ret;
}

int afk_service_call(struct apple_epic_service *service, u16 group, u32 command,
		     const void *data, size_t data_len, size_t data_pad,
		     void *output, size_t output_len, size_t output_pad)
{
	struct epic_service_call *call;
	void *bfr;
	size_t bfr_len = max(data_len + data_pad, output_len + output_pad) +
			 sizeof(*call);
	int ret;
	u32 retcode;
	u32 retlen;

	bfr = kzalloc(bfr_len, GFP_KERNEL);
	if (!bfr)
		return -ENOMEM;

	call = bfr;

	memset(call, 0, sizeof(*call));
	call->group = cpu_to_le16(group);
	call->command = cpu_to_le32(command);
	call->data_len = cpu_to_le32(data_len + data_pad);
	call->magic = cpu_to_le32(EPIC_SERVICE_CALL_MAGIC);

	memcpy(bfr + sizeof(*call), data, data_len);

	ret = afk_send_command(service, EPIC_SUBTYPE_STD_SERVICE, bfr, bfr_len,
			       bfr, bfr_len, &retcode);
	if (ret)
		goto out;
	if (retcode) {
		ret = -EINVAL;
		goto out;
	}
	if (le32_to_cpu(call->magic) != EPIC_SERVICE_CALL_MAGIC ||
	    le16_to_cpu(call->group) != group ||
	    le32_to_cpu(call->command) != command) {
		ret = -EINVAL;
		goto out;
	}

	retlen = le32_to_cpu(call->data_len);
	if (output_len < retlen)
		retlen = output_len;
	if (output && output_len) {
		memset(output, 0, output_len);
		memcpy(output, bfr + sizeof(*call), retlen);
	}

out:
	kfree(bfr);
	return ret;
}
