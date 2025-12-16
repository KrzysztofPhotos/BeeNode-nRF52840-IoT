#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <nrfx.h>
#include <string.h>

/* ---------------- Sensor frame ---------------- */
struct __packed sensor_frame_t {
    uint8_t  dev_id_3[3];
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
    static uint8_t seq = 0;

    read_dev_id_24(frame.dev_id_3);
    frame.date_hour = k_uptime_get() / 1000;

    frame.temp_x10 = 200 + seq;     // 20.0°C + seq
    frame.hum_x10  = 500 + seq;     // 50.0% + seq
    frame.lux      = seq;
    frame.weight   = 100000 + seq;

    frame.in_bees  = seq;
    frame.out_bees = seq;
    frame.stat     = seq & 0x01;

    frame.pp[0] = 'P';
    frame.pp[1] = 'P';

    memcpy(&mfg_data[2], &frame, sizeof(frame));

    printk("SEQ=%u TEMP=%d HUM=%d LUX=%u\n",
           seq, frame.temp_x10, frame.hum_x10, frame.lux);

    seq++;
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
    printk("BeeNode starting (DEBUG MODE, NO SLEEP)\n");

    bt_enable(bt_ready);

    struct bt_le_adv_param adv = {
        .id = BT_ID_DEFAULT,
        .options = BT_LE_ADV_OPT_USE_TX_POWER |
                   BT_LE_AD_GENERAL,
        .interval_min = BT_GAP_ADV_FAST_INT_MIN_2,
        .interval_max = BT_GAP_ADV_FAST_INT_MAX_2,
    };

    bt_le_adv_start(&adv, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));

    /* Set TX power to +4 dBm */
    bt_set_tx_power(BT_HCI_VS_LL_HANDLE_TYPE_ADV, 0, 4);

    while (1) {
        update_sensor_frame();

        bt_le_adv_update_data(ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));

        k_sleep(K_MSEC(150));
    }
}
