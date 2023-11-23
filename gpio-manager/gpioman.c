#include <linux/module.h>
#include <linux/sysfs.h>
#include <linux/of.h>    /* of_device_id */
#include <linux/platform_device.h>  /* platform device and driver */
#include <linux/gpio/consumer.h>    /* gpod_get etc */
#include <linux/string.h>
#include <linux/list.h>
#include <linux/slab.h>

#ifdef USE_HR_TIMERS
#include <linux/hrtimer.h>
#else
#include <linux/timer.h>
#endif

/*
 * NOTE: these are *logic* values that signify a logic level assertion
 * on a gpio line. LOGIC_HIGH does *not* necessarily mean the voltage
 * level will be high, since e.g. the line may be configured as active low.
 */
#define LOGIC_HIGH 1
#define LOGIC_LOW  0

#define MS_PER_SEC 1000
#define US_PER_SEC 1000000   /* 10e6 */
#define KERNEL_HERTZ HZ      /* see /usr/include/asm/param.h */

#define message(fmt, ...) pr_info(KBUILD_MODNAME ": " fmt "\n", ##__VA_ARGS__)
#define debug(fmt, ...)   pr_debug(KBUILD_MODNAME ": " fmt "\n", ##__VA_ARGS__)
#define match(a, b) strcmp(a, b) == 0
#define UNUSED(x) (void)x

/*
 * This is the "function" prefix followed by the "-gpios" suffix in
 * the DT property name. This module expects all devices to use the
 * same property name ('custom-gpios') for simplicity */
#define GPIO_FUNCTION "custom"

/*
 * Main sysfs entry to nest alll other properties under */
#define DRIVER_SYSFS_DIRNAME "gpioman-driver"

/*
 * Per gpio-pin state. Each gpio is associated with a virtual
 * (since for our purposes there is no fixed physical device)
 * platform device; therefore this is also per-device state. */
struct gpio_line_state {
    struct list_head list;
    struct kobject kobj;
    const char *devname;  /* property read from the device tree */

#ifdef USE_HR_TIMERS
    struct hrtimer timer;
#else
    struct timer_list timer;
#endif

    struct gpio_desc *gpio_descriptor;

    /* syfs-controlled */
    int pulse_period;

    int on_cycles;
    int off_cycles;
    int counter;

    int pin_ctl_enabled;
    int pin_logic_level;
    /* ----------------- */
};


static bool debug_mode = false;
static LIST_HEAD(list);               /* track live gpio_line_state instances */
struct kobject *driver_sysfs_entry;   /* main driver sysfs dir */


#ifdef USE_HR_TIMERS
static enum hrtimer_restart hr_interval_cb(struct hrtimer *timer){
    struct gpio_line_state *gls;
    debug("called hr interval callback");

    gls = container_of(timer, struct gpio_line_state, timer);

    if (!gls->pin_ctl_enabled)  /* if status==0 in sysfs, always LOW */
        return HRTIMER_NORESTART;

    /*
     * Simple state machine; a counter is incremented from 0 to
     * (on_cycles + offcycles).
     *  - while counter is between [0, on_cycles), drive the line HIGH.
     *  - while counter is between [on_cycles, on_cycles+off_cycles),
     *    drive the line LOW.  */
    if (gls->pin_logic_level == LOGIC_HIGH){
        if (gls->counter++ == gls->on_cycles){
            /* if there are off cycles, then move state machine to the
             * off-cyles state; otherwise stay in on_cycles state: reset
             * counter and start over */
            if (gls->off_cycles > 0) gls->pin_logic_level = LOGIC_LOW;
            else gls->counter = 0;
        }
    }
    else if (gls->pin_logic_level == LOGIC_LOW){
        /* should only ever enter here if there are off cycles */
        BUG_ON(gls->off_cycles == 0);

        if (gls->counter++ == (gls->on_cycles + gls->off_cycles)){
            gls->pin_logic_level = LOGIC_HIGH;

            /* back to on_cycles state; counter is set to 1 because we're
             * switching to the on_cycles state right away, contributing 1 to
             * the counter (i.e. line is being set to be high for this cycle). */
            gls->counter = 1;
        }
    }

    debug("LEVEL: %d  counter=%d", gls->pin_logic_level, gls->counter);
    gpiod_set_value(gls->gpio_descriptor, gls->pin_logic_level);

    hrtimer_forward_now(timer, ns_to_ktime((u64)gls->pulse_period * 1000));
    return HRTIMER_RESTART;
}

#else  /* !USE_HR_TIMERS */

/*
 * refer to comments in the hr version of the callback function */
static void lr_interval_cb(struct timer_list *timer){
    struct gpio_line_state *gls =
        container_of(timer, struct gpio_line_state, timer);

    debug("called lr interval callback");

    if (!gls->pin_ctl_enabled) return;

    if (mod_timer(timer, jiffies + msecs_to_jiffies(gls->pulse_period))){
        message("failed to rearm timer");
    }

    // TODO refactor to match the other function once tested
    if (gls->pin_logic_level == LOGIC_HIGH){
        if (gls->counter++ == gls->on_cycles){
            /* if there are off cycles, then move state machine to the
             * off-cyle state; otherwise reset counter and start over */
            if (gls->off_cycles > 0) gls->pin_logic_level = LOGIC_LOW;
            else gls->counter = 0;
        }
    }
    else if (gls->pin_logic_level == LOGIC_LOW){
        /* should only enter here if there are off cycles */
        BUG_ON(gls->off_cycles == 0);

        if (gls->counter++ == (gls->on_cycles + gls->off_cycles)){
            gls->pin_logic_level = LOGIC_HIGH;
            gls->counter = 1;  /* back to logic high state; 1 because we're setting high once right away */
        }
    }

    //message("LEVEL: %d  counter=%d", gls->pin_logic_level, gls->counter);
    gpiod_set_value(gls->gpio_descriptor, gls->pin_logic_level);
}
#endif   /* USE_HR_TIMERS */


/* =================================================
 * ==== Generic sysfs operation callbacks ==========
 * =================================================
 * - these are called for the default attributes created
 *   in sysfs by this module, for each device (gpio line)
 *   managed.
 * -----------------------------------------------*/

/* called when user reads sysfs attribute */
static ssize_t read_sysfs_attribute(
        struct kobject *kobj,
        struct kobj_attribute *kattr,
        char *buf)
{
	int var;
    const char *attribute = kattr->attr.name;
    struct gpio_line_state *gls = container_of(kobj, struct gpio_line_state, kobj);

    debug("called read_sysfs_attribute");

    if (match(attribute, "status"))           var = gls->pin_ctl_enabled;
    else if (match(attribute, "on_cycles"))   var = gls->on_cycles;
    else if (match(attribute, "off_cycles"))  var = gls->off_cycles;

    else if (match(attribute, "freq")){
#ifdef USE_HR_TIMERS
        var = US_PER_SEC/gls->pulse_period;
#else
        var = MS_PER_SEC/gls->pulse_period;
#endif
    }

	return sprintf(buf, "%u\n", var);
}

/*
 * The frquency freq is specified in microseconds when using high-res timers;
 * otherwise it is specified in millisecondfor convenience when using low-res
 * timers, but note that even milliseconds are too granular since the resolution
 * is at best that of the jiffy.
 * Specifically, if the kernel HZ variable is e.g 250 then it's pointless
 * for the user to set a higher value than that for the frequency in sysfs.
 * The callback will not be invoked more than HZ times a second.
 *
 * NOTE: the HZ is a compile-time constant set in kconfig. It can be checked
 * at the command line like this:
 *    cat /boot/config-$(uname -r) | grep -i 'HZ='
 * -- assuming the kernel .config is stored there for the platform.
 */
void set_gls_frequency(struct gpio_line_state *gls, int freq){
#ifdef USE_HR_TIMERS
    /* user should use common sense: the kernel will certainly not be
    * calling the callback every microsecond, especially on a busy
    * system! */
    gls->pulse_period = freq > 0 ? US_PER_SEC / freq : 0;
#else
    /* use the jiffy if user has specified a higher freq than that since
     * you cannot get a more granular resolution than the jiffy */
    if (freq > KERNEL_HERTZ){
        message("Frequency setting cannot be met; defaulting to HZ (%u)",
                KERNEL_HERTZ);
        freq = KERNEL_HERTZ;
    }
    gls->pulse_period = freq > 0 ? MS_PER_SEC / freq : 0;
#endif

    if (gls->pin_ctl_enabled){
        gpiod_set_value(gls->gpio_descriptor, LOGIC_HIGH);
        /* if freq>0 and status=1, start timer in case it was disabled;
         * else if freq=0, no timer needed so cancel in case it's running. */
#ifdef USE_HR_TIMERS
        if (freq > 0){
            hrtimer_start(&gls->timer, ms_to_ktime(0), HRTIMER_MODE_REL);
        } else { hrtimer_cancel(&gls->timer); }
#else
        if (freq > 0){
            mod_timer(&gls->timer, jiffies + msecs_to_jiffies(gls->pulse_period));
        } else { del_timer(&gls->timer); }
#endif
    }
}

/* called when user writes to sysfs attribute */
static ssize_t write_sysfs_attribute(
        struct kobject *kobj,
        struct kobj_attribute *kattr,
        const char *buf, size_t count)
{
	int var, rc;
    struct gpio_line_state *gls;
    const char *attribute = kattr->attr.name;
	rc = kstrtoint(buf, 10, &var);

    debug("called write_sysfs_attribute");

	if (rc < 0 || var < 0){
        message("Invalid sysfs write: value must be positive integer");
        return EINVAL;
    }

    gls = container_of(kobj, struct gpio_line_state, kobj);

    /* restart state machine; NOTE: always start in the on_cycles state */
    gls->counter = 0;

	if (match(attribute, "status")){
        switch(var){

        case LOGIC_LOW: /* essentially disabled; stop timer and set to low */
#ifdef USE_HR_TIMERS
            hrtimer_cancel(&gls->timer);
#else
            del_timer(&gls->timer);
#endif
            gls->pin_ctl_enabled = false;
            gls->pin_logic_level = LOGIC_LOW;
            gpiod_set_value(gls->gpio_descriptor, LOGIC_LOW);
            break;

        case LOGIC_HIGH:
            gls->pin_logic_level = LOGIC_HIGH;
            gpiod_set_value(gls->gpio_descriptor, LOGIC_HIGH);

            if (gls->pulse_period > 0){
                /* restart timer in case it was disabled; NOTE: if pulse_period
                 * is 0, there are no pulses and hence no timer. Only a stable
                 * LOGIC_HIGH state. */
#ifdef USE_HR_TIMERS
                hrtimer_start(&gls->timer, ms_to_ktime(0), HRTIMER_MODE_REL);
#else
                mod_timer(&gls->timer, jiffies+msecs_to_jiffies(gls->pulse_period));
#endif
            }
            gls->pin_ctl_enabled = true;
            break;
        }
    }

    else if (match(attribute, "freq")){
        set_gls_frequency(gls, var);
    }
    else if (match(attribute, "on_cycles")){
        gls->on_cycles = var;
    }
    else if (match(attribute, "off_cycles")){
        gls->off_cycles = var;
    }

    /* used whole buffer; see
     * https://www.kernel.org/doc/html/next/filesystems/sysfs.html fmi */
	return count;
}

/* =======================================================
 * ==== Global driver sysfs attribute callbacks ==========
 * =======================================================
 * - these are called for sysfs attributes that apply to
 *   this module as a whole.
 * -----------------------------------------------*/
static ssize_t read_sysfs_driver_attribute(struct kobject *kobj,
        struct kobj_attribute *attr, char *buf)
{
    debug("called %s", __func__);
	return sprintf(buf, "%u\n", debug_mode);
}

static ssize_t write_sysfs_driver_attribute(struct kobject *kobj,
        struct kobj_attribute *attr, const char *buf, size_t count)
{
    int var = 0;
	int rc = kstrtoint(buf, 10, &var);

    debug("called %s", __func__);

	if (rc < 0) return rc;

    debug_mode = var;
    return count;
}
/* -----------------------------------------------------------*/

/*
 * Driver attributes; NOTE: *not* currently used. Kept only for reference
 * / as a placeholder.
 *
 * To manually add this as a driver sysfs property under the kobject
 * directory corresponding to this driver, call sysfs_create_file()
 * in the module init function (and balance it with a call to
 * sysfs_remove_file in the module exit function. ) */
static struct kobj_attribute debug_mode_sysfs_toggle =
	__ATTR(debug, 0664, read_sysfs_driver_attribute, write_sysfs_driver_attribute);

/*
 * Per-device (i.e. per-gpio line) attributes. These are used as the default
 * attributes for the gpio_control_interface ktype and sysfs files corresponding
 * to these attributes will therefore be created automatically for each device
 * bound to this driver. */
static struct kobj_attribute pin_logic_level_attribute =
	__ATTR(status, 0664, read_sysfs_attribute, write_sysfs_attribute);

static struct kobj_attribute freq_attribute =
	__ATTR(freq, 0664, read_sysfs_attribute, write_sysfs_attribute);

static struct kobj_attribute on_cycles_attribute =
	__ATTR(on_cycles, 0664, read_sysfs_attribute, write_sysfs_attribute);

static struct kobj_attribute off_cycles_attribute =
	__ATTR(off_cycles, 0664, read_sysfs_attribute, write_sysfs_attribute);

static struct attribute *default_gpio_control_interface_attributes[] = {
	&pin_logic_level_attribute.attr,
	&freq_attribute.attr,
    &on_cycles_attribute.attr,
    &off_cycles_attribute.attr,
	NULL
};

static int rm_func(struct platform_device *pdev){
    struct gpio_line_state *gls = dev_get_drvdata(&pdev->dev);
    kobject_put(&gls->kobj);   /* let the release callback do its thing */
    return 0;
}

/*
 * Called by the kobject subsystem when the reference count for the object
 * (as decremented by kobject_put() gets to 0).
 */
static void gpio_line_state_release_func(struct kobject *kobj){
    struct gpio_line_state *gls = container_of(kobj, struct gpio_line_state, kobj);
    debug("Kobj release called for device %s", gls->devname);

#ifdef USE_HR_TIMERS
    hrtimer_cancel(&gls->timer);
#else
    /* NOTE: recent kernels will have renamed this to
     * timer_delete_sync(&gls->timer). Not so on 5.15 */
    del_timer_sync(&gls->timer);
#endif

    gpiod_set_value(gls->gpio_descriptor, LOGIC_LOW);
    gpiod_put(gls->gpio_descriptor);
    list_del(&gls->list);
    kfree(gls);
}

/*
 * NOTE: buffer is provided by the kernel and is always of size PAGE_SIZE.
 * See https://www.kernel.org/doc/Documentation/filesystems/sysfs.txt fmi.
 */
static ssize_t gls_show_func(struct kobject *kobj,
				struct attribute *attr, char *buffer)
{
    struct kobj_attribute *kattr;
    debug("%s called", __func__);

    kattr = container_of(attr, struct kobj_attribute, attr);
    return read_sysfs_attribute(kobj, kattr, buffer);
}

static ssize_t gls_store_func(struct kobject *kobj,
				struct attribute *attr,
				const char *buffer, size_t count)
{
    struct kobj_attribute *kattr;
    debug("%s called", __func__);

    kattr = container_of(attr, struct kobj_attribute, attr);
    return write_sysfs_attribute(kobj, kattr, buffer, count);
}

static const struct sysfs_ops gpio_control_interface_syfs_ops = {
	.show = gls_show_func,
	.store = gls_store_func
};

/* ktype for sysfs entries corresponding to gpio_line_state instances */
static struct kobj_type gpio_control_interface_ktype = {
	.release = gpio_line_state_release_func,
	.sysfs_ops = &gpio_control_interface_syfs_ops,
	.default_attrs = default_gpio_control_interface_attributes,
};

/*
 * Initialize state variables to defaults.
 */
static inline int initialize_gls(
        struct gpio_line_state *gls,
        struct platform_device *pdev,
        struct gpio_desc *desc,
        const char *device_name
        )
{
    int rc;

    gls->devname = device_name;
    gls->gpio_descriptor = desc;

    /* Always LOW (and no timer) by default */
    gls->pulse_period = 0;
    gls->pin_logic_level = LOGIC_LOW;

    /* alternate between high and low (=>square wave, 50% duty cycle)
     * by default, when user sets status=1 (HIGH) */
    gls->on_cycles = 1;
    gls->off_cycles = 1;

#ifdef USE_HR_TIMERS
    hrtimer_init(&gls->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    gls->timer.function = hr_interval_cb;
#else
    timer_setup(&gls->timer, lr_interval_cb, 0);
#endif

    INIT_LIST_HEAD(&gls->list);
    list_add(&gls->list, &list);

    dev_set_drvdata(&pdev->dev, gls);

    /* create a sysfs directory with the given name for each device,
     * populated with the default attributes specified for
     * gpio_control_interface_ktype */
    rc = kobject_init_and_add(
            &gls->kobj,
            &gpio_control_interface_ktype,
            driver_sysfs_entry,  /* nest entries under the driver directory */
            device_name);

    if (rc){
        message("Failed to initialize kobject (%d) for %s", rc, device_name);
        kobject_put(&gls->kobj);
    }

    return rc;
}

/*
 * Called when a matching device is found. Return 0 to confirm to
 * confirm the match as valid and proceed with binding the device
 * to this driver.
 * See platform_match in drivers/base/platform.c fmi. */
static int gpio_probe_func(struct platform_device *pdev){
    const char *of_prop = "";
    struct gpio_line_state *gls = NULL;
    struct gpio_desc *desc = NULL;
    int rc;

    debug("%s called", __func__);

    if ((rc = of_property_read_string(pdev->dev.of_node, "name", &of_prop))){
        message("Failed to read DT property; failed to bind device");
        return rc;
    }

    desc = gpiod_get(&pdev->dev, GPIO_FUNCTION, GPIOD_OUT_LOW);
    if (IS_ERR(desc)){
        message("Failed to get GPIO descriptor for device %s", of_prop);
        return PTR_ERR(desc);
    }

    if (! (gls = kzalloc(sizeof(struct gpio_line_state), GFP_KERNEL))){
        message("Memory allocation failure");
        gpiod_put(desc); return -ENOMEM;
    }

    if ((rc = initialize_gls(gls, pdev, desc, of_prop))) return rc;

    message("Bound to device: '%s'", of_prop);
    return 0;
}

/*
 * platform devices will match if their compatible string
 * is listed here. See of_match_node in drivers/of/base.c and
 * drivers/base/platform.c fmi. */
static const struct of_device_id dt_comp_match_specs[] = {
        /* NOTE: dummy manufacturer */
        {.compatible = "vcstech,virtual_gpioman_device", .data = NULL},
        { },
};

/*
 * platform devices will match if their name in the device tree
 * is listed here. See platform_match in drivers/base/platform.c fmi. */
static const struct platform_device_id dt_name_match_specs[] = {
    {.name = "virtual_gpiomanager", .driver_data = 0},
    {}
};

/* Support hot-plugging; driver gets loaded automatically
 * when matching device is connected.
 * NOTE: Of course, the devices here are virtual, so this
 * is useless in this case. Only kept for reference. */
MODULE_DEVICE_TABLE(of, dt_comp_match_specs);

static struct platform_driver gpioman_driver  = {
    .probe    = gpio_probe_func,
    .remove   = rm_func,
    .id_table = dt_name_match_specs,
    .driver   = {
        .name = "gpioman-driver",
        .of_match_table = dt_comp_match_specs,
        .owner = THIS_MODULE
    }
};

static int __init initialize(void) {
	int rc;
    UNUSED(debug_mode_sysfs_toggle); /* see comup ment up above */

    message("module loaded");

    /* create a simple kobject and place it in sysfs under sys/kernel;
     * NOTE: set 2nd param to NULL to place directory directly under sys/
     * instead. */
	driver_sysfs_entry = kobject_create_and_add(DRIVER_SYSFS_DIRNAME, kernel_kobj);
	if (!driver_sysfs_entry){
        message("failed to create sysfs driver directory");
        return -ENOMEM;
    }

    if ((rc = platform_driver_register(&gpioman_driver))){
        message("Failed to register driver (%d)", rc);
        kobject_put(driver_sysfs_entry); driver_sysfs_entry = NULL;
    }

	return rc;
}

static void __exit cleanup(void) {
    struct gpio_line_state *gls;

    if (driver_sysfs_entry) kobject_put(driver_sysfs_entry);
    platform_driver_unregister(&gpioman_driver);

    list_for_each_entry(gls, &list, list){
        /* trigger the release() callback of each gls instance
         * still present */
        kobject_put(&gls->kobj);
    }

    message("module unloaded");
}

module_init(initialize);
module_exit(cleanup);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("vcsaturninus <vcsaturninus@protonmail.com>");
MODULE_DESCRIPTION("Linux kernel GPIO line manager");
MODULE_INFO(detail, "Basic GPIO consumer reference module");
