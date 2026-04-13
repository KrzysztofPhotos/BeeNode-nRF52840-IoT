/**
 * ============================================================================
 * BeeNode - Beehive Monitoring System
 * ============================================================================
 * 
 * System monitorowania warunków w ulu pszczelim oparty na NRF52840 DK
 * 
 * Tryb pracy:
 * 1. Deep Sleep (System OFF) przez 10 minut
 * 2. Wybudzenie przez RTC
 * 3. Advertising przez 30 sekund (możliwość połączenia z RPi)
 * 4. Jeśli RPi się połączy - wysyłanie danych przez GATT
 * 5. Powrót do Deep Sleep
 * 
 * Architektura BLE:
 * - NRF52840 = Peripheral (slave)
 * - Raspberry Pi = Central (master)
 * - Dane w GATT Service (nie tylko w advertising)
 * - UUID do identyfikacji serwisu i charakterystyk
 */
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include <zephyr/random/random.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <nrfx.h>
#include <string.h>
#include <zephyr/sys/poweroff.h> // dla uzywania sys_poweroff()
#include <hal/nrf_gpio.h>
#include <hal/nrf_radio.h>
#include <hal/nrf_power.h>
#include <zephyr/settings/settings.h>

/* ============================================================================
 * KONFIGURACJA CZASOWA
 * ============================================================================ */
#define ADVERTISING_DURATION_MS  30000   // 30 sekund advertising (30000)
#define DEEP_SLEEP_DURATION_S    600     // 10 minut deep sleep (600)
#define ADV_UPDATE_INTERVAL_MS   1000    // Aktualizacja danych co 1s podczas advertising


// Definicje pinow dla nRF52840-DK (DT=27, SCK=26)
#define HX711_DT_PIN  27
#define HX711_SCK_PIN 26

static const struct device *gpio0_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));

// Inicjalizacja pinów
static void hx711_init(void) {
    if (!device_is_ready(gpio0_dev)) {
        printk("BLAD: Port GPIO0 nie jest gotowy!\n");
        return;
    }
    // Zegar jako wyjscie (stan niski na start), Dane jako wejscie
    gpio_pin_configure(gpio0_dev, HX711_SCK_PIN, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure(gpio0_dev, HX711_DT_PIN, GPIO_INPUT);
    printk("HX711 zainicjalizowany recznie (pin 26 i 27)\n");
}

// Funkcja bit-banging czytajaca dane z HX711
static int32_t hx711_read(void) {
    int32_t count = 0;
    int timeout = 10000; 

    // Czekaj az uklad bedzie gotowy (linia DT zejdzie na LOW)
    while (gpio_pin_get(gpio0_dev, HX711_DT_PIN) == 1 && timeout > 0) {
        k_busy_wait(10);
        timeout--;
    }

    if (timeout == 0) {
        printk("BLAD: Timeout - HX711 nie odpowiada!\n");
        return 0;
    }

    // Odczyt 24 bitow danych
    for (int i = 0; i < 24; i++) {
        gpio_pin_set(gpio0_dev, HX711_SCK_PIN, 1);
        k_busy_wait(1); // 1 mikrosekunda w gore
        count = count << 1;
        gpio_pin_set(gpio0_dev, HX711_SCK_PIN, 0);
        k_busy_wait(1); // 1 mikrosekunda w dol
        
        if (gpio_pin_get(gpio0_dev, HX711_DT_PIN)) {
            count++;
        }
    }

    // 25 impuls to ustawienie wzmocnienia na 128 dla kolejnego pomiaru
    gpio_pin_set(gpio0_dev, HX711_SCK_PIN, 1);
    k_busy_wait(1);
    gpio_pin_set(gpio0_dev, HX711_SCK_PIN, 0);
    k_busy_wait(1);

    // Format U2 (wartosci ujemne)
    if (count & 0x800000) {
        count |= 0xFF000000;
    }

    return count;
}


/* ============================================================================
 * PARAMETRY KALIBRACJI I PAMIĘĆ NVS
 * ============================================================================ */
int32_t tare_offset = -600000;         // Domyslna tara
float calib_factor = 1000000.0;        // Domyslny wspolczynnik
float known_weight_kg = 0.1835;         // WAGA REDMI NOTE 12 4g
volatile bool start_tare = false;
volatile bool start_calib = false;

// Funkcja obsługująca zapis/odczyt z pamięci flash (NVS)
static int scale_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    const char *next;
    if (settings_name_steq(name, "tare", &next) && !next) {
        if (len != sizeof(tare_offset)) return -EINVAL;
        read_cb(cb_arg, &tare_offset, sizeof(tare_offset));
        return 0;
    }
    if (settings_name_steq(name, "factor", &next) && !next) {
        if (len != sizeof(calib_factor)) return -EINVAL;
        read_cb(cb_arg, &calib_factor, sizeof(calib_factor));
        return 0;
    }
    return -ENOENT;
}

struct settings_handler scale_conf = {
    .name = "scale",
    .h_set = scale_settings_set,
};

/* ============================================================================
 * INTERFEJS: LED 1, PRZYCISK SW2 (Tara), PRZYCISK SW3 (Kalibracja)
 * ============================================================================ */
// --- LED i Timer do mrugania w tle ---
static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

static void led_timer_isr(struct k_timer *timer_id) {
    gpio_pin_toggle_dt(&led0); // Zmiana stanu diody na przeciwny
}
K_TIMER_DEFINE(led_blink_timer, led_timer_isr, NULL);

enum led_mode { LED_OFF, LED_BLINK_SLOW, LED_BLINK_FAST };

static void set_led_mode(enum led_mode mode) {
    if (!device_is_ready(led0.port)) return;
    
    if (mode == LED_OFF) {
        k_timer_stop(&led_blink_timer);
        gpio_pin_set_dt(&led0, 0); // Wylacz diode
    } else if (mode == LED_BLINK_SLOW) {
        k_timer_start(&led_blink_timer, K_MSEC(500), K_MSEC(500)); // Mruga co pol sekundy
    } else if (mode == LED_BLINK_FAST) {
        k_timer_start(&led_blink_timer, K_MSEC(100), K_MSEC(100)); // Mruga bardzo szybko
    }
}

// --- Przycisk SW2 (Button 2) do Tary ---
static const struct gpio_dt_spec btn_tare = GPIO_DT_SPEC_GET(DT_ALIAS(sw1), gpios);
static struct gpio_callback btn_tare_cb;

static void btn_tare_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins) {
    start_tare = true;
}

// --- Przycisk SW3 (Button 3) do Kalibracji ---
static const struct gpio_dt_spec btn_calib = GPIO_DT_SPEC_GET(DT_ALIAS(sw2), gpios);
static struct gpio_callback btn_calib_cb;

static void btn_calib_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins) {
    start_calib = true;
}

static int ui_setup(void) {
    // 1. Inicjalizacja LED
    if (device_is_ready(led0.port)) {
        gpio_pin_configure_dt(&led0, GPIO_OUTPUT_INACTIVE);
    }

    // 2. Inicjalizacja Przycisku SW2 (Tara)
    if (device_is_ready(btn_tare.port)) {
        gpio_pin_configure_dt(&btn_tare, GPIO_INPUT | GPIO_PULL_UP);
        gpio_pin_interrupt_configure_dt(&btn_tare, GPIO_INT_EDGE_TO_ACTIVE);
        gpio_init_callback(&btn_tare_cb, btn_tare_isr, BIT(btn_tare.pin));
        gpio_add_callback(btn_tare.port, &btn_tare_cb);
    } else {
        printk("WARN: Przycisk SW2 niedostepny!\n");
    }

    // 3. Inicjalizacja Przycisku SW3 (Kalibracja)
    if (device_is_ready(btn_calib.port)) {
        gpio_pin_configure_dt(&btn_calib, GPIO_INPUT | GPIO_PULL_UP);
        gpio_pin_interrupt_configure_dt(&btn_calib, GPIO_INT_EDGE_TO_ACTIVE);
        gpio_init_callback(&btn_calib_cb, btn_calib_isr, BIT(btn_calib.pin));
        gpio_add_callback(btn_calib.port, &btn_calib_cb);
    } else {
        printk("WARN: Przycisk SW3 niedostepny!\n");
    }

    printk("Gotowe! SW1: Test (Wybudzenie), SW2: Tara, SW3: Kalibracja, LED0: status.\n");
    return 0;
}

/* ============================================================================
 * POMOCNICZA: UŚREDNIANIE (jak w Arduino)
 * ============================================================================ */
static int32_t hx711_read_average(int times) {
    int64_t sum = 0;
    for (int i = 0; i < times; i++) {
        sum += hx711_read();
        k_sleep(K_MSEC(10)); // Krotka przerwa miedzy odczytami
    }
    return (int32_t)(sum / times);
}

/* ============================================================================
 * DANE BLUETOOTH (Zmienne globalne dla GATT)
 * ============================================================================ */
uint16_t current_weight_ble = 0;       // Waga dla BLE (rozdzielczosc 0.005 kg)
uint8_t  current_battery_level = 88;   // Procent baterii (0-100%)
uint16_t current_battery_voltage = 395;// Napiecie baterii (395 = 3.95V)
int16_t  current_temperature = 2250;   // Temperatura (2250 = 22.50 °C)


/* ============================================================================
 * ODCZYT I KALIBRACJA WAGI
 * ============================================================================ */
static void test_read_weight(void) {
    // 1. PROCEDURA TARY
    if (start_tare) {
        start_tare = false;
        
        printk("\n========================================\n");
        printk("          START TARY (SW2)\n");
        printk("========================================\n");
        
        set_led_mode(LED_BLINK_SLOW);
        printk("1. Zdejmij wszystko z wagi!\n");
        printk("   Masz 5 sekund...\n");
        k_sleep(K_SECONDS(5));
        
        tare_offset = hx711_read_average(10);
        settings_save_one("scale/tare", &tare_offset, sizeof(tare_offset));
        printk(">>> Tara zrobiona! (Wartosc: %d) <<<\n", tare_offset);
        printk("========================================\n\n");
        
        set_led_mode(LED_OFF);
        return; 
    }

    // 2. PROCEDURA KALIBRACJI
    if (start_calib) {
        start_calib = false;
        
        printk("\n========================================\n");
        printk("        START KALIBRACJI (SW3)\n");
        printk("========================================\n");
        
        set_led_mode(LED_BLINK_FAST);
        printk("1. Poloz swoj telefon (%.3f kg) na wadze!\n", known_weight_kg);
        printk("   Masz 5 sekund...\n");
        k_sleep(K_SECONDS(5));
        
        int32_t reading = hx711_read_average(10);
        calib_factor = (float)(tare_offset - reading) / known_weight_kg;
        settings_save_one("scale/factor", &calib_factor, sizeof(calib_factor));
        
        printk(">>> Wspolczynnik wyliczony! (Wartosc: %.1f) <<<\n", calib_factor);
        printk("========================================\n\n");
        
        set_led_mode(LED_OFF);
        return; 
    }

    // 3. NORMALNY ODCZYT (odpala sie caly czas, gdy nie kalibrujemy)
    int32_t raw_val = hx711_read();
    int32_t weight_no_tare = tare_offset - raw_val;
    
    // Delikatne zerowanie szumow w okolicach zera (do 5 gramow)
    if (weight_no_tare < 0 && weight_no_tare > -5000) {
        weight_no_tare = 0; 
    }
    
    double weight_kg = (double)weight_no_tare / calib_factor;
    printk(">>> SUROWY: %d | WAGA: %.3f kg | TEMP: %.2f st.C <<<\n", 
           raw_val, weight_kg, (float)current_temperature / 100.0);
}

/* ============================================================================
 * CZUJNIK TEMPERATURY DS18B20
 * ============================================================================ */
static const struct device *ds18b20_dev = DEVICE_DT_GET_ANY(maxim_ds18b20);

static int16_t read_real_temperature(void) {
    if (!device_is_ready(ds18b20_dev)) {
        printk("WARN: Czujnik DS18B20 nie jest podlaczony lub gotowy!\n");
        return 2250; // Wartosc awaryjna (22.50 st C) jesli czujnik odpadnie
    }

    struct sensor_value temp;
    if (sensor_sample_fetch(ds18b20_dev) == 0) {
        sensor_channel_get(ds18b20_dev, SENSOR_CHAN_AMBIENT_TEMP, &temp);
        double temp_c = sensor_value_to_double(&temp);
        int16_t temp_x100 = (int16_t)(temp_c * 100);
        // printk(">>> DS18B20: %.2f st. C (do BLE: %d) <<<\n", temp_c, temp_x100);
        return temp_x100;
    }
    
    printk("Blad odczytu DS18B20!\n");
    return 2250;
}


/* ============================================================================
 * BLUETOOTH - ADVERTISING (Nowy Standard)
 * ============================================================================ */
// Company ID (0xFFFE) | DeviceType (0xA1) | Sensor ID (4 Bajty)
static uint8_t mfg_data[7] = {0xFE, 0xFF, 0xA1, 0x00, 0x00, 0x00, 0x00};

static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
    BT_DATA(BT_DATA_MANUFACTURER_DATA, mfg_data, sizeof(mfg_data)),
};

static const struct bt_data sd[] = {
    BT_DATA(BT_DATA_NAME_COMPLETE, "BeeNode", 7),
};

/* ============================================================================
 * BLUETOOTH - CALLBACKI ODCZYTU (GATT)
 * ============================================================================ */
// 1. ODCZYT WAGI (Flags 1B + Waga 2B)
static ssize_t read_weight(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf, uint16_t len, uint16_t offset) {
    uint8_t payload[3];
    payload[0] = 0x00; // Flaga: Waga w kilogramach (SI)
    payload[1] = current_weight_ble & 0xFF;        // mlodszy bajt
    payload[2] = (current_weight_ble >> 8) & 0xFF; // starszy bajt
    return bt_gatt_attr_read(conn, attr, buf, len, offset, payload, sizeof(payload));
}

// 2. ODCZYT PROCENTÓW BATERII
static ssize_t read_bat_level(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf, uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &current_battery_level, sizeof(current_battery_level));
}

// 3. ODCZYT NAPIĘCIA BATERII
static ssize_t read_bat_voltage(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf, uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &current_battery_voltage, sizeof(current_battery_voltage));
}

// 4. ODCZYT TEMPERATURY
static ssize_t read_temperature(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf, uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &current_temperature, sizeof(current_temperature));
}

/* ============================================================================
 * BLUETOOTH - DEFINICJA STRUKTURY SERWISÓW (GATT TREE)
 * ============================================================================ */
// UUID dla Napięcia (Niestandardowe, z dokumentacji)
static struct bt_uuid_128 bat_volt_uuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x0000FFF2, 0x0000, 0x1000, 0x8000, 0x00805F9B34FB)
);

BT_GATT_SERVICE_DEFINE(beenode_svc,
    // --- SERWIS WAGI (Weight Scale: 0x181D) ---
    BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_16(0x181D)),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_16(0x2A9D), BT_GATT_CHRC_READ, BT_GATT_PERM_READ, read_weight, NULL, NULL),

    // --- SERWIS BATERII (Battery Service: 0x180F) ---
    BT_GATT_PRIMARY_SERVICE(BT_UUID_BAS),
    BT_GATT_CHARACTERISTIC(BT_UUID_BAS_BATTERY_LEVEL, BT_GATT_CHRC_READ, BT_GATT_PERM_READ, read_bat_level, NULL, NULL),
    BT_GATT_CHARACTERISTIC(&bat_volt_uuid.uuid, BT_GATT_CHRC_READ, BT_GATT_PERM_READ, read_bat_voltage, NULL, NULL),

    // --- SERWIS TEMPERATURY (Environmental Sensing: 0x181A) ---
    BT_GATT_PRIMARY_SERVICE(BT_UUID_ESS),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_16(0x2A6E), BT_GATT_CHRC_READ, BT_GATT_PERM_READ, read_temperature, NULL, NULL)
);

/* ============================================================================
 * AKTUALIZACJA DANYCH BLUETOOTH (Zastępuje stare update_sensor_frame)
 * ============================================================================ */
static void update_ble_data(void)
{
    // 1. Wpisz unikalne ID mikrokontrolera do paczki Advertisingu
    uint32_t dev_id = NRF_FICR->DEVICEID[0];
    mfg_data[3] = (dev_id >> 24) & 0xFF;
    mfg_data[4] = (dev_id >> 16) & 0xFF;
    mfg_data[5] = (dev_id >> 8)  & 0xFF;
    mfg_data[6] = dev_id & 0xFF;
    
    // 2. Przelicz aktualna wage na standard BLE (jesli nie trwaja testy)
    if (!start_tare && !start_calib) {
        int32_t raw_val = hx711_read();
        int32_t weight_no_tare = tare_offset - raw_val;
        if (weight_no_tare < 0 && weight_no_tare > -5000) weight_no_tare = 0; 
        
        double weight_kg = (double)weight_no_tare / calib_factor;
        
        // Zgodnie z wytycznymi: rozdzielczość 0.005 kg
        // Żeby uzyskać wartość dla BLE matematycznie: waga / 0.005 (czyli waga * 200)
        current_weight_ble = (uint16_t)(weight_kg * 200.0);
    }

    // 3. Pobierz prawdziwa temperature z DS18B20 do GATT
    current_temperature = read_real_temperature();
    // -----------------
}

/* ============================================================================
 * BLUETOOTH - CONNECTION CALLBACKS
 * ============================================================================ */

static struct bt_conn *current_conn = NULL;

static void connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        printk("BLE: Błąd połączenia: %u\n", err);
        return;
    }

    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    printk("BLE: Połączono z %s\n", addr);
    
    current_conn = bt_conn_ref(conn);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    printk("BLE: Rozłączono z %s (powód: %u)\n", addr, reason);
    
    if (current_conn) {
        bt_conn_unref(current_conn);
        current_conn = NULL;
    }
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
};

/* ============================================================================
 * BLUETOOTH - INICJALIZACJA
 * ============================================================================ */

static void bt_ready(int err)
{
    if (err) {
        printk("BLE: Błąd inicjalizacji: %d\n", err);
        return;
    }
    printk("BLE: Stack gotowy\n");
}

/* ============================================================================
 * ADVERTISING - ZARZĄDZANIE
 * ============================================================================ */

/**
 * Rozpoczyna advertising z szybkim interwałem
 * Umożliwia szybkie wykrycie przez RPi
 */
static int start_advertising(void)
{
    // DEBUGOWANIE: Sprawdź rozmiary
    // printk("DEBUG: sizeof(sensor_frame_t) = %u\n", sizeof(struct sensor_frame_t));
    // printk("DEBUG: sizeof(mfg_data) = %u\n", sizeof(mfg_data));
    // printk("DEBUG: Total AD payload = %u bytes\n", 
    //        3 + sizeof(mfg_data) + 2);  // FLAGS + MFG_DATA + overhead
    
    // Sprawdź czy BT jest gotowy
    if (!bt_is_ready()) {
        printk("ERROR: Bluetooth nie jest gotowy!\n");
        return -EAGAIN;
    }

    struct bt_le_adv_param adv_param = {
        .id = BT_ID_DEFAULT,
        .options = BT_LE_ADV_OPT_CONNECTABLE |
                   BT_LE_ADV_OPT_USE_TX_POWER,  // Usunięto USE_NAME
        .interval_min = BT_GAP_ADV_FAST_INT_MIN_2,  // 100 ms
        .interval_max = BT_GAP_ADV_FAST_INT_MAX_2,  // 150 ms
    };

    int err = bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
    if (err) {
        printk("ERROR: Błąd startu advertising: %d (", err);
        switch(err) {
            case -EINVAL: printk("EINVAL - nieprawidłowe parametry)\n"); break;
            case -EALREADY: printk("EALREADY - advertising już aktywny)\n"); break;
            case -ENOTSUP: printk("ENOTSUP - niewspierana operacja)\n"); break;
            case -ENOMEM: printk("ENOMEM - brak pamięci)\n"); break;
            case -EAGAIN: printk("EAGAIN - spróbuj ponownie)\n"); break;
            default: printk("Nieznany błąd)\n");
        }
        return err;
    }

    printk("BLE: Advertising rozpoczęty ✓\n");
    return 0;
}

/**
 * Zatrzymuje advertising
 */
static void stop_advertising(void)
{
    int err = bt_le_adv_stop();
    if (err) {
        printk("BLE: Błąd zatrzymania advertising: %d\n", err);
    } else {
        printk("BLE: Advertising zatrzymany\n");
    }
}

/* ============================================================================
 * DEEP SLEEP (SYSTEM OFF)
 * ============================================================================ */

/**
 * Przechodzi w tryb System OFF z wybudzeniem przez RTC
 * 
 * UWAGA: W trybie System OFF:
 * - Zatrzymane są wszystkie peryferia (BLE, UART, itp.)
 * - Pobór prądu ~0.4 µA
 * - Wybudzenie = reset systemu (program startuje od początku)
 * - Zachowana jest tylko pamięć RAM (jeśli skonfigurowana)
 * 
 * @param seconds Czas snu w sekundach
 */
static void enter_deep_sleep(uint32_t seconds)
{
    printk("System OFF na %u sekund\n", seconds);

    /* 1. Start LFCLK (RTC tego wymaga) */
    NRF_CLOCK->TASKS_LFCLKSTART = 1;
    while (!NRF_CLOCK->EVENTS_LFCLKSTARTED);
    NRF_CLOCK->EVENTS_LFCLKSTARTED = 0;

    /* 2. Konfiguracja RTC1 */
    NRF_RTC1->TASKS_STOP = 1;
    NRF_RTC1->TASKS_CLEAR = 1;
    NRF_RTC1->PRESCALER = 0; // 32768 Hz

    uint32_t ticks = seconds * 32768;
    if (ticks > 0x00FFFFFF) {
        ticks = 0x00FFFFFF;
    }

    NRF_RTC1->CC[0] = ticks;
    NRF_RTC1->EVENTS_COMPARE[0] = 0;
    NRF_RTC1->EVTENSET = RTC_EVTENSET_COMPARE0_Msk;
    NRF_RTC1->TASKS_START = 1;

    /* 3. Wake-up z przycisku (P0.11 – BUTTON1) */
    NRF_GPIO->PIN_CNF[NRF_GPIO_PIN_MAP(0,11)] =
        (GPIO_PIN_CNF_SENSE_Low << GPIO_PIN_CNF_SENSE_Pos) |
        (GPIO_PIN_CNF_PULL_Pullup << GPIO_PIN_CNF_PULL_Pos) |
        (GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos) |
        (GPIO_PIN_CNF_DIR_Input << GPIO_PIN_CNF_DIR_Pos);

    printk("SYSTEM OFF\n");

    /* 4. System OFF */
    NRF_POWER->SYSTEMOFF = 1;

    while (1) {
        __WFE();
    }
}


/* ============================================================================
 * MAIN LOOP
 * ============================================================================ */

int main(void)
{
    printk("\n");
    printk("========================================\n");
    printk("  BeeNode - Beehive Monitor v1.0\n");
    printk("========================================\n");
    printk("Board: NRF52840 DK\n");
    printk("Build: %s %s\n", __DATE__, __TIME__);
    printk("========================================\n\n");

    // Inicjalizacja naszej wagi
    hx711_init();

    // Inicjalizacja interfejsu (LED + SW2)
    ui_setup();

    // --- Inicjalizacja i wczytanie pamieci ---
    settings_subsys_init();
    settings_register(&scale_conf);
    settings_load();
    printk("Zaladowano z pamieci NVS -> Tara: %d, Wspolczynnik: %.1f\n", tare_offset, calib_factor);
    // -----------------------------------------

    // Inicjalizacja Bluetooth
    int err = bt_enable(bt_ready);
    if (err) {
        printk("FATAL: Bluetooth enable failed: %d\n", err);
        return err;
    }
    k_sleep(K_MSEC(500));  // Daj czas na inicjalizację

    // --- TX POWER +4 dBm ---
    // Ustaw TX power radia na +4 dBm (nRF52840)
    NRF_RADIO->TXPOWER = RADIO_TXPOWER_TXPOWER_Pos4dBm;
    printk("TX power ustawiony na +4 dBm (RADIO)\n");

    printk("\n=== GŁÓWNA PĘTLA PROGRAMU ===\n\n");

    while (1) {
        // ---------------------------------------------------------------
        // FAZA 1: ADVERTISING (30 sekund)
        // ---------------------------------------------------------------
        printk(">>> FAZA 1: Advertising przez %d sekund\n", 
               ADVERTISING_DURATION_MS / 1000);
        
        // Zaktualizuj dane przed rozpoczęciem advertising
        update_ble_data();
        
        // Rozpocznij advertising
        err = start_advertising();
        if (err) {
            printk("ERROR: Nie można rozpocząć advertising, restart...\n");
            k_sleep(K_SECONDS(5));
            continue;
        }

        // Główna pętla advertising
        uint64_t adv_start_time = k_uptime_get();
        bool connection_handled = false;
        
        while (k_uptime_get() - adv_start_time < ADVERTISING_DURATION_MS) {
            
            // Sprawdź czy RPi się połączył
            if (current_conn) {
                printk("\n!!! RPi połączony - czekam na odczyt GATT !!!\n");
                
                // Daj czas RPi na odczyt danych przez GATT
                // W prawdziwej aplikacji możesz dodać notification callback
                k_sleep(K_SECONDS(5));
                
                // Rozłącz się
                printk("Rozłączam połączenie...\n");
                bt_conn_disconnect(current_conn, 
                                  BT_HCI_ERR_REMOTE_USER_TERM_CONN);
                k_sleep(K_MSEC(500));
                
                connection_handled = true;
                break;  // Wyjdź z pętli advertising
            }
            
            // Aktualizuj dane w advertising co jakiś czas
            if ((k_uptime_get() - adv_start_time) % ADV_UPDATE_INTERVAL_MS == 0) {
                update_ble_data();
                bt_le_adv_update_data(ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
            }
            
            k_sleep(K_MSEC(100));
            
            test_read_weight(); // <--- NASZA WAGA JEST TUTAJ
        }

        // Zatrzymaj advertising po upływie czasu
        stop_advertising();

        if (connection_handled) {
            printk(">>> Połączenie obsłużone, przechodzę do deep sleep\n");
        } else {
            printk(">>> Brak połączenia, przechodzę do deep sleep\n");
        }

        // ---------------------------------------------------------------
        // FAZA 2: DEEP SLEEP (10 minut)
        // ---------------------------------------------------------------
        // UWAGA: Po wybudzeniu z System OFF program restartuje od main()
        
        enter_deep_sleep(DEEP_SLEEP_DURATION_S);
        
        // Ten kod nigdy się nie wykona w prawdziwym System OFF
        // (system się zrestartuje)
        printk(">>> Wybudzenie z deep sleep (symulacja)\n");
        k_sleep(K_SECONDS(2));
    }

    return 0;
}

/**
 * ============================================================================
 * NOTATKI IMPLEMENTACYJNE
 * ============================================================================
 * 
 * TODO dla pełnej funkcjonalności:
 * 
 * 1. DEEP SLEEP:
 *    - Zamień __WFE() loop na sys_poweroff() gdy SDK to wspiera
 *    - Skonfiguruj retention RAM jeśli potrzebne
 *    - Dodaj GPIO wake-up jako backup (np. przycisk)
 * 
 * 2. POWER MANAGEMENT:
 *    - Włącz CONFIG_PM=y w prj.conf
 *    - Optymalizuj power consumption w stanie advertising
 *    - Zmierz rzeczywisty pobór prądu
 * 
 * 3. REAL-TIME CLOCK:
 *    - Dodaj obsługę RTC dla timestamp w date_hour
 *    - Synchronizacja czasu z RPi przy pierwszym połączeniu
 * 
 * 4. CZUJNIKI:
 *    - Zamień fake_*() na prawdziwe odczyty I2C/SPI
 *    - Dodaj error handling dla czujników
 *    - Kalibracja czujników
 * 
 * 5. GATT:
 *    - Dodaj więcej charakterystyk (konfiguracja, status, itp.)
 *    - Notifications dla real-time data streaming
 *    - Write characteristic dla konfiguracji zdalnej
 * 
 * 6. TESTOWANIE:
 *    - Test z nRF Connect (advertising + GATT)
 *    - Test z RPi Central (automatyczne połączenie)
 *    - Test power consumption
 *    - Test długoterminowy (stabilność)
 * 
 * ============================================================================
 */

 
