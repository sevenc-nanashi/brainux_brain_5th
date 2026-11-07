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

struct pwh1_idle_test {
	void __iomem *smc;
	void __iomem *tstmr;
	void __iomem *tpm;
	u32 counter_mask;
};

/* Protect the latched high word from competing reads in IRQ handlers. */
static u64 timestamp(struct pwh1_idle_test *test)
{
	unsigned long flags;
	u32 low, high;

	local_irq_save(flags);
	low = readl(test->tstmr);
	high = readl(test->tstmr + 4);
	local_irq_restore(flags);

	return ((u64)high << 32) | low;
}

static void timer_state(struct pwh1_idle_test *test, const char *phase)
{
	pr_emerg("WFI TEST: %s TPM SC=%08x CONF=%08x\n", phase,
		 readl(test->tpm + 0x10), readl(test->tpm + 0x84));
}

static void timer_rate(struct pwh1_idle_test *test, const char *phase)
{
	u64 stamp = timestamp(test);
	u64 counter = readl(test->tpm + 0x14);
	u64 elapsed, cycles;

	mdelay(20);
	cycles = (readl(test->tpm + 0x14) - counter) & test->counter_mask;
	elapsed = timestamp(test) - stamp;
	pr_emerg("WFI TEST: %s cycles=%llu TST-us=%llu\n", phase, cycles, elapsed);
}

static ssize_t run_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct pwh1_idle_test *test = dev_get_drvdata(dev);
	void __iomem *smc = test->smc;
	u64 stamp, counter, elapsed, cycles;
	unsigned long flags;
	u32 before, shallow, after;
	u32 mask = GENMASK(17, 16) | GENMASK(2, 0);

	if (!sysfs_streq(buf, "1"))
		return -EINVAL;
	if (!mutex_trylock(&test_lock))
		return -EBUSY;
	pr_emerg("WFI TEST: preparing; next tick should wake CPU\n");
	timer_rate(test, "BEFORE rate");
	timer_state(test, "BEFORE");
	pr_emerg("WFI TEST: ENTER (after console settles)\n");
	/* Drain console work with IRQs enabled; do not create a pending tick. */
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
	stamp = timestamp(test);
	counter = readl(test->tpm + 0x14);
	cpu_do_idle();
	cycles = (readl(test->tpm + 0x14) - counter) & test->counter_mask;
	elapsed = timestamp(test) - stamp;
	pr_emerg("WFI TEST: RETURN, IRQs still masked\n");
	writel(before, smc + 0x10);
	after = readl(smc + 0x10);
	pr_emerg("WFI TEST: SLEEP cycles=%llu TST-us=%llu\n", cycles, elapsed);
	timer_state(test, "AFTER");
	local_irq_restore(flags);
	timer_rate(test, "AFTER rate");
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
	struct pwh1_idle_test *test;
	struct resource *res;
	u32 width;

	test = devm_kzalloc(&pdev->dev, sizeof(*test), GFP_KERNEL);
	if (!test)
		return -ENOMEM;
	test->smc = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(test->smc))
		return PTR_ERR(test->smc);
	test->tstmr = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(test->tstmr))
		return PTR_ERR(test->tstmr);
	/* TPM is owned by the clocksource driver; this mapping is read-only. */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 2);
	if (!res)
		return -EINVAL;
	test->tpm = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	if (!test->tpm)
		return -ENOMEM;
	width = (readl(test->tpm + 4) >> 16) & 0xff;
	if (width != 16 && width != 32)
		return -ENODEV;
	test->counter_mask = GENMASK(width - 1, 0);
	platform_set_drvdata(pdev, test);
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
