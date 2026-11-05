// SPDX-License-Identifier: GPL-2.0-only
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>

struct pwh1_refresh {
	void __iomem *lcd;
	struct delayed_work work;
};

static void pwh1_refresh_work(struct work_struct *work)
{
	struct pwh1_refresh *refresh = container_of(to_delayed_work(work),
						  struct pwh1_refresh, work);
	/* Firmware uses a one-shot LCDIF transfer; simplefb alone never sets RUN. */
	if (!(readl(refresh->lcd) & 1))
		writel(1, refresh->lcd + 4);
	schedule_delayed_work(&refresh->work, msecs_to_jiffies(33));
}

static int pwh1_refresh_probe(struct platform_device *pdev)
{
	struct pwh1_refresh *refresh;
	struct clk *clk;

	clk = devm_clk_get_enabled(&pdev->dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(clk), "LCD clock unavailable\n");

	refresh = devm_kzalloc(&pdev->dev, sizeof(*refresh), GFP_KERNEL);
	if (!refresh)
		return -ENOMEM;
	refresh->lcd = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(refresh->lcd))
		return PTR_ERR(refresh->lcd);
	INIT_DELAYED_WORK(&refresh->work, pwh1_refresh_work);
	platform_set_drvdata(pdev, refresh);
	schedule_delayed_work(&refresh->work, 0);
	return 0;
}

static int pwh1_refresh_remove(struct platform_device *pdev)
{
	struct pwh1_refresh *refresh = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&refresh->work);
	return 0;
}

static void pwh1_refresh_shutdown(struct platform_device *pdev)
{
	pwh1_refresh_remove(pdev);
}

static const struct of_device_id pwh1_refresh_ids[] = {
	{ .compatible = "sharp,pwh1-lcd-refresh" },
	{ }
};
MODULE_DEVICE_TABLE(of, pwh1_refresh_ids);
static struct platform_driver pwh1_refresh_driver = {
	.probe = pwh1_refresh_probe,
	.remove = pwh1_refresh_remove,
	.shutdown = pwh1_refresh_shutdown,
	.driver = {
		.name = "pwh1-lcd-refresh",
		.of_match_table = pwh1_refresh_ids,
	},
};
module_platform_driver(pwh1_refresh_driver);
MODULE_LICENSE("GPL");
