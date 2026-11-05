// SPDX-License-Identifier: GPL-2.0-only
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define LCD_CTRL1 0x10
#define LCD_CTRL2 0x20
#define LCD_AS_CTRL 0x210
#define REG_SET 4
#define REG_CLR 8
#define RECOVERY_ON_UNDERFLOW BIT(24)
#define OUTSTANDING_REQS_MASK GENMASK(23, 21)
#define OUTSTANDING_REQS_16 (4 << 21)

static int pwh1_refresh_probe(struct platform_device *pdev)
{
	void __iomem *lcd;
	struct clk *clk;

	clk = devm_clk_get_enabled(&pdev->dev, "lcdif");
	if (IS_ERR(clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(clk), "LCD clock unavailable\n");
	clk = devm_clk_get_enabled(&pdev->dev, "dsi");
	if (IS_ERR(clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(clk), "DSI clock unavailable\n");

	lcd = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(lcd))
		return PTR_ERR(lcd);

	/* Match mxsfb: recover pixel alignment after a FIFO underrun. */
	writel(RECOVERY_ON_UNDERFLOW, lcd + LCD_CTRL1 + REG_SET);
	writel(OUTSTANDING_REQS_MASK, lcd + LCD_CTRL2 + REG_CLR);
	writel(OUTSTANDING_REQS_16, lcd + LCD_CTRL2 + REG_SET);
	/* simplefb owns the primary plane, not the firmware overlay DMA. */
	writel(1, lcd + LCD_AS_CTRL + REG_CLR);
	if (!(readl(lcd + LCD_CTRL1) & RECOVERY_ON_UNDERFLOW) ||
	    (readl(lcd + LCD_CTRL2) & OUTSTANDING_REQS_MASK) != OUTSTANDING_REQS_16 ||
	    (readl(lcd + LCD_AS_CTRL) & 1))
		return dev_err_probe(&pdev->dev, -EIO, "LCD scanout setup failed\n");
	return 0;
}

static const struct of_device_id pwh1_refresh_ids[] = {
	{ .compatible = "sharp,pwh1-lcd-refresh" },
	{ }
};
MODULE_DEVICE_TABLE(of, pwh1_refresh_ids);
static struct platform_driver pwh1_refresh_driver = {
	.probe = pwh1_refresh_probe,
	.driver = {
		.name = "pwh1-lcd-refresh",
		.of_match_table = pwh1_refresh_ids,
	},
};
module_platform_driver(pwh1_refresh_driver);
MODULE_LICENSE("GPL");
