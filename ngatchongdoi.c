#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/jiffies.h>

#define DEVICE_NAME "led_seq" //Name module
#define LED_COUNT 3
#define BUTTON_GPIO 4 // Set the GPIO pin 4 or 15 for the button

// Set the GPIO pin for 3 LEDs
static int led_pins[LED_COUNT] = {17, 27, 22};

static struct class *led_class;
static struct cdev led_cdev;
static dev_t dev_num;
static int led_seq_running = 0; //Sequential lighting state variable
static int button_irq;
static unsigned long last_jiffies; // Variable debounceTime 
static int button_state = 1; // Previous state of the button (1 = released, 0 = pressed)
       int i;
#define DEBOUNCE_DELAY_MS 20 // Debounce delay in milliseconds

static int led_open(struct inode *inode, struct file *file) {
    printk(KERN_INFO "LED device opened\n");
    return 0;
}

static int led_release(struct inode *inode, struct file *file) {
    printk(KERN_INFO "LED device closed\n");
    return 0;
}

static ssize_t led_write(struct file *file, const char __user *buf, size_t len, loff_t *off) {
    char command[16];

    if (len > sizeof(command) - 1)
        return -EINVAL;

    if (copy_from_user(command, buf, len))
        return -EFAULT;

    command[len] = '\0';

    // Start or stop the LED sequence
    if (strncmp(command, "start", 5) == 0) {
        led_seq_running = 1;
        printk(KERN_INFO "LED sequence started\n");
    } else if (strncmp(command, "stop", 4) == 0) {
        led_seq_running = 0;
        printk(KERN_INFO "LED sequence stopped\n");
    } else {
        printk(KERN_WARNING "Invalid command\n");
        return -EINVAL;
    }

    return len;
}

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = led_open,
    .release = led_release,
    .write = led_write,
};

// Thread function to handle LED sequence
static int led_seq_thread(void *data) {
    while (!kthread_should_stop()) {
        if (led_seq_running) {
            for (i = 0; i < LED_COUNT; i++) {
                gpio_set_value(led_pins[i], 1);  // Turn on the LED
                msleep(500);                    // Delay 500ms
                gpio_set_value(led_pins[i], 0);  // Turn off the LED
            }
        } else {
            msleep(100); // Wait for a moment when not running sequentially
        }
    }

    return 0;
}

static struct task_struct *led_task;

// ISR (Interrupt Service Routine) for the button
static irqreturn_t button_irq_handler(int irq, void *dev_id) {
    unsigned long current_jiffies = jiffies;
    if (time_before(current_jiffies, last_jiffies + msecs_to_jiffies(DEBOUNCE_DELAY_MS))) {
        return IRQ_HANDLED;
    }
    last_jiffies = current_jiffies;

    // Check the current state of the button
    if (gpio_get_value(BUTTON_GPIO) == 0) { // Button pressed
        if (button_state == 1) { // Previously released, now pressed down
            printk(KERN_INFO "Button pressed, turning off LEDs\n");

            //Turn off all LEDs
            for (i = 0; i < LED_COUNT; i++) {
                gpio_set_value(led_pins[i], 0);
            }

            led_seq_running = 0; // Stop the LED sequence
            button_state = 0;    // Update the button state
        }
    } else if (gpio_get_value(BUTTON_GPIO) == 1) { // The button is released
        if (button_state == 0) { // Previously pressed, now released
            printk(KERN_INFO "Button released, resuming LED sequence\n");
            led_seq_running = 1; // Continue the LED sequence
            button_state = 1;    // Update the button state
        }
    }

    return IRQ_HANDLED;
}

static int __init led_init(void) {
    int ret;

    // Allocate the device number
    ret = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        printk(KERN_ALERT "Failed to allocate device number\n");
        return ret;
    }

    //Create a device class
    led_class = class_create(THIS_MODULE, DEVICE_NAME);
    if (IS_ERR(led_class)) {
        unregister_chrdev_region(dev_num, 1);
        return PTR_ERR(led_class);
    }

    // Initialize cdev structure and add to system
    cdev_init(&led_cdev, &fops);
    led_cdev.owner = THIS_MODULE;
    ret = cdev_add(&led_cdev, dev_num, 1);
    if (ret < 0) {
        class_destroy(led_class);
        unregister_chrdev_region(dev_num, 1);
        return ret;
    }

    // Create device file
    device_create(led_class, NULL, dev_num, NULL, DEVICE_NAME);

    // Request GPIO and configure output
    for (i = 0; i < LED_COUNT; i++) {
        if (gpio_request_one(led_pins[i], GPIOF_OUT_INIT_LOW, NULL)) {
            printk(KERN_ALERT "Failed to request GPIO %d\n", led_pins[i]);
            return -1;
        }
    }

    // Request GPIO for the button and configure input
    if (gpio_request_one(BUTTON_GPIO, GPIOF_IN, "button_gpio")) {
        printk(KERN_ALERT "Failed to request GPIO %d\n", BUTTON_GPIO);
        return -1;
    }

    // Request an IRQ for the button
    button_irq = gpio_to_irq(BUTTON_GPIO);
    if (button_irq < 0) {
        printk(KERN_ALERT "Failed to get IRQ number for GPIO %d\n", BUTTON_GPIO);
        return button_irq;
    }

    ret = request_irq(button_irq, button_irq_handler, IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING, "button_irq", NULL);
    if (ret) {
        printk(KERN_ALERT "Failed to request IRQ\n");
        return ret;
    }

    // Start LED sequence thread
    led_task = kthread_run(led_seq_thread, NULL, "led_seq_thread");

    printk(KERN_INFO "LED driver initialized\n");
    return 0;
}

static void __exit led_exit(void) {

    // Stop LED sequence thread
    if (led_task) {
        kthread_stop(led_task);
    }

    // Free the IRQ for the button
    free_irq(button_irq, NULL);

    // Free the GPIO for the button
    gpio_free(BUTTON_GPIO);

    // Free GPIO pins for LEDs
    for (i = 0; i < LED_COUNT; i++) {
        gpio_set_value(led_pins[i], 0);
        gpio_free(led_pins[i]);
    }

    // Destroy device file and class
    device_destroy(led_class, dev_num);
    class_destroy(led_class);
    cdev_del(&led_cdev);
    unregister_chrdev_region(dev_num, 1);

    printk(KERN_INFO "LED driver removed\n");
}

module_init(led_init);
module_exit(led_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Nguyen Vy <nvystudent@gmail.com>");
MODULE_DESCRIPTION("A driver to control 3 LEDs in a sequential lighting pattern with button interrupt handling and debounce for Raspberry Pi 3 Model B+");
