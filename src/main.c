#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include <zephyr/random/random.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <nrfx.h>
#include <string.h>

/* ---------------- Sensor frame ---------------- */
struct __packed sensor_frame_t {
    uint8_t  dev_id_3[3];   //hej 
    uint32_t date_hour;      
    int16_t  temp_x10;
    int16_t  hum_x10;
    uint16_t lux;
    uint32_t weight;
    uint16_t in_bees;
    uint16_t out_bees;
    uint8_t  stat;
    char     pp[2];          
};
static struct sensor_frame_t frame;

/* Manufacturer data */
static uint8_t mfg_data[2 + sizeof(frame)] = {0x59, 0x00};

/* BLE advertising data */
static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
    BT_DATA(BT_DATA_MANUFACTURER_DATA, mfg_data, sizeof(mfg_data)),
};
static const struct bt_data sd[] = {
    BT_DATA(BT_DATA_NAME_COMPLETE, "BeeNode", 7),
};

/* ---------------- Button ---------------- */
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET_OR(DT_ALIAS(sw0), gpios, {0});
static struct gpio_callback button_cb;
static volatile bool button_pressed = false;

static void button_pressed_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    button_pressed = true;
}

/* ---------------- Fake sensors ---------------- */
static double fake_temp(void)   { return 20.0 + (sys_rand32_get() % 1000)/100.0; }
static double fake_hum(void)    { return 50.0 + (sys_rand32_get() % 500)/10.0; }
static uint16_t fake_lux(void)  { return 100 + (sys_rand32_get() % 1000); }
static uint32_t fake_weight(void){ return 100000 + (sys_rand32_get() % 50000); }
static uint16_t fake_in(void)   { return sys_rand32_get() % 300; }
static uint16_t fake_out(void)  { return sys_rand32_get() % 300; }
static uint8_t  fake_stat(void) { return sys_rand32_get() % 2; }

/* ---------------- DevID ---------------- */
static void read_dev_id_24(uint8_t out[3])
{
    uint32_t d0 = NRF_FICR->DEVICEID[0];
    uint32_t d1 = NRF_FICR->DEVICEID[1];

    uint32_t dev24 = ((d1 & 0xFF) << 16) | (d0 & 0xFFFF);
    out[0] = (dev24 >> 16) & 0xFF;
    out[1] = (dev24 >> 8) & 0xFF;
    out[2] = dev24 & 0xFF;
}

/* ---------------- Update frame ---------------- */
static void update_sensor_frame(void)
{
    read_dev_id_24(frame.dev_id_3); 
    frame.date_hour = 0;
    frame.temp_x10 = (int16_t)(fake_temp() * 10);
    frame.hum_x10  = (int16_t)(fake_hum() * 10);
    frame.lux      = fake_lux();
    frame.weight   = fake_weight();
    frame.in_bees  = fake_in();
    frame.out_bees = fake_out();
    frame.stat     = fake_stat();
    frame.pp[0]    = 'P';
    frame.pp[1]    = 'P';

    memcpy(&mfg_data[2], &frame, sizeof(frame));

    printk("FRAME DUMP: temp=%d, hum=%d, lux=%u, weight=%u\n",
           frame.temp_x10, frame.hum_x10, frame.lux, frame.weight);
}

/* ---------------- BLE callbacks ---------------- */
static struct bt_conn *current_conn;

static void connected(struct bt_conn *conn, uint8_t err)
{
    if (!err) {
        printk("Connected\n");
        current_conn = bt_conn_ref(conn);
    }
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    printk("Disconnected: %u\n", reason);
    if (current_conn) {
        bt_conn_unref(current_conn);
        current_conn = NULL;
    }
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
};

/* ----------------- Advertising burst ----------------- */
#define BURST_MS 60000
#define ADV_UPDATE_MS 150
static struct k_work_delayable adv_work;
static int adv_remaining;

static void adv_burst_work(struct k_work *w)
{
    if (adv_remaining <= 0) return;

    update_sensor_frame();

    int err = bt_le_adv_update_data(ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
    if (err && err != -EAGAIN)
        printk("adv update failed: %d\n", err);

    adv_remaining -= ADV_UPDATE_MS;
    if (adv_remaining > 0)
        k_work_schedule(&adv_work, K_MSEC(ADV_UPDATE_MS));
}

/* --------------------------------------------------------- */
/*               SYSTEM ON ULTRA LOW POWER SLEEP             */
/*                 RTC1 → wake-up after N seconds            */
/* --------------------------------------------------------- */

static volatile bool rtc_wakeup_flag = false;

void RTC1_IRQHandler(void)
{
    if (NRF_RTC1->EVENTS_COMPARE[0]) {
        NRF_RTC1->EVENTS_COMPARE[0] = 0;
        rtc_wakeup_flag = true;
        __SEV();   // wake from WFE
    }

    printk("RTC IRQ\n");
}

static void system_on_sleep_seconds(uint32_t seconds)
{


    printk("Entering tak ULTRA LOW POWER for %u s...\n", seconds);

    /* Start low frequency clock required by RTC1 */
    NRF_CLOCK->TASKS_LFCLKSTART = 1;
    while (!NRF_CLOCK->EVENTS_LFCLKSTARTED);
    printk("LFCLK started: %u\n", (uint32_t)NRF_CLOCK->EVENTS_LFCLKSTARTED);
    NRF_CLOCK->EVENTS_LFCLKSTARTED = 0;

    bt_le_adv_stop();
    while (NRF_RADIO->STATE != RADIO_STATE_STATE_Disabled) {
        k_sleep(K_MSEC(1));
    }
    k_work_cancel_delayable(&adv_work);
    k_sleep(K_MSEC(10)); // give radio time to stop

    rtc_wakeup_flag = false;
    NRF_RTC1->TASKS_STOP = 1;
    NRF_RTC1->TASKS_CLEAR = 1;
    NRF_RTC1->PRESCALER = 0;

    uint32_t ticks = 32768UL * seconds;
    NRF_RTC1->CC[0] = ticks & 0x00FFFFFF; // 24-bit CC
    NRF_RTC1->EVENTS_COMPARE[0] = 0;
    NRF_RTC1->INTENSET = RTC_INTENSET_COMPARE0_Msk;

    NVIC_ClearPendingIRQ(RTC1_IRQn);
    NVIC_SetPriority(RTC1_IRQn, 0);
    NVIC_EnableIRQ(RTC1_IRQn);
    printk("NVIC: pri=%u\n", NVIC_GetPriority(RTC1_IRQn));
    NRF_RTC1->EVENTS_TICK = 0;
    NRF_RTC1->EVENTS_OVRFLW = 0;
    NRF_RTC1->TASKS_START = 1;
    printk("RTC1 COUNTER=%u\n", (unsigned)NRF_RTC1->COUNTER);
    k_msleep(100);
    printk("RTC1 COUNTER after 100ms=%u\n", (unsigned)NRF_RTC1->COUNTER);

    __SEV();
    __WFE(); // first WFE clears event
    while (!rtc_wakeup_flag) 
        k_sleep(K_MSEC(10));

    NRF_RTC1->TASKS_STOP = 1;
    NVIC_DisableIRQ(RTC1_IRQn);

    printk("Woke up from ULTRA LOW POWER\n");
}

/* ----------------- Button setup ----------------- */
static int button_setup(void)
{
    if (!device_is_ready(button.port)) return -ENODEV;

    gpio_pin_configure_dt(&button, GPIO_INPUT | GPIO_PULL_UP);
    gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
    gpio_init_callback(&button_cb, button_pressed_isr, BIT(button.pin));
    gpio_add_callback(button.port, &button_cb);
    return 0;
}

/* ----------------- bt_ready ----------------- */
static void bt_ready(int err)
{
    if (err) {
        printk("Bluetooth init failed: %d\n", err);
        return;
    }

    printk("Bluetooth ready\n");
}

/* ----------------- Main ----------------- */
int main(void)
{
    printk("BeeNode starting...\n");

    button_setup();

    bt_enable(bt_ready);

    k_work_init_delayable(&adv_work, adv_burst_work);

    while (1) {

        printk("Wakeup - Advertising burst\n");

        adv_remaining = BURST_MS;

        struct bt_le_adv_param adv = {
            .id = BT_ID_DEFAULT,
            .options = BT_LE_ADV_OPT_CONNECTABLE |
                       BT_LE_AD_GENERAL |
                       BT_LE_ADV_OPT_USE_TX_POWER,
            .interval_min = BT_GAP_ADV_FAST_INT_MIN_2,
            .interval_max = BT_GAP_ADV_FAST_INT_MAX_2,
        };

        bt_le_adv_start(&adv, ad, ARRAY_SIZE(ad), NULL, 0);

        k_work_schedule(&adv_work, K_MSEC(0));

        uint64_t start = k_uptime_get();

        while (k_uptime_get() - start < BURST_MS) {

            if (current_conn) {
                printk("Central connected - sending data...\n");
                k_sleep(K_MSEC(500));
                bt_conn_disconnect(current_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
                break;
            }

            if (button_pressed) {
                printk("Button pressed - restarting burst\n");
                button_pressed = false;
                adv_remaining = BURST_MS;
                start = k_uptime_get();
            }

            k_sleep(K_MSEC(1000));
        }

        /* After window → low power for 60s */
        //system_on_sleep_seconds(2);

        printk("Cycle done\n");
    }
}
