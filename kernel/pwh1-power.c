// SPDX-License-Identifier: GPL-2.0-only
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/slab.h>

struct pwh1_power {
	void __iomem *mux;
	void __iomem *clear;
};

static int pwh1_power_off(struct sys_off_data *data)
{
	struct pwh1_power *power = data->cb_data;

	pr_emerg("PWH1: power off in 3 seconds\n");
	mdelay(3000);
	/* V690_H1 M4 stop routine: select PTA22 GPIO output, then drive low. */
	writel(0x00020100, power->mux);
	writel(BIT(22), power->clear);
	mdelay(1000);
	pr_emerg("PWH1: power off returned; use physical reset\n");
	return NOTIFY_DONE;
}

static int pwh1_power_probe(struct platform_device *pdev)
{
	static const u16 signature[] = {
		0xb580, 0x492c, 0x2016, 0xf007, 0xfdf4,
		0x2100, 0x2016, 0xf007, 0xfe46, 0xe7fe,
	};
	struct device *dev = &pdev->dev;
	struct pwh1_power *power;
	void __iomem *code;
	unsigned int i;

	code = devm_platform_ioremap_resource(pdev, 2);
	if (IS_ERR(code))
		return PTR_ERR(code);
	for (i = 0; i < ARRAY_SIZE(signature); i++)
		if (readw(code + i * sizeof(u16)) != signature[i])
			return dev_err_probe(dev, -ENODEV, "Unsupported M4 stop routine\n");

	power = devm_kzalloc(dev, sizeof(*power), GFP_KERNEL);
	if (!power)
		return -ENOMEM;
	power->mux = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(power->mux))
		return PTR_ERR(power->mux);
	power->clear = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(power->clear))
		return PTR_ERR(power->clear);

	return devm_register_sys_off_handler(dev, SYS_OFF_MODE_POWER_OFF,
					    SYS_OFF_PRIO_DEFAULT,
					    pwh1_power_off, power);
}

static const struct of_device_id pwh1_power_ids[] = {
	{ .compatible = "sharp,pwh1-v690-poweroff" },
	{ }
};

static struct platform_driver pwh1_power_driver = {
	.probe = pwh1_power_probe,
	.driver = {
		.name = "pwh1-poweroff",
		.of_match_table = pwh1_power_ids,
	},
};
builtin_platform_driver(pwh1_power_driver);
