// SPDX-License-Identifier: GPL-2.0-only
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/irqchip/arm-gic.h>
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
	void __iomem *gicd;
	void __iomem *gicc;
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

struct irq_snapshot {
	u32 count, compare, channel, status;
	u32 enabled, pending, active, group, priority, target;
	u32 distributor, cpu, mask, highest, running;
};

static void capture_irq(struct pwh1_idle_test *test, struct irq_snapshot *s)
{
	/* TPM5 is SPI 22, GIC INTID 54: bank 1, bit 22. Never read IAR. */
	s->count = readl(test->tpm + 0x14);
	s->compare = readl(test->tpm + 0x24);
	s->channel = readl(test->tpm + 0x20);
	s->status = readl(test->tpm + 0x1c);
	s->enabled = !!(readl(test->gicd + GIC_DIST_ENABLE_SET + 4) & BIT(22));
	s->pending = !!(readl(test->gicd + GIC_DIST_PENDING_SET + 4) & BIT(22));
	s->active = !!(readl(test->gicd + GIC_DIST_ACTIVE_SET + 4) & BIT(22));
	s->group = !!(readl(test->gicd + GIC_DIST_IGROUP + 4) & BIT(22));
	s->priority = readb(test->gicd + GIC_DIST_PRI + 54);
	s->target = readb(test->gicd + GIC_DIST_TARGET + 54);
	s->distributor = readl(test->gicd + GIC_DIST_CTRL);
	s->cpu = readl(test->gicc + GIC_CPU_CTRL);
	s->mask = readl(test->gicc + GIC_CPU_PRIMASK);
	s->highest = readl(test->gicc + GIC_CPU_HIGHPRI);
	s->running = readl(test->gicc + GIC_CPU_RUNNINGPRI);
}

static void print_irq(const char *phase, const struct irq_snapshot *s)
{
	pr_emerg("WFI TEST: %s CNT=%08x CMP=%08x CH=%x ST=%x\n",
		 phase, s->count, s->compare, s->channel, s->status);
	pr_emerg("WFI TEST: %s IRQ54 E=%u P=%u A=%u G=%u PRI=%x TGT=%x\n",
		 phase, s->enabled, s->pending, s->active, s->group, s->priority, s->target);
	pr_emerg("WFI TEST: %s GIC D=%x C=%x PMR=%x HPPIR=%x RPR=%x\n",
		 phase, s->distributor, s->cpu, s->mask, s->highest, s->running);
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
	struct irq_snapshot awake, pre, post;
	unsigned long flags;
	u32 before, shallow, after;
	u32 mask = GENMASK(17, 16) | GENMASK(2, 0);

	if (!sysfs_streq(buf, "1"))
		return -EINVAL;
	if (!mutex_trylock(&test_lock))
		return -EBUSY;
	pr_emerg("WFI TEST: preparing; next tick should wake CPU\n");
	timer_rate(test, "BEFORE rate");
	/* Compare delivery with the CPU awake before entering WFI. */
	local_irq_save(flags);
	mdelay(20);
	capture_irq(test, &awake);
	local_irq_restore(flags);
	print_irq("AWAKE", &awake);
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
	capture_irq(test, &pre);
	stamp = timestamp(test);
	counter = readl(test->tpm + 0x14);
	cpu_do_idle();
	cycles = (readl(test->tpm + 0x14) - counter) & test->counter_mask;
	elapsed = timestamp(test) - stamp;
	capture_irq(test, &post);
	pr_emerg("WFI TEST: RETURN, IRQs still masked\n");
	writel(before, smc + 0x10);
	after = readl(smc + 0x10);
	pr_emerg("WFI TEST: SLEEP cycles=%llu TST-us=%llu\n", cycles, elapsed);
	local_irq_restore(flags);
	print_irq("PRE", &pre);
	print_irq("POST", &post);
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
	unsigned int i;
	void __iomem **shared[3];

	test = devm_kzalloc(&pdev->dev, sizeof(*test), GFP_KERNEL);
	if (!test)
		return -ENOMEM;
	test->smc = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(test->smc))
		return PTR_ERR(test->smc);
	test->tstmr = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(test->tstmr))
		return PTR_ERR(test->tstmr);
	/* Existing timer/GIC drivers own these regions; only read their state. */
	shared[0] = &test->tpm;
	shared[1] = &test->gicd;
	shared[2] = &test->gicc;
	for (i = 0; i < ARRAY_SIZE(shared); i++) {
		res = platform_get_resource(pdev, IORESOURCE_MEM, i + 2);
		if (!res)
			return -EINVAL;
		*shared[i] = devm_ioremap(&pdev->dev, res->start, resource_size(res));
		if (!*shared[i])
			return -ENOMEM;
	}
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
