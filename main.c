/* kxo: A Tic-Tac-Toe Game Engine implemented as Linux kernel module */

#include <linux/cdev.h>
#include <linux/circ_buf.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/kfifo.h>
#include <linux/module.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/version.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

#include "game.h"
#include "load.h"
#include "mcts.h"
#include "negamax.h"
MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("National Cheng Kung University, Taiwan");
MODULE_DESCRIPTION("In-kernel Tic-Tac-Toe game engine");

/* Macro DECLARE_TASKLET_OLD exists for compatibility.
 * See https://lwn.net/Articles/830964/
 */
#ifndef DECLARE_TASKLET_OLD
#define DECLARE_TASKLET_OLD(arg1, arg2) DECLARE_TASKLET(arg1, arg2, 0L)
#endif

#define DEV_NAME "kxo"

#define NR_KMLDRV 1

static int delay = 100; /* time (in ms) to generate an event */
struct Load *load;      /*Create load to store the average load*/
/* Declare kernel module attribute for sysfs */

struct kxo_attr {
    char display;
    char resume;
    char end;
    rwlock_t lock;
};

static struct kxo_attr attr_obj;

static ssize_t kxo_state_show(struct device *dev,
                              struct device_attribute *attr,
                              char *buf)
{
    read_lock(&attr_obj.lock);
    int ret = snprintf(buf, 6, "%c %c %c\n", attr_obj.display, attr_obj.resume,
                       attr_obj.end);
    read_unlock(&attr_obj.lock);
    return ret;
}

static ssize_t kxo_state_store(struct device *dev,
                               struct device_attribute *attr,
                               const char *buf,
                               size_t count)
{
    write_lock(&attr_obj.lock);
    sscanf(buf, "%c %c %c", &(attr_obj.display), &(attr_obj.resume),
           &(attr_obj.end));
    write_unlock(&attr_obj.lock);
    return count;
}

static DEVICE_ATTR_RW(kxo_state);

/* Data produced by the simulated device */

/* Timer to simulate a periodic IRQ */
static struct timer_list timer;

/* Character device stuff */
static int major;
static struct class *kxo_class;
static struct cdev kxo_cdev;

/*
 * Use an int to store the table state. Each grid is represented by
 * two bits. With 16 grids in total, a 32-bit int can store the entire
 * table efficiently.
 * 00: ""
 * 01: "X"
 * 10: "O"
 */
static game_state *game_state_1;
static game_state *game_state_2;
static char load_buf[LOAD_SIZE];
static struct tasklet_struct game_tasklet1;
static struct tasklet_struct game_tasklet2;
/* Workqueue for asynchronous bottom-half processing */
static struct workqueue_struct *kxo_workqueue_1, *kxo_workqueue_2;

/* Data are stored into a kfifo buffer before passing them to the userspace */
static DECLARE_KFIFO_PTR(rx_fifo, unsigned char);

/* NOTE: the usage of kfifo is safe (no need for extra locking), until there is
 * only one concurrent reader and one concurrent writer. Writes are serialized
 * from the interrupt context, readers are serialized using this mutex.
 */
static DEFINE_MUTEX(read_lock);

/* Wait queue to implement blocking I/O from userspace */
static DECLARE_WAIT_QUEUE_HEAD(rx_wait);

/* Insert the whole chess board into the kfifo buffer */
static void produce_board(void)
{
    int str_len = snprintf(
        load_buf, sizeof(load_buf), "1min: %10llu 5min: %10llu 15min: %10llu",
        load->one_min_load, load->five_min_load, load->fifteen_min_load);

    if (str_len < 0) {
        pr_warn_ratelimited("snprintf error\n");
        return;
    }
    unsigned int len = kfifo_in(&rx_fifo, load_buf, str_len);
    // Push the load buffer into fifo
    if (unlikely(len < str_len))
        pr_warn_ratelimited("%s: %d bytes dropped\n", __func__, str_len - len);
    // Push the game1's table into fifo
    u32 compressed_table = table_compressor(game_state_1->table);
    len = kfifo_in(&rx_fifo, (u8 *) &compressed_table, sizeof(u32));
    if (unlikely(len < sizeof(game_state_1->table)))
        pr_warn_ratelimited("%s: %zu bytes dropped\n", __func__,
                            sizeof(game_state_1->table) - len);
    // Push the game2's table into fifo
    compressed_table = table_compressor(game_state_2->table);
    len = kfifo_in(&rx_fifo, (u8 *) &compressed_table, sizeof(u32));
    if (unlikely(len < sizeof(game_state_2->table)))
        pr_warn_ratelimited("%s: %zu bytes dropped\n", __func__,
                            sizeof(game_state_2->table) - len);
    pr_debug("kxo: %s: in %u/%u bytes\n", __func__, len, kfifo_len(&rx_fifo));
}

/* Mutex to serialize kfifo writers within the workqueue handler */
static DEFINE_MUTEX(producer_lock);

/* Mutex to serialize fast_buf consumers: we can use a mutex because consumers
 * run in workqueue handler (kernel thread context).
 */
static DEFINE_MUTEX(consumer_lock);

/* We use an additional "faster" circular buffer to quickly store data from
 * interrupt context, before adding them to the kfifo.
 */
static struct circ_buf fast_buf;

/* Clear all data from the circular buffer fast_buf */
static void fast_buf_clear(void)
{
    fast_buf.head = fast_buf.tail = 0;
}

/* Workqueue handler: executed by a kernel thread */
static void drawboard_work_func(struct work_struct *w)
{
    int cpu;

    /* This code runs from a kernel thread, so softirqs and hard-irqs must
     * be enabled.
     */
    WARN_ON_ONCE(in_softirq());
    WARN_ON_ONCE(in_interrupt());

    /* Pretend to simulate access to per-CPU data, disabling preemption
     * during the pr_info().
     */
    cpu = get_cpu();
    pr_info("kxo: [CPU#%d] %s\n", cpu, __func__);
    put_cpu();

    read_lock(&attr_obj.lock);
    if (attr_obj.display == '0') {
        read_unlock(&attr_obj.lock);
        return;
    }
    read_unlock(&attr_obj.lock);

    /* Store data to the kfifo buffer */
    mutex_lock(&consumer_lock);
    produce_board();
    mutex_unlock(&consumer_lock);

    wake_up_interruptible(&rx_wait);
}

static void ai_one_work_func(struct work_struct *w)
{
    ktime_t tv_start, tv_end;
    s64 nsecs;

    int cpu;

    struct ai_work *ai = container_of(w, struct ai_work, work);
    game_state *state = (ai->game_num == 1) ? game_state_1 : game_state_2;
    READ_ONCE(state);
    /* If the turn is not correct, then leave the work */
    if (state->turn != 'O') {
        pr_info("kxo: AI_%d work on incorrect turn: %c\n", ai->game_num,
                state->turn);
        WRITE_ONCE(state->finish, 1);
        kfree(ai);
        return;
    }

    WARN_ON_ONCE(in_softirq());
    WARN_ON_ONCE(in_interrupt());

    cpu = get_cpu();
    pr_info("kxo: [CPU#%d] start doing %s\n", cpu, __func__);
    tv_start = ktime_get();
    mutex_lock(&producer_lock);
    int move;

    WRITE_ONCE(move, mcts(state->table, 'O'));

    smp_mb();

    if (move != -1) {
        WRITE_ONCE(state->table[move], 'O');
    }
    WRITE_ONCE(state->turn, 'X');
    WRITE_ONCE(state->finish, 1);
    if (ai->game_num == 1)
        pr_info("State for game1 (O) %s\n", game_state_1->table);
    smp_wmb();
    mutex_unlock(&producer_lock);
    tv_end = ktime_get();

    nsecs = (s64) ktime_to_ns(ktime_sub(tv_end, tv_start));
    pr_info("kxo: [CPU#%d] %s completed in %llu usec (game %d)\n", cpu,
            __func__, (unsigned long long) nsecs >> 10, ai->game_num);
    renew_load(load, (unsigned long long) nsecs >> 10);
    put_cpu();
    kfree(ai);
}

static void ai_two_work_func(struct work_struct *w)
{
    ktime_t tv_start, tv_end;
    s64 nsecs;

    int cpu;

    struct ai_work *ai = container_of(w, struct ai_work, work);
    game_state *state = (ai->game_num == 1) ? game_state_1 : game_state_2;
    READ_ONCE(state);
    /* If the turn is not correct, then leave the work */
    if (state->turn != 'X') {
        pr_info("kxo: AI_%d work on incorrect turn: %c\n", ai->game_num,
                state->turn);
        WRITE_ONCE(state->finish, 1);
        kfree(ai);
        return;
    }
    WARN_ON_ONCE(in_softirq());
    WARN_ON_ONCE(in_interrupt());

    cpu = get_cpu();
    pr_info("kxo: [CPU#%d] start doing %s\n", cpu, __func__);
    tv_start = ktime_get();
    mutex_lock(&producer_lock);
    int move;
    WRITE_ONCE(move, negamax_predict(state->table, 'X').move);

    smp_mb();

    if (move != -1) {
        WRITE_ONCE(state->table[move], 'X');
    }

    WRITE_ONCE(state->turn, 'O');
    WRITE_ONCE(state->finish, 1);
    if (ai->game_num == 1)
        pr_info("State for game1 (X) %s\n", game_state_1->table);
    smp_wmb();
    mutex_unlock(&producer_lock);
    tv_end = ktime_get();

    nsecs = (s64) ktime_to_ns(ktime_sub(tv_end, tv_start));
    pr_info("kxo: [CPU#%d] %s completed in %llu usec (game %d)\n", cpu,
            __func__, (unsigned long long) nsecs >> 10, ai->game_num);
    put_cpu();
    kfree(ai);
}

/* Work item: holds a pointer to the function that is going to be executed
 * asynchronously.
 */
static DECLARE_WORK(drawboard_work, drawboard_work_func);
/* Tasklet handler.
 *
 * NOTE: different tasklets can run concurrently on different processors, but
 * two of the same type of tasklet cannot run simultaneously. Moreover, a
 * tasklet always runs on the same CPU that schedules it.
 */
static void game_tasklet_func(struct tasklet_struct *t)
{
    /* Fetech game number */
    unsigned long game_num = t->data;
    /* malloc a work struct */
    struct ai_work *ai = kmalloc(sizeof(*ai), GFP_ATOMIC);
    if (!ai)
        return;

    READ_ONCE(game_state_1);
    READ_ONCE(game_state_2);
    game_state *state = (game_num == 1) ? game_state_1 : game_state_2;
    ai->game_num = game_num;
    READ_ONCE(state->turn);
    if (state->turn == 'O') {
        INIT_WORK(&ai->work, ai_one_work_func);
    } else {
        INIT_WORK(&ai->work, ai_two_work_func);
    }

    /* Add the ai_work to kxo_workqueue */
    if (game_num == 1)
        queue_work(kxo_workqueue_1, &ai->work);
    else
        queue_work(kxo_workqueue_2, &ai->work);



    ktime_t tv_start, tv_end;
    s64 nsecs;

    WARN_ON_ONCE(!in_interrupt());
    WARN_ON_ONCE(!in_softirq());

    tv_start = ktime_get();

    READ_ONCE(state);
    READ_ONCE(state->finish);
    READ_ONCE(state->turn);
    smp_rmb();

    if (state->finish && (state->turn == 'O' || state->turn == 'X')) {
        WRITE_ONCE(state->finish, 0);
        smp_wmb();
    }


    if (game_num == 1)
        queue_work(kxo_workqueue_1, &drawboard_work);
    else
        queue_work(kxo_workqueue_2, &drawboard_work);

    tv_end = ktime_get();

    nsecs = (s64) ktime_to_ns(ktime_sub(tv_end, tv_start));
    /* SoftIRQ means interrupt by software */
    pr_info("kxo: [CPU#%d] %s in_softirq: %llu usec\n", smp_processor_id(),
            __func__, (unsigned long long) nsecs >> 10);
}

static void ai_game(int game_id)
{
    WARN_ON_ONCE(!irqs_disabled());

    pr_info("kxo: [CPU#%d] scheduling tasklet\n", smp_processor_id());
    if (game_id == 1) {
        tasklet_schedule(&game_tasklet1);
        pr_info("kxo: [CPU#%d] doing AI game 1\n", smp_processor_id());
    }
    if (game_id == 2) {
        tasklet_schedule(&game_tasklet2);
        pr_info("kxo: [CPU#%d] doing AI game\n", smp_processor_id());
    }
}

static void timer_handler(struct timer_list *__timer)
{
    ktime_t tv_start, tv_end;
    s64 nsecs;

    pr_info("kxo: [CPU#%d] enter %s\n", smp_processor_id(), __func__);
    /* We are using a kernel timer to simulate a hard-irq, so we must expect
     * to be in softirq context here.
     */
    WARN_ON_ONCE(!in_softirq());

    /* Disable interrupts for this CPU to simulate real interrupt context */
    local_irq_disable();

    tv_start = ktime_get();
    READ_ONCE(game_state_1);
    READ_ONCE(game_state_2);
    char win_1 = check_win(game_state_1->table);
    char win_2 = check_win(game_state_2->table);

    if ((win_1 == ' ') || (win_2 == ' ')) {
        if (win_1 == ' ')
            ai_game(1);
        if (win_2 == ' ')
            ai_game(2);
        mod_timer(&timer, jiffies + msecs_to_jiffies(delay));
    } else {
        read_lock(&attr_obj.lock);
        if (attr_obj.display == '1') {
            int cpu = get_cpu();
            pr_info("kxo: [CPU#%d] Drawing final board\n", cpu);
            put_cpu();

            /* Store data to the kfifo buffer */
            mutex_lock(&consumer_lock);
            produce_board();
            mutex_unlock(&consumer_lock);

            wake_up_interruptible(&rx_wait);
        }
        /* Reset the gmae table for two games */
        if (attr_obj.end == '0') {
            if (win_1 != ' ')
                init_game_state(game_state_1);
            if (win_2 != ' ')
                init_game_state(game_state_2);
            mod_timer(&timer, jiffies + msecs_to_jiffies(delay));
        }

        read_unlock(&attr_obj.lock);

        if (win_1 != ' ')
            pr_info("game1: kxo: %c win!!!\n", win_1);
        if (win_2 != ' ')
            pr_info("game2: kxo: %c win!!!\n", win_2);
    }
    tv_end = ktime_get();

    nsecs = (s64) ktime_to_ns(ktime_sub(tv_end, tv_start));

    pr_info("kxo: [CPU#%d] %s in_irq: %llu usec\n", smp_processor_id(),
            __func__, (unsigned long long) nsecs >> 10);

    local_irq_enable();
}

static ssize_t kxo_read(struct file *file,
                        char __user *buf,
                        size_t count,
                        loff_t *ppos)
{
    unsigned int read;
    int ret;

    pr_debug("kxo: %s(%p, %zd, %lld)\n", __func__, buf, count, *ppos);

    if (unlikely(!access_ok(buf, count)))
        return -EFAULT;

    if (mutex_lock_interruptible(&read_lock))
        return -ERESTARTSYS;

    do {
        ret = kfifo_to_user(&rx_fifo, buf, count, &read);
        if (unlikely(ret < 0))
            break;
        if (read)
            break;
        if (file->f_flags & O_NONBLOCK) {
            ret = -EAGAIN;
            break;
        }
        ret = wait_event_interruptible(rx_wait, kfifo_len(&rx_fifo));
    } while (ret == 0);
    pr_debug("kxo: %s: out %u/%u bytes\n", __func__, read, kfifo_len(&rx_fifo));

    mutex_unlock(&read_lock);

    return ret ? ret : read;
}

static atomic_t open_cnt;

static int kxo_open(struct inode *inode, struct file *filp)
{
    pr_debug("kxo: %s\n", __func__);
    if (atomic_inc_return(&open_cnt) == 1)
        mod_timer(&timer, jiffies + msecs_to_jiffies(delay));
    pr_info("openm current cnt: %d\n", atomic_read(&open_cnt));

    return 0;
}

static int kxo_release(struct inode *inode, struct file *filp)
{
    pr_debug("kxo: %s\n", __func__);
    if (atomic_dec_and_test(&open_cnt)) {
        del_timer_sync(&timer);
        flush_workqueue(kxo_workqueue_1);
        flush_workqueue(kxo_workqueue_2);
        fast_buf_clear();
    }
    pr_info("release, current cnt: %d\n", atomic_read(&open_cnt));
    attr_obj.end = 48;

    return 0;
}

static const struct file_operations kxo_fops = {
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0)
    .owner = THIS_MODULE,
#endif
    .read = kxo_read,
    .llseek = no_llseek,
    .open = kxo_open,
    .release = kxo_release,
};

static int __init kxo_init(void)
{
    dev_t dev_id;
    int ret;
    /* Initialize the game state */
    game_state_1 = kmalloc(sizeof(game_state), GFP_KERNEL);
    game_state_2 = kmalloc(sizeof(game_state), GFP_KERNEL);
    init_game_state(game_state_1);
    init_game_state(game_state_2);
    /* Setup two tasklets for two independent games */
    tasklet_setup(&game_tasklet1, game_tasklet_func);
    tasklet_setup(&game_tasklet2, game_tasklet_func);
    /* Setup the variable that is passing to the tasklet */
    game_tasklet1.data = 1;
    game_tasklet2.data = 2;

    if (!game_state_1) {
        pr_err("Failed to allocate memory for game states 1\n");
        kfree(game_state_1);
        return -ENOMEM;
    }

    if (!game_state_2) {
        pr_err("Failed to allocate memory for game states 2\n");
        kfree(game_state_2);
        return -ENOMEM;
    }


    /* Initialize the average load data structure*/
    load = kmalloc(sizeof(struct Load), GFP_KERNEL);
    if (!load) {
        pr_info("Failed to allocate memory for Load!\n");
        goto error_alloc;
    }
    init_load(load); /*Initialize the structure*/

    if (kfifo_alloc(&rx_fifo, PAGE_SIZE, GFP_KERNEL) < 0)
        return -ENOMEM;

    /* Register major/minor numbers */
    ret = alloc_chrdev_region(&dev_id, 0, NR_KMLDRV, DEV_NAME);
    if (ret)
        goto error_alloc;
    major = MAJOR(dev_id);

    /* Add the character device to the system */
    cdev_init(&kxo_cdev, &kxo_fops);
    ret = cdev_add(&kxo_cdev, dev_id, NR_KMLDRV);
    if (ret) {
        kobject_put(&kxo_cdev.kobj);
        goto error_region;
    }

    /* Create a class structure */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0)
    kxo_class = class_create(THIS_MODULE, DEV_NAME);
#else
    kxo_class = class_create(DEV_NAME);
#endif
    if (IS_ERR(kxo_class)) {
        printk(KERN_ERR "error creating kxo class\n");
        ret = PTR_ERR(kxo_class);
        goto error_cdev;
    }

    /* Register the device with sysfs */
    struct device *kxo_dev =
        device_create(kxo_class, NULL, MKDEV(major, 0), NULL, DEV_NAME);

    ret = device_create_file(kxo_dev, &dev_attr_kxo_state);
    if (ret < 0) {
        printk(KERN_ERR "failed to create sysfs file kxo_state\n");
        goto error_device;
    }

    /* Allocate fast circular buffer */
    fast_buf.buf = vmalloc(PAGE_SIZE);
    if (!fast_buf.buf) {
        ret = -ENOMEM;
        goto error_vmalloc;
    }

    /* Create the workqueue */
    kxo_workqueue_1 = alloc_workqueue("kxod_1", WQ_UNBOUND, WQ_MAX_ACTIVE);
    if (!kxo_workqueue_1) {
        ret = -ENOMEM;
        goto error_workqueue;
    }

    kxo_workqueue_2 = alloc_workqueue("kxod_2", WQ_UNBOUND, WQ_MAX_ACTIVE);
    if (!kxo_workqueue_2) {
        ret = -ENOMEM;
        goto error_workqueue;
    }

    negamax_init();
    mcts_init();

    attr_obj.display = '1';
    attr_obj.resume = '1';
    attr_obj.end = '0';
    rwlock_init(&attr_obj.lock);
    /* Setup the timer */
    timer_setup(&timer, timer_handler, 0);
    atomic_set(&open_cnt, 0);

    pr_info("kxo: registered new kxo device: %d,%d\n", major, 0);
out:
    return ret;
error_workqueue:
    vfree(fast_buf.buf);
error_vmalloc:
    device_destroy(kxo_class, dev_id);
error_device:
    class_destroy(kxo_class);
error_cdev:
    cdev_del(&kxo_cdev);
error_region:
    unregister_chrdev_region(dev_id, NR_KMLDRV);
error_alloc:
    kfifo_free(&rx_fifo);
    kfree(load);
    goto out;
}

static void __exit kxo_exit(void)
{
    dev_t dev_id = MKDEV(major, 0);

    del_timer_sync(&timer);
    tasklet_kill(&game_tasklet1);
    tasklet_kill(&game_tasklet2);
    flush_workqueue(kxo_workqueue_1);
    destroy_workqueue(kxo_workqueue_1);
    flush_workqueue(kxo_workqueue_2);
    destroy_workqueue(kxo_workqueue_2);
    vfree(fast_buf.buf);
    device_destroy(kxo_class, dev_id);
    class_destroy(kxo_class);
    cdev_del(&kxo_cdev);
    unregister_chrdev_region(dev_id, NR_KMLDRV);

    kfifo_free(&rx_fifo);
    pr_info("kxo: unloaded\n");
}

module_init(kxo_init);
module_exit(kxo_exit);
