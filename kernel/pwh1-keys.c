// SPDX-License-Identifier: GPL-2.0-only
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>

/* shortcut: V690_H1 M4 state only; replace with input events after key calibration. */
static ssize_t raw_keys_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	void __iomem *state = dev_get_drvdata(dev);
	u32 keys = readl(state + 8);

	return sysfs_emit(buf, "%02x %02x %02x (state %u)\n",
			 keys & 0xff, (keys >> 8) & 0xff, (keys >> 16) & 0xff,
			 readb(state));
}
static DEVICE_ATTR_RO(raw_keys);

static struct attribute *pwh1_keys_attrs[] = {
	&dev_attr_raw_keys.attr,
	NULL,
};
ATTRIBUTE_GROUPS(pwh1_keys);

static int pwh1_keys_probe(struct platform_device *pdev)
{
	void __iomem *code, *state;

	code = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(code))
		return PTR_ERR(code);
	/* V690_H1 key task at M4 address 1ffd76b4: push; ldr.w r5. */
	if (readl(code) != 0xf8dfb57f || readl(code + 4) != 0x22005738)
		return dev_err_probe(&pdev->dev, -ENODEV, "M4 key firmware mismatch\n");

	state = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(state))
		return PTR_ERR(state);
	platform_set_drvdata(pdev, state);
	return 0;
}

static const struct of_device_id pwh1_keys_ids[] = {
	{ .compatible = "sharp,pwh1-v690-key-state" },
	{ }
};
MODULE_DEVICE_TABLE(of, pwh1_keys_ids);
static struct platform_driver pwh1_keys_driver = {
	.probe = pwh1_keys_probe,
	.driver = {
		.name = "pwh1-keys",
		.of_match_table = pwh1_keys_ids,
		.dev_groups = pwh1_keys_groups,
	},
};
module_platform_driver(pwh1_keys_driver);
MODULE_LICENSE("GPL");
