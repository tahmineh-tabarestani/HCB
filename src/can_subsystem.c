#include "can_subsystem.h"

#include <zephyr.h>
#include <kernel.h>
#include <sys/printk.h>
#include <device.h>
#include <drivers/can.h>
#include <drivers/gpio.h>
#include <sys/byteorder.h>

#define RX_THREAD_STACK_SIZE	     512
#define RX_THREAD_PRIORITY	     2
#define STATE_POLL_THREAD_STACK_SIZE 512
#define STATE_POLL_THREAD_PRIORITY   2
#define LED_MSG_ID		     0x10
#define COUNTER_MSG_ID		     0x12345
#define SET_LED			     1
#define RESET_LED		     0
#define SLEEP_TIME		     K_MSEC(250)

#define FT_ID 0x1b0

K_THREAD_STACK_DEFINE(rx_thread_stack, RX_THREAD_STACK_SIZE);
K_THREAD_STACK_DEFINE(poll_state_stack, STATE_POLL_THREAD_STACK_SIZE);

const struct device *can_dev;
const struct device *led_gpio_dev;

struct k_thread rx_thread_data;
struct k_thread poll_state_thread_data;
struct zcan_work rx_work;
struct k_work state_change_work;
enum can_state current_state;
struct can_bus_err_cnt current_err_cnt;

#ifdef CANFESTIVAL_ZEPHYR_DEBUG
static void print_frame(struct zcan_frame *frame)
{
	printk("|0x%3x|%s|%d|", frame->id, frame->rtr ? "RTR" : "   ",
	       frame->dlc);

	for (int i = 0; i < CAN_MAX_DLEN; i++) {
		if (i < frame->dlc) {
			printk(" 0x%02x", frame->data[i]);
		} else {
			printk("     ");
		}
	}

	printk("|\n");
}
#endif

void tx_irq_callback(uint32_t error_flags, void *arg)
{
	char *sender = (char *)arg;

	if (error_flags) {
		printk("Callback! error-code: %d\nSender: %s\n", error_flags,
		       sender);
	}
}

K_MUTEX_DEFINE(ftMutex);
static int32_t forces[3];
static int32_t torques[3];

/**
 * @brief Get the adc values object
 * 
 * @param dst 
 */
void get_ft_values(int32_t *dst)
{
	k_mutex_lock(&ftMutex, K_FOREVER);
	memcpy(dst, forces, sizeof(forces));
	memcpy(dst+3, torques, sizeof(torques));
	k_mutex_unlock(&ftMutex);
}

void receive_change_led(struct zcan_frame *frame, void *unused)
{
	ARG_UNUSED(unused);

	for (int i = 1; i < 4; ++i)
	{
		if (frame->id == (FT_ID + i)) {
			k_mutex_lock(&ftMutex, K_FOREVER);
			uint32_t f = frame->data_32[0];
			forces[i - 1] = (int32_t)__bswap_32(f);
			uint32_t t = frame->data_32[1];
			torques[i - 1] = (int32_t)__bswap_32(t);
			k_mutex_unlock(&ftMutex);
		}
	}

#ifdef CANFESTIVAL_ZEPHYR_DEBUG
	printk(" CAN_RECEIVE: ");
	print_frame(frame);
#endif
}

char *state_to_str(enum can_state state)
{
	switch (state) {
	case CAN_ERROR_ACTIVE:
		return "error-active";
	case CAN_ERROR_PASSIVE:
		return "error-passive";
	case CAN_BUS_OFF:
		return "bus-off";
	default:
		return "unknown";
	}
}

void poll_state_thread(void *unused1, void *unused2, void *unused3)
{
	struct can_bus_err_cnt err_cnt = { 0, 0 };
	struct can_bus_err_cnt err_cnt_prev = { 0, 0 };
	enum can_state state_prev = CAN_ERROR_ACTIVE;
	enum can_state state;

	while (1) {
		state = can_get_state(can_dev, &err_cnt);
		if (err_cnt.tx_err_cnt != err_cnt_prev.tx_err_cnt ||
		    err_cnt.rx_err_cnt != err_cnt_prev.rx_err_cnt ||
		    state_prev != state) {
			err_cnt_prev.tx_err_cnt = err_cnt.tx_err_cnt;
			err_cnt_prev.rx_err_cnt = err_cnt.rx_err_cnt;
			state_prev = state;
			printk("state: %s\n"
			       "rx error count: %d\n"
			       "tx error count: %d\n",
			       state_to_str(state), err_cnt.rx_err_cnt,
			       err_cnt.tx_err_cnt);
		} else {
			k_sleep(K_MSEC(100));
		}
	}
}

void state_change_work_handler(struct k_work *work)
{
	printk("State Change ISR\nstate: %s\n"
	       "rx error count: %d\n"
	       "tx error count: %d\n",
	       state_to_str(current_state), current_err_cnt.rx_err_cnt,
	       current_err_cnt.tx_err_cnt);

#ifndef CONFIG_CAN_AUTO_BUS_OFF_RECOVERY
	if (current_state == CAN_BUS_OFF) {
		printk("Recover from bus-off\n");

		if (can_recover(can_dev, K_MSEC(100) != 0)) {
			printk("Recovery timed out\n");
		}
	}
#endif /* CONFIG_CAN_AUTO_BUS_OFF_RECOVERY */
}

void state_change_isr(enum can_state state, struct can_bus_err_cnt err_cnt)
{
	current_state = state;
	current_err_cnt = err_cnt;
	k_work_submit(&state_change_work);
}

void main_can_ati_thread(void *_1, void *_2, void *_3)
{
	static const struct zcan_filter my_filter = {
		.id_type = CAN_STANDARD_IDENTIFIER,
		.rtr = CAN_DATAFRAME,
		.id = 0x1,
		.rtr_mask = 0,
		.id_mask = 0x0
	};

	struct zcan_frame req_long_data_frame = {
		.id_type = CAN_STANDARD_IDENTIFIER,
		.rtr = CAN_DATAFRAME,
		.id = FT_ID,
		.dlc = 1
	};
	uint8_t toggle = 1;
	uint16_t counter = 0;
	k_tid_t rx_tid, get_state_tid;
	int ret;

	can_dev = device_get_binding(DT_CHOSEN_ZEPHYR_CAN_PRIMARY_LABEL);

	if (!can_dev) {
		printk("CAN: Device driver not found.\n");
		return;
	}

	k_work_init(&state_change_work, state_change_work_handler);

	ret = can_attach_workq(can_dev, &k_sys_work_q, &rx_work,
			       receive_change_led, NULL, &my_filter);
	if (ret == CAN_NO_FREE_FILTER) {
		printk("Error, no filter available!\n");
		return;
	}

	printk("Change LED filter ID: %d\n", ret);
	get_state_tid =
		k_thread_create(&poll_state_thread_data, poll_state_stack,
				K_THREAD_STACK_SIZEOF(poll_state_stack),
				poll_state_thread, NULL, NULL, NULL,
				STATE_POLL_THREAD_PRIORITY, 0, K_NO_WAIT);
	if (!get_state_tid) {
		printk("ERROR spawning poll_state_thread\n");
	}

	can_register_state_change_isr(can_dev, state_change_isr);

	printk("Finished init.\n");

	while (1) {
		req_long_data_frame.data[0] = 0x01;
		/* This sending call is none blocking. */
		can_send(can_dev, &req_long_data_frame, K_FOREVER,
			 tx_irq_callback, "LED change");
		k_sleep(SLEEP_TIME);
	}
}

// Thread Definition

#define CANATI_STACK_SIZE 512
#define CANATI_PRIORITY	  5

K_THREAD_DEFINE(CANATI, CANATI_STACK_SIZE, main_can_ati_thread, NULL, NULL,
		NULL, CANATI_PRIORITY, 0, 0);
