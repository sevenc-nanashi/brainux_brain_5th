// SPDX-License-Identifier: GPL-2.0-only
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <asm/proc-fns.h>

/* Explicit diagnostic only; normal idle remains polling. */
static DEFINE_MUTEX(test_lock);

static ssize_t run_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	void __iomem *smc = dev_get_drvdata(dev);
	unsigned long flags;
	u32 before, shallow, after;
	u32 mask = GENMASK(17, 16) | GENMASK(2, 0);

	if (!sysfs_streq(buf, "1"))
		return -EINVAL;
	if (!mutex_trylock(&test_lock))
		return -EBUSY;
	pr_emerg("WFI TEST: preparing; next tick should wake CPU\n");
	/* Allow the console's last line to reach scanout before sleeping. */
	msleep(100);
	local_irq_save(flags);
	before = readl(smc + 0x10);
	shallow = (before & ~mask) | (3 << 16);
	writel(shallow, smc + 0x10);
	after = readl(smc + 0x10);
	if ((after & mask) != (shallow & mask)) {
		writel(before, smc + 0x10);
		local_irq_restore(flags);
		mutex_unlock(&test_lock);
		return -EIO;
	}
	pr_emerg("WFI TEST: ENTER PMCTRL=%08x -> %08x\n", before, after);
	cpu_do_idle();
	pr_emerg("WFI TEST: RETURN, IRQs still masked\n");
	writel(before, smc + 0x10);
	after = readl(smc + 0x10);
	local_irq_restore(flags);
	pr_emerg("WFI TEST: IRQs restored, PMCTRL=%08x\n", after);
	mutex_unlock(&test_lock);
	if ((after & mask) != (before & mask))
		return -EIO;
	return count;
}
static DEVICE_ATTR_WO(run);

static struct attribute *pwh1_idle_test_attrs[] = {
	&dev_attr_run.attr,
	NULL,
};
ATTRIBUTE_GROUPS(pwh1_idle_test);

static int pwh1_idle_test_probe(struct platform_device *pdev)
{
	void __iomem *smc = devm_platform_ioremap_resource(pdev, 0);

	if (IS_ERR(smc))
		return PTR_ERR(smc);
	platform_set_drvdata(pdev, smc);
	return 0;
}

static const struct of_device_id pwh1_idle_test_ids[] = {
	{ .compatible = "sharp,pwh1-idle-test" },
	{ }
};
MODULE_DEVICE_TABLE(of, pwh1_idle_test_ids);
static struct platform_driver pwh1_idle_test_driver = {
	.probe = pwh1_idle_test_probe,
	.driver = {
		.name = "pwh1-idle-test",
		.of_match_table = pwh1_idle_test_ids,
		.dev_groups = pwh1_idle_test_groups,
	},
};
module_platform_driver(pwh1_idle_test_driver);
MODULE_LICENSE("GPL");
