// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitmap.h>
#include <linux/backlight.h>
#include <linux/io.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>

/* shortcut: V690_H1 RAM layout; use a firmware protocol once it is understood. */
struct pwh1_keys {
	void __iomem *state;
	void __iomem *queue;
	void __iomem *gate;
	void __iomem *portb;
	void __iomem *iomux;
	void __iomem *gpio;
	void __iomem *rtos;
	void __iomem *rpc_gate;
	void __iomem *event_gate;
	void __iomem *mu;
	void __iomem *backlight_state;
	void __iomem *touch_state;
	struct backlight_device *backlight;
	bool lid_closed;
	u32 discarded;
	u32 acknowledged;
	bool symbols;
	bool mode_down;
	u32 held;
	unsigned short pressed[26];
};

/* A=01 and Q=11 observed; the intervening alphabet is inferred. */
static const unsigned short alphabet[] = {
	KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I,
	KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R,
	KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
};
static_assert(ARRAY_SIZE(alphabet) == 26);
/* Alphabet scan order; Q-P are digits, A-K and Z-C are punctuation. */
static const unsigned short symbols[] = {
	KEY_MINUS, KEY_RESERVED, KEY_SLASH, KEY_LEFTBRACE, KEY_3, KEY_RIGHTBRACE,
	KEY_BACKSLASH, KEY_SEMICOLON, KEY_8, KEY_APOSTROPHE, KEY_GRAVE, KEY_TAB,
	KEY_RESERVED, KEY_RESERVED, KEY_9, KEY_0, KEY_1, KEY_4,
	KEY_EQUAL, KEY_5, KEY_7, KEY_RESERVED, KEY_2, KEY_DOT,
	KEY_6, KEY_COMMA,
};
static_assert(ARRAY_SIZE(symbols) == ARRAY_SIZE(alphabet));
/* Modifiers must precede printable keys within a polling sample. */
static const struct {
	u8 scan;
	unsigned short key;
} special_keys[] = {
	{ 0x2e, KEY_LEFTSHIFT },
	{ 0x30, KEY_LEFTCTRL },
	{ 0x1d, KEY_F1 }, { 0x1e, KEY_F2 }, { 0x1f, KEY_F3 },
	{ 0x20, KEY_F4 }, { 0x21, KEY_F5 }, { 0x22, KEY_F6 },
	{ 0x25, KEY_ENTER }, { 0x26, KEY_ESC },
	{ 0x27, KEY_UP }, { 0x28, KEY_RIGHT },
	{ 0x29, KEY_DOWN }, { 0x2a, KEY_LEFT },
	{ 0x2b, KEY_PAGEUP }, { 0x2c, KEY_PAGEDOWN },
	{ 0x2d, KEY_BACKSPACE }, { 0x2f, KEY_SPACE },
	{ 0x1b, KEY_MINUS },
};

static bool pwh1_key_down(u32 keys, u8 code)
{
	return (keys & 0xff) == code || ((keys >> 8) & 0xff) == code ||
	       ((keys >> 16) & 0xff) == code;
}

/* V690_H1 message buffer 1: M4 notifications to the retired A7 firmware. */
static void pwh1_discard_notifications(struct pwh1_keys *keys)
{
	void __iomem *q = keys->queue;
	u32 count, head, tail, free;

	/* SEMA4 gate 5; firmware core IDs are M4=1 and A7=2. Never wait. */
	if (readb(keys->gate) & 0xf)
		return;
	writeb(2, keys->gate);
	if ((readb(keys->gate) & 0xf) != 2)
		return;
	mb();
	count = readl(q + 0x10);
	head = readl(q + 0x14);
	tail = readl(q + 0x18);
	free = readl(q + 0x1c);
	/* shortcut: blocked RTOS senders need a wake handshake, not just free space. */
	if (WARN_ON_ONCE(readl(q) != 1 || readl(q + 4) != 1024 ||
			 readl(q + 8) != 8 || readl(q + 12) != 0x2f016000 ||
			 readl(q + 0x20) != 5 ||
			 readl(q + 0x24) != 0x200070ec ||
			 head >= 1024 || tail >= 1024 ||
			 free > 1024 || ((head | tail | free) & 3) ||
			 count > (1024 - free) / 8 ||
			 ((head - tail) & 1023) != ((1024 - free) & 1023)))
		goto unlock;
	if (count) {
		/* Only drain when the producer wait list is empty. */
		writel(head, q + 0x18);
		writel(0, q + 0x10);
		writel(1024, q + 0x1c);
		WRITE_ONCE(keys->discarded, keys->discarded + count);
	}
unlock:
	mb();
	writeb(0, keys->gate);
}

/* V690_H1 1ffd481c sends synchronous calls; 1ffde1f8 completes set-event. */
static void pwh1_ack_events(struct pwh1_keys *keys)
{
	void __iomem *rpc = keys->rtos + 0x6e60;

	/* M4 owns gate 3 until it consumes our response; the receiver never locks it. */
	if ((readb(keys->rpc_gate) & 0xf) != 1 || readl(rpc + 4) ||
	    !readl(rpc + 0x10))
		return;
	/* shortcut: only retired A7 event notifications; other RPCs need real handlers. */
	if (WARN_ON_ONCE(readl(keys->rtos + 0x6ec8) != 0x20006e60 ||
			 readl(rpc) != 3 || readl(rpc + 0x10) != 0xffffff19 ||
			 readl(rpc + 0x14) >> 24 != 1 ||
			 readl(rpc + 0x18) || readl(rpc + 0x1c) != 10 ||
			 readl(rpc + 0x20) || readl(rpc + 0x24) != 2))
		return;
	/* Linux consumes the notification; no original A7 task remains to wake. */
	writel(0, rpc + 8);
	wmb();
	writel(1, rpc + 4);
	WRITE_ONCE(keys->acknowledged, keys->acknowledged + 1);
}

static int pwh1_backlight_update(struct backlight_device *bd)
{
	struct pwh1_keys *keys = bl_get_data(bd);
	void __iomem *q = keys->rtos + 0x6800;
	void __iomem *entry;
	u32 count, head;
	u8 mode = readl(keys->rtos + 0x6094) & 0xff;
	bool on;
	int ret = 0;

	/* V690_H1 property 1040: 1=open, 2=closed, 3=reverse-folded. */
	if (mode >= 1 && mode <= 3)
		keys->lid_closed = mode == 2;
	on = backlight_get_brightness(bd) && !keys->lid_closed;
	if (on == (readl(keys->backlight_state + 4) == 2))
		return 0;

	/* V690_H1 1ffd475c: M4's asynchronous RPC queue, protected by gate 0. */
	if (readb(keys->event_gate) & 0xf)
		return -EBUSY;
	writeb(2, keys->event_gate);
	if ((readb(keys->event_gate) & 0xf) != 2)
		return -EBUSY;
	mb();
	count = readl(q + 4);
	head = readl(q + 8);
	if (readl(keys->rtos + 0x6eb4) != 0x20006800 ||
	    readl(keys->rtos + 0x6ea8) != 16 || readl(q) != 0 ||
	    count > 16 || head >= 16) {
		ret = -EIO;
		goto unlock;
	}
	if (count == 16) {
		ret = -EBUSY;
		goto unlock;
	}
	entry = q + 0x10 + head * 0x30;
	writel(0xffffff19, entry); /* set event on another core */
	writel(2 << 24, entry + 4); /* A7 sender, no original task ID */
	writel(0, entry + 8);
	writel(11, entry + 12); /* asynchronous event handler */
	writel(0, entry + 16);
	writel(1, entry + 20); /* destination M4 */
	writel(8, entry + 24); /* backlight task's event group */
	/* Keep the firmware's calibrated brightness; event 1=on, 2=off. */
	writel(on ? 1 : 2, entry + 28);
	writel((head + 1) % 16, q + 8);
	writel(count + 1, q + 4);
unlock:
	mb();
	writeb(0, keys->event_gate);
	if (!ret) {
		/* Match 1ffd4350/1ffd50c8: notify M4 using general interrupt 3. */
		writel((readl(keys->mu) & ~GENMASK(19, 16)) | BIT(16), keys->mu);
	}
	return ret;
}

static int pwh1_backlight_get(struct backlight_device *bd)
{
	struct pwh1_keys *keys = bl_get_data(bd);

	return readl(keys->backlight_state + 4) == 2;
}

static const struct backlight_ops pwh1_backlight_ops = {
	.update_status = pwh1_backlight_update,
	.get_brightness = pwh1_backlight_get,
};

static void pwh1_touch_poll(struct input_dev *input)
{
	struct pwh1_keys *keys = input_get_drvdata(input);
	void __iomem *touch = keys->touch_state;
	u8 state = readb(touch);
	u32 x = readl(touch + 4), y = readl(touch + 8);
	u32 angle = readl(keys->rtos + 0x607c);
	u32 tmp;

	/* Skip M4's down/debounce transitions and samples changed during reads. */
	if (state != readb(touch) || x != readl(touch + 4) ||
	    y != readl(touch + 8) || (state != 0 && state != 1 && state != 3))
		return;
	if (state != 3 || !readb(touch + 2)) {
		input_report_key(input, BTN_TOUCH, 0);
		input_sync(input);
		return;
	}
	/* Undo V690_H1 1ffd8356's portrait transform, retaining its calibration. */
	if (angle == 270) {
		tmp = x;
		x = y;
		y = 480 - tmp;
	}
	if (x > 853 || y > 479)
		return;
	input_report_key(input, BTN_TOUCH, 1);
	input_report_abs(input, ABS_X, x);
	input_report_abs(input, ABS_Y, y);
	input_sync(input);
}

static ssize_t raw_touch_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct pwh1_keys *keys = dev_get_drvdata(dev);
	void __iomem *touch = keys->touch_state;

	return sysfs_emit(buf, "state=%u enabled=%u x=%u y=%u angle=%u\n",
		readb(touch), readb(touch + 2), readl(touch + 4),
		readl(touch + 8), readl(keys->rtos + 0x607c));
}
static DEVICE_ATTR_RO(raw_touch);

static void pwh1_keys_poll(struct input_dev *input)
{
	struct pwh1_keys *keys = input_get_drvdata(input);
	u8 state = readb(keys->state);
	u32 sample = readl(keys->state + 8) & 0xffffff;
	unsigned int i;
	bool mode_down;
	DECLARE_BITMAP(down, KEY_CNT);

	pwh1_discard_notifications(keys);
	pwh1_ack_events(keys);
	/* Retry busy queues and reconcile coalesced events after rapid lid changes. */
	backlight_update_status(keys->backlight);

	/* State 2 is transitional; state 1 must have cleared the key bytes. */
	if ((state != 1 && state != 3) || readb(keys->state) != state ||
	    (state == 1 && sample))
		return;
	mode_down = pwh1_key_down(sample, 0x31);
	if (mode_down && !keys->mode_down)
		WRITE_ONCE(keys->symbols, !keys->symbols);
	keys->mode_down = mode_down;
	bitmap_zero(down, KEY_CNT);
	for (i = 0; i < ARRAY_SIZE(special_keys); i++)
		if (pwh1_key_down(sample, special_keys[i].scan))
			__set_bit(special_keys[i].key, down);
	for (i = 0; i < ARRAY_SIZE(alphabet); i++) {
		if (!pwh1_key_down(sample, i + 1)) {
			keys->pressed[i] = KEY_RESERVED;
			keys->held &= ~BIT(i);
		} else if (!(keys->held & BIT(i))) {
			keys->pressed[i] = keys->symbols ? symbols[i] : alphabet[i];
			keys->held |= BIT(i);
		}
		if (keys->pressed[i])
			__set_bit(keys->pressed[i], down);
	}
	/* Retain held key identities across mode changes and merge duplicate keys. */
	input_report_key(input, KEY_LEFTSHIFT, test_bit(KEY_LEFTSHIFT, down));
	input_report_key(input, KEY_LEFTCTRL, test_bit(KEY_LEFTCTRL, down));
	for_each_set_bit(i, input->keybit, KEY_CNT)
		input_report_key(input, i, test_bit(i, down));
	input_sync(input);
}

static ssize_t raw_keys_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct pwh1_keys *data = dev_get_drvdata(dev);
	void __iomem *state = data->state;
	u32 keys = readl(state + 8);

	return sysfs_emit(buf, "%02x %02x %02x (state %u)\n",
			 keys & 0xff, (keys >> 8) & 0xff, (keys >> 16) & 0xff,
			 readb(state));
}
static DEVICE_ATTR_RO(raw_keys);

static ssize_t key_mode_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct pwh1_keys *keys = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", READ_ONCE(keys->symbols) ? "symbols" : "letters");
}
static DEVICE_ATTR_RO(key_mode);

static ssize_t discarded_notifications_show(struct device *dev,
					   struct device_attribute *attr, char *buf)
{
	struct pwh1_keys *keys = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(keys->discarded));
}
static DEVICE_ATTR_RO(discarded_notifications);

static ssize_t firmware_status_show(struct device *dev,
                                    struct device_attribute *attr, char *buf)
{
	struct pwh1_keys *keys = dev_get_drvdata(dev);

	/* Read-only samples: M4 can change these fields between reads. */
	return sysfs_emit(buf, "state=%u enable=%u special=%u gate=%x count=%u free=%u wait=%08x discarded=%u\n",
		readb(keys->state), readb(keys->state + 1),
		readb(keys->state + 5), readb(keys->gate),
		readl(keys->queue + 0x10), readl(keys->queue + 0x1c),
		readl(keys->queue + 0x24), READ_ONCE(keys->discarded));
}
static DEVICE_ATTR_RO(firmware_status);

static ssize_t rpc_status_show(struct device *dev, struct device_attribute *attr,
			       char *buf)
{
	struct pwh1_keys *keys = dev_get_drvdata(dev);
	void __iomem *rpc = keys->rtos + 0x6e60;

	return sysfs_emit(buf, "RPC: acked=%u gate=%x done=%u cmd=%08x event=%u bits=%08x\n",
		READ_ONCE(keys->acknowledged), readb(keys->rpc_gate),
		readl(rpc + 4), readl(rpc + 0x10),
		readl(rpc + 0x28), readl(rpc + 0x2c));
}
static DEVICE_ATTR_RO(rpc_status);

static ssize_t gpio_status_show(struct device *dev, struct device_attribute *attr,
                                char *buf)
{
	struct pwh1_keys *keys = dev_get_drvdata(dev);
	u32 enabled = 0;
	unsigned int pin;

	for (pin = 0; pin < 20; pin++)
		if ((BIT(pin) & 0x942c8) &&
		    (readl(keys->portb + pin * 4) & GENMASK(19, 16)))
			enabled |= BIT(pin);
	return sysfs_emit(buf, "IRQ=%05x pending=%05x input=%05x rows=%08x/%08x\n",
		enabled, readl(keys->portb + 0xa0) & 0x942c8,
		readl(keys->gpio + 0x50) & 0x942c8,
		readl(keys->gpio), readl(keys->gpio + 0x40));
}
static DEVICE_ATTR_RO(gpio_status);

static void __iomem *rtos_pointer(struct pwh1_keys *keys, u32 address, u32 size)
{
	if ((address & 3) || address < 0x20000000 ||
	    address > 0x20010000 - size)
		return NULL;
	return keys->rtos + (address - 0x20000000);
}

static ssize_t m4_status_show(struct device *dev, struct device_attribute *attr,
                              char *buf)
{
	struct pwh1_keys *keys = dev_get_drvdata(dev);
	void __iomem *tasks, *mutexes, *task, *mutex;
	void __iomem *running, *flags, *event;
	u32 ta = readl(keys->rtos + 0x3f50);
	u32 ma = readl(keys->rtos + 0x3f64);
	u32 ca = readl(keys->rtos + 0x3f34);
	u32 fa = readl(keys->rtos + 0x3f58);
	ssize_t len;

	/* V690_H1 task 7 is the key task; it acquires mutex 1 before scanning. */
	tasks = rtos_pointer(keys, ta, 32);
	mutexes = rtos_pointer(keys, ma, 8);
	if (!tasks || !mutexes)
		return sysfs_emit(buf, "M4 tables outside mapped RAM: %08x %08x\n", ta, ma);
	ta = readl(tasks + 7 * 4);
	ma = readl(mutexes + 4);
	task = rtos_pointer(keys, ta, 0x74);
	mutex = rtos_pointer(keys, ma, 0x14);
	if (!task || !mutex)
		return sysfs_emit(buf, "M4 objects outside mapped RAM: %08x %08x\n", ta, ma);
	len = sysfs_emit(buf, "task7=%08x id=%u handler=%08x arg=%08x\nmutex1=%08x owner=%u current=%08x\n",
		ta, readw(task + 0x22), readl(task + 0x14), readl(task + 0x4c),
		ma, readw(mutex + 0x10), ca);
	/* The handler is the last syscall, not a live program counter. */
	len += sysfs_emit_at(buf, len,
		"key: flags=%08x wait-id=%u mask=%08x sp=%08x\n",
		readl(task + 0x10), readw(task + 0x32),
		readl(task + 0x50), readl(task + 0x34));
	running = rtos_pointer(keys, ca, 0x74);
	if (running)
		len += sysfs_emit_at(buf, len,
			"current: id=%u flags=%08x handler=%08x arg=%08x\n",
			readw(running + 0x22), readl(running + 0x10),
			readl(running + 0x14), readl(running + 0x4c));
	flags = rtos_pointer(keys, fa, 12);
	if (!flags)
		return len + sysfs_emit_at(buf, len, "Invalid event table: %08x\n", fa);
	fa = readl(flags + 2 * 4);
	event = rtos_pointer(keys, fa, 0x10);
	if (!event)
		return len + sysfs_emit_at(buf, len, "Invalid key event: %08x\n", fa);
	len += sysfs_emit_at(buf, len,
		"event2=%08x bits=%08x head=%08x tail=%08x\n",
		fa, readl(event + 8), readl(event), readl(event + 4));
	return len + sysfs_emit_at(buf, len,
		"scheduler: flags=%08x service=%08x\n",
		readl(keys->rtos + 0x3f00), readl(keys->rtos + 0x3f30));
}
static DEVICE_ATTR_RO(m4_status);

/* V690_H1 1ffdd7d2: rearm the seven key columns, excluding PTB1. */
static ssize_t recover_store(struct device *dev, struct device_attribute *attr,
                             const char *buf, size_t count)
{
	struct pwh1_keys *keys = dev_get_drvdata(dev);
	static const u16 rows[] = { 0x112, 0x111, 0x10a, 0x017, 0x000, 0x012, 0x009 };
	unsigned int pin, i;

	if (!sysfs_streq(buf, "1"))
		return -EINVAL;
	if (readb(keys->state) != 1 || (readl(keys->state + 8) & 0xffffff))
		return -EBUSY;
	for (pin = 0; pin < 20; pin++) {
		if ((BIT(pin) & 0x942c8) &&
		    (readl(keys->portb + pin * 4) & GENMASK(19, 16)))
			return -EALREADY;
	}
	/* shortcut: manual recovery only until M4's lost rearm is understood. */
	writel(0x010842c8, keys->portb + 0x88);
	writel(0x01080009, keys->portb + 0x8c);
	/* Complete 1ffdd7d2 -> 1ffdd79c: drive rows low, then restore mux. */
	for (i = 0; i < ARRAY_SIZE(rows); i++) {
		unsigned int bank = rows[i] >> 8;
		unsigned int bit = rows[i] & 0xff;

		writel(BIT(bit), keys->gpio + bank * 0x40 + 8);
		writel(0x00020120, keys->iomux + bank * 0x80 + bit * 4);
	}
	readl(keys->iomux);
	dev_info(dev, "rearmed key column interrupts\n");
	return count;
}
static DEVICE_ATTR_WO(recover);

static struct attribute *pwh1_keys_attrs[] = {
	&dev_attr_raw_touch.attr,
	&dev_attr_raw_keys.attr,
	&dev_attr_m4_status.attr,
	&dev_attr_gpio_status.attr,
	&dev_attr_recover.attr,
	&dev_attr_firmware_status.attr,
	&dev_attr_rpc_status.attr,
	&dev_attr_key_mode.attr,
	&dev_attr_discarded_notifications.attr,
	NULL,
};
ATTRIBUTE_GROUPS(pwh1_keys);

static int pwh1_keys_probe(struct platform_device *pdev)
{
	void __iomem *code, *state;
	struct pwh1_keys *keys;
	struct input_dev *input;
	struct backlight_device *backlight;
	struct backlight_properties props = { .type = BACKLIGHT_RAW, .max_brightness = 1 };
	unsigned int i;
	int ret;

	code = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(code))
		return PTR_ERR(code);
	/* V690_H1 key task at M4 address 1ffd76b4: push; ldr.w r5. */
	if (readl(code) != 0xf8dfb57f || readl(code + 4) != 0x22005738)
		return dev_err_probe(&pdev->dev, -ENODEV, "M4 key firmware mismatch\n");

	state = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(state))
		return PTR_ERR(state);
	keys = devm_kzalloc(&pdev->dev, sizeof(*keys), GFP_KERNEL);
	if (!keys)
		return -ENOMEM;
	keys->state = state;
	keys->queue = devm_platform_ioremap_resource(pdev, 2);
	if (IS_ERR(keys->queue))
		return PTR_ERR(keys->queue);
	keys->gate = devm_platform_ioremap_resource(pdev, 3);
	if (IS_ERR(keys->gate))
		return PTR_ERR(keys->gate);
	code = devm_platform_ioremap_resource(pdev, 4);
	if (IS_ERR(code))
		return PTR_ERR(code);
	if (readw(code) != 0xe92d || readw(code + 2) != 0x43fe ||
	    readw(code + 4) != 0x4680 || readw(code + 6) != 0x000d)
		return dev_err_probe(&pdev->dev, -ENODEV, "M4 message firmware mismatch\n");
	keys->portb = devm_platform_ioremap_resource(pdev, 5);
	if (IS_ERR(keys->portb))
		return PTR_ERR(keys->portb);
	/* Shared with firmware and the reboot driver; do not claim whole banks. */
	{
		struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, 6);

		if (!res)
			return -EINVAL;
		keys->iomux = devm_ioremap(&pdev->dev, res->start, resource_size(res));
		res = platform_get_resource(pdev, IORESOURCE_MEM, 7);
		if (!res)
			return -EINVAL;
		keys->gpio = devm_ioremap(&pdev->dev, res->start, resource_size(res));
		if (!keys->iomux || !keys->gpio)
			return -ENOMEM;
	}
	{
		struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, 8);

		if (!res)
			return -EINVAL;
		keys->rtos = devm_ioremap(&pdev->dev, res->start, resource_size(res));
		if (!keys->rtos)
			return -ENOMEM;
	}
	keys->rpc_gate = devm_platform_ioremap_resource(pdev, 9);
	if (IS_ERR(keys->rpc_gate))
		return PTR_ERR(keys->rpc_gate);
	code = devm_platform_ioremap_resource(pdev, 10);
	if (IS_ERR(code))
		return PTR_ERR(code);
	if (readl(code) != 0x4ff8e92d || readl(code + 4) != 0x980b4604)
		return dev_err_probe(&pdev->dev, -ENODEV, "M4 RPC firmware mismatch\n");
	keys->event_gate = devm_platform_ioremap_resource(pdev, 11);
	if (IS_ERR(keys->event_gate))
		return PTR_ERR(keys->event_gate);
	keys->mu = devm_platform_ioremap_resource(pdev, 12);
	if (IS_ERR(keys->mu))
		return PTR_ERR(keys->mu);
	keys->backlight_state = devm_platform_ioremap_resource(pdev, 13);
	if (IS_ERR(keys->backlight_state))
		return PTR_ERR(keys->backlight_state);
	keys->touch_state = devm_platform_ioremap_resource(pdev, 14);
	if (IS_ERR(keys->touch_state))
		return PTR_ERR(keys->touch_state);
	code = devm_platform_ioremap_resource(pdev, 15);
	if (IS_ERR(code))
		return PTR_ERR(code);
	if (readw(code) != 0xb5f8 || readw(code + 2) != 0x4607 ||
	    readw(code + 4) != 0x460c || readw(code + 6) != 0x4615)
		return dev_err_probe(&pdev->dev, -ENODEV, "M4 touch firmware mismatch\n");
	props.brightness = readl(keys->backlight_state + 4) == 2;
	backlight = devm_backlight_device_register(&pdev->dev, "pwh1", &pdev->dev,
		keys, &pwh1_backlight_ops, &props);
	if (IS_ERR(backlight))
		return PTR_ERR(backlight);
	keys->backlight = backlight;
	platform_set_drvdata(pdev, keys);

	input = devm_input_allocate_device(&pdev->dev);
	if (!input)
		return -ENOMEM;
	input->name = "PW-H1 firmware touchscreen";
	input->id.bustype = BUS_HOST;
	input_set_drvdata(input, keys);
	input_set_capability(input, EV_KEY, BTN_TOUCH);
	__set_bit(INPUT_PROP_DIRECT, input->propbit);
	input_set_abs_params(input, ABS_X, 0, 853, 0, 0);
	input_set_abs_params(input, ABS_Y, 0, 479, 0, 0);
	ret = input_setup_polling(input, pwh1_touch_poll);
	if (ret)
		return ret;
	input_set_poll_interval(input, 20);
	ret = input_register_device(input);
	if (ret)
		return ret;

	input = devm_input_allocate_device(&pdev->dev);
	if (!input)
		return -ENOMEM;
	input->name = "PW-H1 firmware keyboard";
	input->id.bustype = BUS_HOST;
	input_set_drvdata(input, keys);
	for (i = 0; i < ARRAY_SIZE(alphabet); i++)
		input_set_capability(input, EV_KEY, alphabet[i]);
	for (i = 0; i < ARRAY_SIZE(symbols); i++)
		if (symbols[i])
			input_set_capability(input, EV_KEY, symbols[i]);
	for (i = 0; i < ARRAY_SIZE(special_keys); i++)
		input_set_capability(input, EV_KEY, special_keys[i].key);
	__set_bit(EV_REP, input->evbit);
	ret = input_setup_polling(input, pwh1_keys_poll);
	if (ret)
		return ret;
	input_set_poll_interval(input, 20);
	return input_register_device(input);
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
