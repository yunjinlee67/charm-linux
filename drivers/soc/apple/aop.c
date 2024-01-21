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

#define aop_afk_init(aop, ep, ops) (afk_init((aop)->dev, (aop)->rtk, (aop), ep, ops))

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
		.name = "spuapp",
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
		.name = "audio",
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
		.name = "voicetrigger",
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
