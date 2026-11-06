// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitmap.h>
#include <linux/io.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>

/* shortcut: V690_H1 RAM layout; use a firmware protocol once it is understood. */
struct pwh1_keys {
	void __iomem *state;
	u32 sample;
	u8 stable;
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

static void pwh1_keys_poll(struct input_dev *input)
{
	struct pwh1_keys *keys = input_get_drvdata(input);
	u8 state = readb(keys->state);
	u32 sample = readl(keys->state + 8) & 0xffffff;
	unsigned int i;
	u8 required = sample ? 2 : 4;
	bool mode_down;
	DECLARE_BITMAP(down, KEY_CNT);

	/* State 2 is transitional; state 1 must have cleared the key bytes. */
	if ((state != 1 && state != 3) || readb(keys->state) != state ||
	    (state == 1 && sample)) {
		keys->stable = 0;
		return;
	}

	/* Debounce presses for 20 ms and all-key releases for 60 ms. */
	if (!keys->stable || sample != keys->sample) {
		keys->sample = sample;
		keys->stable = 1;
		return;
	}
	if (keys->stable < required)
		keys->stable++;
	if (keys->stable < required)
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

static struct attribute *pwh1_keys_attrs[] = {
	&dev_attr_raw_keys.attr,
	&dev_attr_key_mode.attr,
	NULL,
};
ATTRIBUTE_GROUPS(pwh1_keys);

static int pwh1_keys_probe(struct platform_device *pdev)
{
	void __iomem *code, *state;
	struct pwh1_keys *keys;
	struct input_dev *input;
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
	platform_set_drvdata(pdev, keys);

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
	/* shortcut: no autorepeat until firmware key-release timing is verified. */
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
