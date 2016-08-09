/*
 * Legacy System Control and Power Interface (SCPI) Message Protocol driver
 *
 * SCPI Message Protocol is used between the System Control Processor(SCP)
 * and the Application Processors(AP). The Message Handling Unit(MHU)
 * provides a mechanism for inter-processor communication between SCP's
 * Cortex M3 and AP.
 *
 * SCP offers control and management of the core/cluster power states,
 * various power domain DVFS including the core/cluster, certain system
 * clocks configuration, thermal sensors and many others.
 *
 * Copyright (C) 2016 BayLibre, SAS.
 * Author: Neil Armstrong <narmstrong@baylibre.com>
 *
 * Heavily based on arm_scpi.c from :
 * Copyright (C) 2015 ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * DISCLAIMER
 *
 * This SCPI implementation is based on a technology preview release
 * and new ARMv8 SoCs implementations should use the standard SCPI
 * implementation as defined in the ARM DUI 0922G and implemented
 * in the arm_scpi.c driver.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/bitmap.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/printk.h>
#include <linux/scpi_protocol.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/spinlock.h>

#define CMD_ID_SHIFT		0
#define CMD_ID_MASK		0x7f
#define CMD_SENDER_ID_SHIFT	8
#define CMD_SENDER_ID_MASK	0xff
#define CMD_DATA_SIZE_SHIFT	20
#define CMD_DATA_SIZE_MASK	0x1ff
#define PACK_SCPI_CMD(cmd_id, sender, tx_sz)				\
	((((cmd_id) & CMD_ID_MASK) << CMD_ID_SHIFT) |			\
	(((sender) & CMD_SENDER_ID_MASK) << CMD_SENDER_ID_SHIFT) |	\
	(((tx_sz) & CMD_DATA_SIZE_MASK) << CMD_DATA_SIZE_SHIFT))

#define CMD_SIZE(cmd)	(((cmd) >> CMD_DATA_SIZE_SHIFT) & CMD_DATA_SIZE_MASK)
#define CMD_UNIQ_MASK	(CMD_TOKEN_ID_MASK << CMD_TOKEN_ID_SHIFT | CMD_ID_MASK)
#define CMD_XTRACT_UNIQ(cmd)	((cmd) & CMD_UNIQ_MASK)

#define MAX_DVFS_DOMAINS	3
#define MAX_DVFS_OPPS		16
#define DVFS_LATENCY(hdr)	(le32_to_cpu(hdr) >> 16)
#define DVFS_OPP_COUNT(hdr)	((le32_to_cpu(hdr) >> 8) & 0xff)

#define MAX_RX_TIMEOUT          (msecs_to_jiffies(30))

enum legacy_scpi_error_codes {
	SCPI_SUCCESS = 0, /* Success */
	SCPI_ERR_PARAM = 1, /* Invalid parameter(s) */
	SCPI_ERR_ALIGN = 2, /* Invalid alignment */
	SCPI_ERR_SIZE = 3, /* Invalid size */
	SCPI_ERR_HANDLER = 4, /* Invalid handler/callback */
	SCPI_ERR_ACCESS = 5, /* Invalid access/permission denied */
	SCPI_ERR_RANGE = 6, /* Value out of range */
	SCPI_ERR_TIMEOUT = 7, /* Timeout has occurred */
	SCPI_ERR_NOMEM = 8, /* Invalid memory area or pointer */
	SCPI_ERR_PWRSTATE = 9, /* Invalid power state */
	SCPI_ERR_SUPPORT = 10, /* Not supported or disabled */
	SCPI_ERR_DEVICE = 11, /* Device error */
	SCPI_ERR_BUSY = 12, /* Device busy */
	SCPI_ERR_MAX
};

enum legacy_scpi_client_id {
	SCPI_CL_NONE,
	SCPI_CL_CLOCKS,
	SCPI_CL_DVFS,
	SCPI_CL_POWER,
	SCPI_CL_THERMAL,
	SCPI_MAX,
};

enum legacy_scpi_std_cmd {
	SCPI_CMD_INVALID		= 0x00,
	SCPI_CMD_SCPI_READY		= 0x01,
	SCPI_CMD_SCPI_CAPABILITIES	= 0x02,
	SCPI_CMD_EVENT			= 0x03,
	SCPI_CMD_SET_CSS_PWR_STATE	= 0x04,
	SCPI_CMD_GET_CSS_PWR_STATE	= 0x05,
	SCPI_CMD_CFG_PWR_STATE_STAT	= 0x06,
	SCPI_CMD_GET_PWR_STATE_STAT	= 0x07,
	SCPI_CMD_SYS_PWR_STATE		= 0x08,
	SCPI_CMD_L2_READY		= 0x09,
	SCPI_CMD_SET_AP_TIMER		= 0x0a,
	SCPI_CMD_CANCEL_AP_TIME		= 0x0b,
	SCPI_CMD_DVFS_CAPABILITIES	= 0x0c,
	SCPI_CMD_GET_DVFS_INFO		= 0x0d,
	SCPI_CMD_SET_DVFS		= 0x0e,
	SCPI_CMD_GET_DVFS		= 0x0f,
	SCPI_CMD_GET_DVFS_STAT		= 0x10,
	SCPI_CMD_SET_RTC		= 0x11,
	SCPI_CMD_GET_RTC		= 0x12,
	SCPI_CMD_CLOCK_CAPABILITIES	= 0x13,
	SCPI_CMD_SET_CLOCK_INDEX	= 0x14,
	SCPI_CMD_SET_CLOCK_VALUE	= 0x15,
	SCPI_CMD_GET_CLOCK_VALUE	= 0x16,
	SCPI_CMD_PSU_CAPABILITIES	= 0x17,
	SCPI_CMD_SET_PSU		= 0x18,
	SCPI_CMD_GET_PSU		= 0x19,
	SCPI_CMD_SENSOR_CAPABILITIES	= 0x1a,
	SCPI_CMD_SENSOR_INFO		= 0x1b,
	SCPI_CMD_SENSOR_VALUE		= 0x1c,
	SCPI_CMD_SENSOR_CFG_PERIODIC	= 0x1d,
	SCPI_CMD_SENSOR_CFG_BOUNDS	= 0x1e,
	SCPI_CMD_SENSOR_ASYNC_VALUE	= 0x1f,
	SCPI_CMD_COUNT
};

struct legacy_scpi_xfer {
	u32 cmd;
	u32 status;
	const void *tx_buf;
	void *rx_buf;
	unsigned int tx_len;
	unsigned int rx_len;
	struct completion done;
	void *vendor_msg;
};

struct legacy_scpi_chan {
	struct mbox_client cl;
	struct mbox_chan *chan;
	void __iomem *tx_payload;
	void __iomem *rx_payload;
	struct mutex xfers_lock;
	struct legacy_scpi_xfer t;
	void *vendor_data;
};

struct legacy_scpi_ops {
	int (*init)(struct device *dev, struct legacy_scpi_chan *chan);
	int (*prepare)(struct legacy_scpi_chan *chan, unsigned long arg);
};

struct legacy_scpi_drvinfo {
	int num_chans;
	struct legacy_scpi_chan *channels;
	struct scpi_dvfs_info *dvfs[MAX_DVFS_DOMAINS];
	const struct legacy_scpi_ops *ops;
};

/*
 * The SCP firmware only executes in little-endian mode, so any buffers
 * shared through SCPI should have their contents converted to little-endian
 */
struct legacy_scpi_shared_mem {
	__le32 status;
	u8 payload[0];
} __packed;

struct scp_capabilities {
	__le32 protocol_version;
	__le32 event_version;
	__le32 platform_version;
	__le32 commands[4];
} __packed;

struct clk_get_info {
	__le16 id;
	__le16 flags;
	__le32 min_rate;
	__le32 max_rate;
	u8 name[20];
} __packed;

struct clk_get_value {
	__le32 rate;
} __packed;

struct clk_set_value {
	__le32 rate;
	__le16 id;
	__le16 reserved;
} __packed;

struct dvfs_info {
	__le32 header;
	struct {
		__le32 freq;
		__le32 m_volt;
	} opps[MAX_DVFS_OPPS];
} __packed;

struct dvfs_get {
	u8 index;
} __packed;

struct dvfs_set {
	u8 domain;
	u8 index;
} __packed;

struct sensor_capabilities {
	__le16 sensors;
} __packed;

struct sensor_info {
	__le16 sensor_id;
	u8 class;
	u8 trigger_type;
	char name[20];
};

struct sensor_value {
	__le32 val;
} __packed;

static struct legacy_scpi_drvinfo *legacy_scpi_info;

static int legacy_scpi_linux_errmap[SCPI_ERR_MAX] = {
	/* better than switch case as long as return value is continuous */
	0, /* SCPI_SUCCESS */
	-EINVAL, /* SCPI_ERR_PARAM */
	-ENOEXEC, /* SCPI_ERR_ALIGN */
	-EMSGSIZE, /* SCPI_ERR_SIZE */
	-EINVAL, /* SCPI_ERR_HANDLER */
	-EACCES, /* SCPI_ERR_ACCESS */
	-ERANGE, /* SCPI_ERR_RANGE */
	-ETIMEDOUT, /* SCPI_ERR_TIMEOUT */
	-ENOMEM, /* SCPI_ERR_NOMEM */
	-EINVAL, /* SCPI_ERR_PWRSTATE */
	-EOPNOTSUPP, /* SCPI_ERR_SUPPORT */
	-EIO, /* SCPI_ERR_DEVICE */
	-EBUSY, /* SCPI_ERR_BUSY */
};

static inline int legacy_scpi_to_linux_errno(int errno)
{
	if (errno >= SCPI_SUCCESS && errno < SCPI_ERR_MAX)
		return legacy_scpi_linux_errmap[errno];
	return -EIO;
}

static void legacy_scpi_handle_remote_msg(struct mbox_client *c, void *msg)
{
	struct legacy_scpi_chan *ch =
		container_of(c, struct legacy_scpi_chan, cl);
	struct legacy_scpi_shared_mem *mem = ch->rx_payload;
	unsigned int len;

	len = ch->t.rx_len;

	ch->t.status = le32_to_cpu(mem->status);
	if (len)
		memcpy_fromio(ch->t.rx_buf, mem->payload, len);

	complete(&ch->t.done);
}

static void legacy_scpi_tx_prepare(struct mbox_client *c, void *msg)
{
	struct legacy_scpi_chan *ch =
		container_of(c, struct legacy_scpi_chan, cl);

	if (ch->t.tx_buf && ch->t.tx_len)
		memcpy_toio(ch->tx_payload, ch->t.tx_buf, ch->t.tx_len);
}

static int high_priority_cmds[] = {
	SCPI_CMD_GET_CSS_PWR_STATE,
	SCPI_CMD_CFG_PWR_STATE_STAT,
	SCPI_CMD_GET_PWR_STATE_STAT,
	SCPI_CMD_SET_DVFS,
	SCPI_CMD_GET_DVFS,
	SCPI_CMD_SET_RTC,
	SCPI_CMD_GET_RTC,
	SCPI_CMD_SET_CLOCK_INDEX,
	SCPI_CMD_SET_CLOCK_VALUE,
	SCPI_CMD_GET_CLOCK_VALUE,
	SCPI_CMD_SET_PSU,
	SCPI_CMD_GET_PSU,
	SCPI_CMD_SENSOR_CFG_PERIODIC,
	SCPI_CMD_SENSOR_CFG_BOUNDS,
};

static int legacy_scpi_get_chan(u8 cmd)
{
	int idx;

	for (idx = 0; idx < ARRAY_SIZE(high_priority_cmds); idx++)
		if (cmd == high_priority_cmds[idx])
			return 1;

	return 0;
}

/* Rockchip SoCs needs a special structure as a message */

struct rockchip_scpi_xfer {
	u32 cmd;
	int rx_size;
};

static int rockchip_init(struct device *dev, struct legacy_scpi_chan *chan)
{
	chan->vendor_data = devm_kmalloc(dev,
					 sizeof(struct rockchip_scpi_xfer),
					 GFP_KERNEL);
	if (!chan->vendor_data)
		return -ENOMEM;

	return 0;
}

static int rockchip_prepare(struct legacy_scpi_chan *chan,
				    unsigned long arg)
{
	struct legacy_scpi_xfer *msg = &chan->t;
	struct rockchip_scpi_xfer *xfer = chan->vendor_data;

	xfer->cmd = msg->cmd;
	xfer->rx_size = msg->rx_len;

	msg->vendor_msg = xfer;

	return 0;
}

static int legacy_scpi_send_message(u8 cmd, unsigned long arg,
				void *tx_buf, unsigned int tx_len,
				void *rx_buf, unsigned int rx_len)
{
	int ret;
	u8 chan;
	struct legacy_scpi_xfer *msg;
	struct legacy_scpi_chan *legacy_scpi_chan;

	chan = legacy_scpi_get_chan(cmd);
	legacy_scpi_chan = legacy_scpi_info->channels + chan;

	msg = &legacy_scpi_chan->t;

	mutex_lock(&legacy_scpi_chan->xfers_lock);

	msg->cmd = PACK_SCPI_CMD(cmd, arg, tx_len);
	msg->tx_buf = tx_buf;
	msg->tx_len = tx_len;
	msg->rx_buf = rx_buf;
	msg->rx_len = rx_len;
	init_completion(&msg->done);

	/* Call the prepare hook to eventually set the vendor_msg */
	if (legacy_scpi_info->ops &&
	    legacy_scpi_info->ops->prepare) {
		ret = legacy_scpi_info->ops->prepare(legacy_scpi_chan, arg);
		if (ret) {
			mutex_unlock(&legacy_scpi_chan->xfers_lock);
			return ret;
		}
	} else
		msg->vendor_msg = &msg->cmd;

	ret = mbox_send_message(legacy_scpi_chan->chan, msg->vendor_msg);
	if (ret < 0)
		goto out;

	if (!wait_for_completion_timeout(&msg->done, MAX_RX_TIMEOUT))
		ret = -ETIMEDOUT;
	else
		/* first status word */
		ret = msg->status;
out:
	mutex_unlock(&legacy_scpi_chan->xfers_lock);

	/* SCPI error codes > 0, translate them to Linux scale*/
	return ret > 0 ? legacy_scpi_to_linux_errno(ret) : ret;
}

static u32 legacy_scpi_get_version(void)
{
	return 1; /* v0.1 */
}

static unsigned long legacy_scpi_clk_get_val(u16 clk_id)
{
	int ret;
	struct clk_get_value clk;
	__le16 le_clk_id = cpu_to_le16(clk_id);

	ret = legacy_scpi_send_message(SCPI_CMD_GET_CLOCK_VALUE,
				SCPI_CL_CLOCKS,
				&le_clk_id, sizeof(le_clk_id),
				&clk, sizeof(clk));

	return ret ? ret : le32_to_cpu(clk.rate);
}

static int legacy_scpi_clk_set_val(u16 clk_id, unsigned long rate)
{
	int stat;
	struct clk_set_value clk = {
		.id = cpu_to_le16(clk_id),
		.rate = cpu_to_le32(rate)
	};

	return legacy_scpi_send_message(SCPI_CMD_SET_CLOCK_VALUE,
				SCPI_CL_CLOCKS,
				&clk, sizeof(clk),
				&stat, sizeof(stat));
}

static int legacy_scpi_dvfs_get_idx(u8 domain)
{
	int ret;
	struct dvfs_get dvfs;

	ret = legacy_scpi_send_message(SCPI_CMD_GET_DVFS, SCPI_CL_DVFS,
				&domain, sizeof(domain),
				&dvfs, sizeof(dvfs));

	return ret ? ret : dvfs.index;
}

static int legacy_scpi_dvfs_set_idx(u8 domain, u8 index)
{
	int stat;
	struct dvfs_set dvfs = {domain, index};

	return legacy_scpi_send_message(SCPI_CMD_SET_DVFS, SCPI_CL_DVFS,
				&dvfs, sizeof(dvfs),
				&stat, sizeof(stat));
}

static struct scpi_dvfs_info *legacy_scpi_dvfs_get_info(u8 domain)
{
	struct scpi_dvfs_info *info;
	struct scpi_opp *opp;
	struct dvfs_info buf;
	int ret, i;

	if (domain >= MAX_DVFS_DOMAINS)
		return ERR_PTR(-EINVAL);

	if (legacy_scpi_info->dvfs[domain])	/* data already populated */
		return legacy_scpi_info->dvfs[domain];

	ret = legacy_scpi_send_message(SCPI_CMD_GET_DVFS_INFO, SCPI_CL_DVFS,
				&domain, sizeof(domain),
				&buf, sizeof(buf));

	if (ret)
		return ERR_PTR(ret);

	info = kmalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return ERR_PTR(-ENOMEM);

	info->count = DVFS_OPP_COUNT(buf.header);
	info->latency = DVFS_LATENCY(buf.header) * 1000; /* uS to nS */

	info->opps = kcalloc(info->count, sizeof(*opp), GFP_KERNEL);
	if (!info->opps) {
		kfree(info);
		return ERR_PTR(-ENOMEM);
	}

	for (i = 0, opp = info->opps; i < info->count; i++, opp++) {
		opp->freq = le32_to_cpu(buf.opps[i].freq);
		opp->m_volt = le32_to_cpu(buf.opps[i].m_volt);
	}

	legacy_scpi_info->dvfs[domain] = info;
	return info;
}

static int legacy_scpi_sensor_get_capability(u16 *sensors)
{
	struct sensor_capabilities cap_buf;
	int ret;

	ret = legacy_scpi_send_message(SCPI_CMD_SENSOR_CAPABILITIES,
				SCPI_CL_THERMAL, NULL, 0, &cap_buf,
				sizeof(cap_buf));
	if (!ret)
		*sensors = le16_to_cpu(cap_buf.sensors);

	return ret;
}

static int legacy_scpi_sensor_get_info(u16 sensor_id,
				     struct scpi_sensor_info *info)
{
	__le16 id = cpu_to_le16(sensor_id);
	struct sensor_info _info;
	int ret;

	ret = legacy_scpi_send_message(SCPI_CMD_SENSOR_INFO, SCPI_CL_THERMAL,
				&id, sizeof(id),
				&_info, sizeof(_info));
	if (!ret) {
		memcpy(info, &_info, sizeof(*info));
		info->sensor_id = le16_to_cpu(_info.sensor_id);
	}

	return ret;
}

static int legacy_scpi_sensor_get_value(u16 sensor, u64 *val)
{
	__le16 id = cpu_to_le16(sensor);
	struct sensor_value buf;
	int ret;

	ret = legacy_scpi_send_message(SCPI_CMD_SENSOR_VALUE, SCPI_CL_THERMAL,
				&id, sizeof(id),
				&buf, sizeof(buf));
	if (!ret)
		*val = (u64)le32_to_cpu(buf.val);

	return ret;
}

static struct scpi_ops legacy_scpi_ops = {
	.get_version = legacy_scpi_get_version,
	.clk_get_val = legacy_scpi_clk_get_val,
	.clk_set_val = legacy_scpi_clk_set_val,
	.dvfs_get_idx = legacy_scpi_dvfs_get_idx,
	.dvfs_set_idx = legacy_scpi_dvfs_set_idx,
	.dvfs_get_info = legacy_scpi_dvfs_get_info,
	.sensor_get_capability = legacy_scpi_sensor_get_capability,
	.sensor_get_info = legacy_scpi_sensor_get_info,
	.sensor_get_value = legacy_scpi_sensor_get_value,
	.vendor_send_message = legacy_scpi_send_message,
};

static void legacy_scpi_free_channels(struct device *dev,
				struct legacy_scpi_chan *pchan, int count)
{
	int i;

	for (i = 0; i < count && pchan->chan; i++, pchan++)
		mbox_free_channel(pchan->chan);
}

static int legacy_scpi_remove(struct platform_device *pdev)
{
	int i;
	struct device *dev = &pdev->dev;
	struct legacy_scpi_drvinfo *info = platform_get_drvdata(pdev);

	/* stop exporting SCPI ops */
	legacy_scpi_info = NULL;

	of_platform_depopulate(dev);
	legacy_scpi_free_channels(dev, info->channels, info->num_chans);
	platform_set_drvdata(pdev, NULL);

	for (i = 0; i < MAX_DVFS_DOMAINS && info->dvfs[i]; i++) {
		kfree(info->dvfs[i]->opps);
		kfree(info->dvfs[i]);
	}

	return 0;
}

static const struct legacy_scpi_ops scpi_rockchip_ops = {
	.init = rockchip_init,
	.prepare = rockchip_prepare,
};

static const struct of_device_id legacy_scpi_of_match[] = {
	{.compatible = "amlogic,meson-gxbb-scpi"},
	{.compatible = "rockchip,rk3368-scpi", .data = &scpi_rockchip_ops},
	{.compatible = "rockchip,rk3399-scpi", .data = &scpi_rockchip_ops},
	{ /* sentinel */ },
};

static int legacy_scpi_probe(struct platform_device *pdev)
{
	int count, idx, ret;
	struct resource res;
	struct legacy_scpi_chan *legacy_scpi_chan;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	const struct of_device_id *match;
	const struct legacy_scpi_ops *ops;

	match = of_match_device(legacy_scpi_of_match, &pdev->dev);
	if (!match)
		return -EINVAL;

	ops = match->data;

	legacy_scpi_info = devm_kzalloc(dev, sizeof(*legacy_scpi_info),
					GFP_KERNEL);
	if (!legacy_scpi_info)
		return -ENOMEM;

	count = of_count_phandle_with_args(np, "mboxes", "#mbox-cells");
	if (count < 0) {
		dev_err(dev, "no mboxes property in '%s'\n", np->full_name);
		return -ENODEV;
	}

	legacy_scpi_chan = devm_kcalloc(dev, count,
				sizeof(*legacy_scpi_chan), GFP_KERNEL);
	if (!legacy_scpi_chan)
		return -ENOMEM;

	for (idx = 0; idx < count; idx++) {
		resource_size_t size;
		struct legacy_scpi_chan *pchan = legacy_scpi_chan + idx;
		struct mbox_client *cl = &pchan->cl;
		struct device_node *shmem = of_parse_phandle(np, "shmem", idx);

		if (of_address_to_resource(shmem, 0, &res)) {
			dev_err(dev, "failed to get SCPI payload mem resource\n");
			ret = -EINVAL;
			goto err;
		}

		size = resource_size(&res);

		pchan->rx_payload = devm_ioremap(dev, res.start, size);
		if (!pchan->rx_payload) {
			dev_err(dev, "failed to ioremap SCPI payload\n");
			ret = -EADDRNOTAVAIL;
			goto err;
		}
		pchan->tx_payload = pchan->rx_payload + (size >> 1);

		if (ops && ops->init) {
			ret = ops->init(dev, pchan);
			if (ret)
				goto err;
		}

		cl->dev = dev;
		cl->rx_callback = legacy_scpi_handle_remote_msg;
		cl->tx_prepare = legacy_scpi_tx_prepare;
		cl->tx_block = true;
		cl->tx_tout = 20;
		cl->knows_txdone = false; /* controller can't ack */

		mutex_init(&pchan->xfers_lock);

		pchan->chan = mbox_request_channel(cl, idx);
		if (!IS_ERR(pchan->chan))
			continue;
		else
			ret = PTR_ERR(pchan->chan);
err:
		legacy_scpi_free_channels(dev, legacy_scpi_chan, idx);
		legacy_scpi_info = NULL;
		return ret;
	}

	legacy_scpi_info->ops = ops;
	legacy_scpi_info->channels = legacy_scpi_chan;
	legacy_scpi_info->num_chans = count;
	platform_set_drvdata(pdev, legacy_scpi_info);

	ret = devm_scpi_ops_register(dev, &legacy_scpi_ops);
	if (ret)
		return ret;

	return of_platform_populate(dev->of_node, NULL, NULL, dev);
}

MODULE_DEVICE_TABLE(of, legacy_scpi_of_match);

static struct platform_driver legacy_scpi_driver = {
	.driver = {
		.name = "legacy-scpi",
		.of_match_table = legacy_scpi_of_match,
	},
	.probe = legacy_scpi_probe,
	.remove = legacy_scpi_remove,
};
module_platform_driver(legacy_scpi_driver);

MODULE_AUTHOR("Sudeep Holla <sudeep.holla@arm.com>");
MODULE_AUTHOR("Neil Armstrong <narmstrong@baylibre.com>");
MODULE_DESCRIPTION("ARM Legacy SCPI mailbox protocol driver");
MODULE_LICENSE("GPL v2");
