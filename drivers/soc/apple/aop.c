// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple Always-On Processor (AOP) driver
 *
 * Copyright (C) 2024 The Asahi Linux Contributors
 */

#include <linux/apple-mailbox.h>
#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/iommu.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/soc/apple/rtkit.h>

#include "afk.h"

#define APPLE_AOP_COPROC_CPU_CONTROL	 0x44
#define APPLE_AOP_COPROC_CPU_CONTROL_RUN BIT(4)

#define AOP_BOOT_TIMEOUT msecs_to_jiffies(1000)

enum {
	SPUAPP_ENDPOINT       = 0x20,
	ACCEL_ENDPOINT        = 0x21,
	GYRO_ENDPOINT         = 0x22,
	ALS_ENDPOINT          = 0x24,
	WAKEHINT_ENDPOINT     = 0x25,
	UNK26_ENDPOINT        = 0x26,
	AUDIO_ENDPOINT        = 0x27,
	VOICETRIGGER_ENDPOINT = 0x28,
};

struct apple_aop {
	struct device *dev;
	struct apple_rtkit *rtk;

	void __iomem *asc;
	void __iomem *nub;

	struct apple_dcp_afkep *spuappep; // 0x20
	struct apple_dcp_afkep *accelep; // 0x21
	struct apple_dcp_afkep *gyroep; // 0x22
	struct apple_dcp_afkep *alsep; // 0x24
	struct apple_dcp_afkep *wakehintep; // 0x25
	struct apple_dcp_afkep *unk26ep; // 0x26
	struct apple_dcp_afkep *audioep; // 0x27
	struct apple_dcp_afkep *voicetriggerep; // 0x28
};

enum {
	AOP_REPORT_HELLO      = 0xc0,
};

struct aop_epic_service_init {
	char name[16];
	u32 unk0;
	u32 unk1;
	u32 retcode;
	u32 unk3;
	u32 channel;
	u32 unk5;
	u32 unk6;
};
static_assert(sizeof(struct aop_epic_service_init) == 0x2c);

static void apple_aop_recv_handle_init(struct apple_dcp_afkep *ep, u16 subtype, u32 channel,
				 u8 *payload, size_t payload_size)
{
	struct apple_aop *aop = afkep_to_device(ep);
	const struct apple_epic_service_ops *ops;
	struct aop_epic_service_init *prop;
	u32 ch_idx;

	WARN_ON(subtype != EPIC_SUBTYPE_STD_SERVICE);
	WARN_ON(payload_size != sizeof(*prop));

	if (payload_size < sizeof(*prop)) {
		dev_err(ep->dev, "AFK[ep:%02x]: payload too small: %lx\n",
			ep->endpoint, payload_size);
		return;
	}

	if (ep->num_channels >= AFK_MAX_CHANNEL) {
		dev_err(ep->dev, "AFK[ep:%02x]: too many enabled services!\n",
			ep->endpoint);
		return;
	}

	prop = (struct aop_epic_service_init *)payload;
	/* aop doesn't use the passed channel var; we parse it from the struct */
	WARN_ON(afk_epic_find_service(ep, prop->channel));

	ops = afk_match_service(ep, prop->name);
	if (!ops) {
		dev_err(ep->dev,
			"AFK[ep:%02x]: unable to match service %s on channel %d\n",
			ep->endpoint, prop->name, prop->channel);
		return;
	}

	ch_idx = ep->num_channels++;
	spin_lock_init(&ep->services[ch_idx].lock);
	ep->services[ch_idx].enabled = true;
	ep->services[ch_idx].ops = ops;
	ep->services[ch_idx].ep = ep;
	ep->services[ch_idx].channel = prop->channel;
	ep->services[ch_idx].cmd_tag = 0;
	dev_info(ep->dev, "AFK[ep:%02x]: new service %s on channel 0x%x\n",
		 ep->endpoint, prop->name, prop->channel);
}

static const struct apple_afk_epic_ops apple_aop_epic_ops = {
	.recv_handle_init = apple_aop_recv_handle_init,
};

#define aop_afk_init(aop, ep, ops) (afk_init((aop)->dev, (aop)->rtk, (aop), ep, ops, &apple_aop_epic_ops))

static int aop_epic_hello_report(struct apple_epic_service *service,
			 const void *data, size_t data_size)
{
	struct apple_aop *aop = afk_to_device(service);
	// parent class
	// aopep->chan = rep.channel;
	afkep_dbg(service, "Hello! chan:0x%x\n", service->channel);
	afkep_err(service, "[ch:%u]: report len:%zu\n", service->channel, data_size);
	return 0;
}

static int aop_epic_handle_report(struct apple_epic_service *service, enum epic_subtype type,
			 const void *data, size_t data_size)
{
	switch (type) {
	case AOP_REPORT_HELLO:
		return aop_epic_hello_report(service, data, data_size);
	default:
		afkep_err(service, "unknown report type: %x", type);
		return -EINVAL;
	}
}

/* spuapp endpoint */
static void spuapp_service_init(struct apple_epic_service *service, const char *name,
			const char *class, s64 unit)
{
}

static const struct apple_epic_service_ops spuappep_ops[] = {
	{
		.name = "SPUApp",
		.init = spuapp_service_init,
		.report = aop_epic_handle_report,
	},
	{
		.name = "i2c",
		.init = spuapp_service_init,
		.report = aop_epic_handle_report,
	},
	{}
};

static int spuappep_init(struct apple_aop *aop)
{
	aop->spuappep = aop_afk_init(aop, SPUAPP_ENDPOINT, spuappep_ops);
	afk_start(aop->spuappep);
	return 0;
}

/* accel endpoint */
static void accel_service_init(struct apple_epic_service *service, const char *name,
			const char *class, s64 unit)
{
}

static const struct apple_epic_service_ops accelep_ops[] = {
	{
		.name = "accel",
		.init = accel_service_init,
		.report = aop_epic_handle_report,
	},
	{}
};

static int accelep_init(struct apple_aop *aop)
{
	aop->accelep = aop_afk_init(aop, ACCEL_ENDPOINT, accelep_ops);
	afk_start(aop->accelep);
	return 0;
}

/* gyro endpoint */
static void gyro_service_init(struct apple_epic_service *service, const char *name,
			const char *class, s64 unit)
{
}

static const struct apple_epic_service_ops gyroep_ops[] = {
	{
		.name = "gyro",
		.init = gyro_service_init,
		.report = aop_epic_handle_report,
	},
	{}
};

static int gyroep_init(struct apple_aop *aop)
{
	aop->gyroep = aop_afk_init(aop, GYRO_ENDPOINT, gyroep_ops);
	aop->gyroep->dummy = true; // do not start gyro rx/tx
	afk_start(aop->gyroep);
	return 0;
}

/* als endpoint */
static void als_service_init(struct apple_epic_service *service, const char *name,
			const char *class, s64 unit)
{
}

static const struct apple_epic_service_ops alsep_ops[] = {
	{
		.name = "als",
		.init = als_service_init,
		.report = aop_epic_handle_report,
	},
	{}
};

static int alsep_init(struct apple_aop *aop)
{
	aop->alsep = aop_afk_init(aop, ALS_ENDPOINT, alsep_ops);
	afk_start(aop->alsep);
	return 0;
}

/* wakehint endpoint */
static void wakehint_service_init(struct apple_epic_service *service, const char *name,
			const char *class, s64 unit)
{
}

static const struct apple_epic_service_ops wakehintep_ops[] = {
	{
		.name = "wakehint",
		.init = wakehint_service_init,
		.report = aop_epic_handle_report,
	},
	{}
};

static int wakehintep_init(struct apple_aop *aop)
{
	aop->wakehintep = aop_afk_init(aop, WAKEHINT_ENDPOINT, wakehintep_ops);
	afk_start(aop->wakehintep);
	return 0;
}

/* unk26 endpoint */
static void unk26_service_init(struct apple_epic_service *service, const char *name,
			const char *class, s64 unit)
{
}

static const struct apple_epic_service_ops unk26ep_ops[] = {
	{
		.name = "unk26",
		.init = unk26_service_init,
		.report = aop_epic_handle_report,
	},
	{}
};

static int unk26ep_init(struct apple_aop *aop)
{
	aop->unk26ep = aop_afk_init(aop, UNK26_ENDPOINT, unk26ep_ops);
	afk_start(aop->unk26ep);
	return 0;
}

/* audio endpoint */
static void audio_service_init(struct apple_epic_service *service, const char *name,
			const char *class, s64 unit)
{
}

static const struct apple_epic_service_ops audioep_ops[] = {
	{
		.name = "aop-audio",
		.init = audio_service_init,
		.report = aop_epic_handle_report,
	},
	{}
};

static int audioep_init(struct apple_aop *aop)
{
	aop->audioep = aop_afk_init(aop, AUDIO_ENDPOINT, audioep_ops);
	afk_start(aop->audioep);
	return 0;
}

/* voicetrigger endpoint */
static void voicetrigger_service_init(struct apple_epic_service *service, const char *name,
			const char *class, s64 unit)
{
}

static const struct apple_epic_service_ops voicetriggerep_ops[] = {
	{
		.name = "aop-voicetrigger",
		.init = voicetrigger_service_init,
		.report = aop_epic_handle_report,
	},
	{}
};

static int voicetriggerep_init(struct apple_aop *aop)
{
	aop->voicetriggerep = aop_afk_init(aop, VOICETRIGGER_ENDPOINT, voicetriggerep_ops);
	afk_start(aop->voicetriggerep);
	return 0;
}

static int apple_aop_start(struct apple_aop *aop)
{
	int ret;

	// start all the endpoints. doesn't mean we use all of them, but all the eps have to be hello/acked to kick up any one of them

	ret = spuappep_init(aop);
	if (ret)
		dev_warn(aop->dev, "Failed to start spuapp endpoint: %d", ret);

	ret = accelep_init(aop);
	if (ret)
		dev_warn(aop->dev, "Failed to start accel endpoint: %d", ret);

	ret = gyroep_init(aop);
	if (ret)
		dev_warn(aop->dev, "Failed to start gyro endpoint: %d", ret);

	ret = alsep_init(aop);
	if (ret)
		dev_warn(aop->dev, "Failed to start als endpoint: %d", ret);

	ret = wakehintep_init(aop);
	if (ret)
		dev_warn(aop->dev, "Failed to start wakehint endpoint: %d", ret);

	ret = unk26ep_init(aop);
	if (ret)
		dev_warn(aop->dev, "Failed to start unk26 endpoint: %d", ret);

	ret = audioep_init(aop);
	if (ret)
		dev_warn(aop->dev, "Failed to start audio endpoint: %d", ret);

	ret = voicetriggerep_init(aop);
	if (ret)
		dev_warn(aop->dev, "Failed to start voicetrigger endpoint: %d", ret);

	return ret;
}

static void apple_aop_recv_msg(void *cookie, u8 endpoint, u64 message)
{
	struct apple_aop *aop = cookie;

	switch (endpoint) {
	case SPUAPP_ENDPOINT:
		afk_receive_message(aop->spuappep, message);
		return;
	case ACCEL_ENDPOINT:
		afk_receive_message(aop->accelep, message);
		return;
	case GYRO_ENDPOINT:
		afk_receive_message(aop->gyroep, message);
		return;
	case ALS_ENDPOINT:
		afk_receive_message(aop->alsep, message);
		return;
	case WAKEHINT_ENDPOINT:
		afk_receive_message(aop->wakehintep, message);
		return;
	case UNK26_ENDPOINT:
		afk_receive_message(aop->unk26ep, message);
		return;
	case AUDIO_ENDPOINT:
		afk_receive_message(aop->audioep, message);
		return;
	case VOICETRIGGER_ENDPOINT:
		afk_receive_message(aop->voicetriggerep, message);
		return;
	default:
		WARN(endpoint, "unknown AOP endpoint %hhu", endpoint);
	}
}

static void apple_aop_rtk_crashed(void *cookie)
{
	struct apple_aop *aop = cookie;

	dev_err(aop->dev, "aop has crashed");
}

static int apple_aop_rtk_shmem_setup(void *cookie, struct apple_rtkit_shmem *bfr)
{
	struct apple_aop *aop = cookie;

	if (bfr->iova) {
		struct iommu_domain *domain =
			iommu_get_domain_for_dev(aop->dev);
		phys_addr_t phy_addr;

		if (!domain)
			return -ENOMEM;

		// TODO: get map from device-tree
		phy_addr = iommu_iova_to_phys(domain, bfr->iova);
		if (!phy_addr)
			return -ENOMEM;

		// TODO: verify phy_addr, cache attribute
		bfr->buffer = memremap(phy_addr, bfr->size, MEMREMAP_WB);
		if (!bfr->buffer)
			return -ENOMEM;

		bfr->is_mapped = true;
		dev_info(aop->dev,
			 "shmem_setup: iova: %lx -> pa: %lx -> iomem: %lx",
			 (uintptr_t)bfr->iova, (uintptr_t)phy_addr,
			 (uintptr_t)bfr->buffer);
	} else {
		bfr->buffer = dma_alloc_coherent(aop->dev, bfr->size,
						 &bfr->iova, GFP_KERNEL);
		if (!bfr->buffer)
			return -ENOMEM;

		dev_info(aop->dev, "shmem_setup: iova: %lx, buffer: %lx",
			 (uintptr_t)bfr->iova, (uintptr_t)bfr->buffer);
	}

	return 0;
}

static void apple_aop_rtk_shmem_destroy(void *cookie, struct apple_rtkit_shmem *bfr)
{
	struct apple_aop *aop = cookie;

	if (bfr->is_mapped)
		memunmap(bfr->buffer);
	else
		dma_free_coherent(aop->dev, bfr->size, bfr->buffer, bfr->iova);
}

static struct apple_rtkit_ops apple_aop_rtkit_ops = {
	.crashed = apple_aop_rtk_crashed,
	.shmem_setup = apple_aop_rtk_shmem_setup,
	.shmem_destroy = apple_aop_rtk_shmem_destroy,
	.recv_message = apple_aop_recv_msg,
};

#define APPLE_AOP_NUB_OFFSET 0x22c  // 0x224 in 12.3
#define APPLE_AOP_NUB_SIZE   0x230  // 0x228 in 12.3

static const unsigned char bootargs_bin[] = {
  0x47, 0x4b, 0x54, 0x53, 0x08, 0x00, 0x00, 0x00, 0xf4, 0x5f, 0x28, 0xf6,
  0xfd, 0x43, 0x09, 0x00, 0x63, 0x32, 0x69, 0x72, 0x04, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x70, 0x30, 0x43, 0x45, 0x08, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x70, 0x30, 0x44, 0x45,
  0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x6c, 0x61, 0x43, 0x6e, 0x01, 0x00, 0x00, 0x00, 0x00, 0x48, 0x6c, 0x63,
  0x61, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x45, 0x70, 0x61,
  0x6e, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x43, 0x52, 0x41,
  0x70, 0x01, 0x00, 0x00, 0x00, 0x00, 0x63, 0x32, 0x69, 0x73, 0x04, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x70, 0x75, 0x74, 0x6c, 0x04, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x74, 0x50, 0x4f, 0x41, 0x04, 0x00,
  0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x67, 0x69, 0x6c, 0x61, 0x04, 0x00,
  0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x67, 0x62, 0x64, 0x61, 0x04, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x44, 0x49, 0x4c, 0x53, 0x08, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x53, 0x53,
  0x53, 0x43, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x71, 0x46, 0x38, 0x76, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x4c, 0x52, 0x53, 0x44, 0x01, 0x00, 0x00, 0x00, 0x00, 0x53,
  0x56, 0x53, 0x44, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x4c, 0x43, 0x53, 0x44, 0x08, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5a, 0x53, 0x54, 0x52, 0x04,
  0x00, 0x00, 0x00, 0x00, 0xb0, 0x10, 0x00, 0x42, 0x50, 0x54, 0x50, 0x08,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x54,
  0x4e, 0x47, 0x49, 0x01, 0x00, 0x00, 0x00, 0x00, 0x6f, 0x65, 0x4e, 0x53,
  0x01, 0x00, 0x00, 0x00, 0x00, 0x42, 0x56, 0x54, 0x50, 0x08, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x73, 0x50, 0x31,
  0x54, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x42, 0x74, 0x70, 0x47, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x53, 0x74, 0x70, 0x47, 0x08, 0x00, 0x00,
  0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x42, 0x6c, 0x70,
  0x50, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x53, 0x50, 0x54, 0x50, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x42, 0x50, 0x78, 0x47, 0x08, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x53, 0x50, 0x78,
  0x47, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x53, 0x5a, 0x53, 0x44, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x4c, 0x5a, 0x53, 0x44, 0x08, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x53, 0x4e, 0x55,
  0x54, 0x08, 0x00, 0x00, 0x00, 0xf8, 0x05, 0x0b, 0x01, 0x00, 0x00, 0x00,
  0x00, 0x5a, 0x4e, 0x55, 0x54, 0x04, 0x00, 0x00, 0x00, 0xe8, 0x01, 0x00,
  0x00, 0x4f, 0x54, 0x54, 0x52, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x52, 0x63, 0x4d, 0x04, 0x00, 0x00,
  0x00, 0x00, 0x40, 0x00, 0x00, 0x5f, 0x43, 0x4f, 0x53, 0x04, 0x00, 0x00,
  0x00, 0x03, 0x81, 0x00, 0x00, 0x52, 0x43, 0x4f, 0x53, 0x04, 0x00, 0x00,
  0x00, 0x11, 0x00, 0x00, 0x00, 0x64, 0x41, 0x70, 0x43, 0x08, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x4a, 0x02, 0x00, 0x00, 0x00, 0x64, 0x41, 0x72,
  0x57, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x4a, 0x02, 0x00, 0x00,
  0x00, 0x66, 0x56, 0x45, 0x44, 0x01, 0x00, 0x00, 0x00, 0x00, 0x41, 0x42,
  0x4f, 0x49, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x5a, 0x53, 0x4f, 0x49, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x47, 0x4e, 0x52, 0x50, 0x20, 0x00, 0x00, 0x00, 0x75, 0x8c,
  0xc4, 0xec, 0x1c, 0xdd, 0x37, 0x70, 0xe9, 0xbd, 0xf3, 0x92, 0x52, 0x00,
  0xa7, 0x17, 0x79, 0x26, 0x36, 0x43, 0xe2, 0x21, 0x78, 0x6a, 0x77, 0x1a,
  0xf1, 0xd6, 0x6c, 0x63, 0x85, 0xfd, 0x44, 0x49, 0x43, 0x45, 0x08, 0x00,
  0x00, 0x00, 0x1e, 0x00, 0xd2, 0x0e, 0xe1, 0x65, 0x02, 0x00, 0x43, 0x4e,
  0x4f, 0x4e, 0x08, 0x00, 0x00, 0x00, 0xca, 0x0e, 0x9d, 0x84, 0x08, 0x30,
  0x83, 0x45, 0x4d, 0x54, 0x54, 0x54, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00
};
// unsigned int bootargs_bin_len = 684;

static int apple_aop_bootargs_read(struct apple_aop *aop)
{
	u32 args_off = readl_relaxed(aop->nub + APPLE_AOP_NUB_OFFSET);
	u32 args_size = readl_relaxed(aop->nub + APPLE_AOP_NUB_SIZE);
	dev_info(aop->dev, "bootargs: offset: 0x%x size: 0x%x\n", args_off, args_size);

	memcpy_toio(aop->nub + args_off, bootargs_bin, 684);

#if 0
	// TODO readl bulk then access
	u64 off = 0;
	while (off < args_size) {
		u32 key = readl(aop->nub + args_off + off);
		u32 size = readl(aop->nub + args_off + off + sizeof(u32));
		off += sizeof(u32) * 2;
		dev_info(aop->dev, "bootargs: key: %c%c%c%c size: 0x%x\n",
			key & 255, (key >> 8) & 255, (key >> 16) & 255, (key >> 24) & 255,
			size);
		off += size;
	}
#endif

	return 0;
}

static int apple_aop_bootargs_update(struct apple_aop *aop)
{
	apple_aop_bootargs_read(aop);
	return 0;
}

static int apple_aop_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct apple_aop *aop;
	int err;

	aop = devm_kzalloc(dev, sizeof(*aop), GFP_KERNEL);
	if (!aop)
		return -ENOMEM;

	aop->dev = dev;
	//aop->hw = of_device_get_match_data(dev);
	platform_set_drvdata(pdev, aop);
	dev_set_drvdata(dev, aop);

	if (dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64))) // TODO dunno
		return -ENXIO;

	aop->asc = devm_platform_ioremap_resource_byname(pdev, "asc");
	if (IS_ERR(aop->asc))
		return PTR_ERR(aop->asc);

	aop->nub = devm_platform_ioremap_resource_byname(pdev, "nub");
	if (IS_ERR(aop->nub))
		return PTR_ERR(aop->nub);

	apple_aop_bootargs_update(aop);

	aop->rtk = devm_apple_rtkit_init(dev, aop, "mbox", 0, &apple_aop_rtkit_ops);
	if (IS_ERR(aop->rtk))
		return dev_err_probe(dev, PTR_ERR(aop->rtk),
				     "Failed to intialize RTKit");

	u32 cpu_ctrl = readl_relaxed(aop->asc + APPLE_AOP_COPROC_CPU_CONTROL);
	writel_relaxed(cpu_ctrl | APPLE_AOP_COPROC_CPU_CONTROL_RUN,
		       aop->asc + APPLE_AOP_COPROC_CPU_CONTROL);

	err = apple_rtkit_wake(aop->rtk);
	if (err)
		return dev_err_probe(dev, err, "Failed to boot RTKit: %d", err);

	apple_aop_start(aop);

	dev_info(dev, "apple-aop probe!\n");

	return 0;
}

static int apple_aop_remove(struct platform_device *pdev)
{
	struct apple_aop *aop = platform_get_drvdata(pdev);
	(void)aop;

	return 0;
}

static __maybe_unused int apple_aop_runtime_suspend(struct device *dev)
{
	return 0;
}

static __maybe_unused int apple_aop_runtime_resume(struct device *dev)
{
	return 0;
}

static __maybe_unused int apple_aop_suspend(struct device *dev)
{
	return 0;
}

static __maybe_unused int apple_aop_resume(struct device *dev)
{
	return 0;
}

static const struct dev_pm_ops apple_aop_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(apple_aop_suspend, apple_aop_resume)
	RUNTIME_PM_OPS(apple_aop_runtime_suspend, apple_aop_runtime_resume, NULL)
};

static const struct of_device_id apple_aop_of_match[] = {
	{ .compatible = "apple,t8103-aop", .data = NULL,  },
	{}
};
MODULE_DEVICE_TABLE(of, apple_aop_of_match);

static struct platform_driver apple_aop_driver = {
	.driver	= {
		.name = "apple-aop",
		.of_match_table	= apple_aop_of_match,
		.pm = pm_sleep_ptr(&apple_aop_pm_ops),
	},
	.probe		= apple_aop_probe,
	.remove		= apple_aop_remove,
};
module_platform_driver(apple_aop_driver);

MODULE_AUTHOR("Eileen Yoon <eiln@gmx.com>");
MODULE_DESCRIPTION("Apple Always-On Processor driver");
MODULE_LICENSE("Dual MIT/GPL");
