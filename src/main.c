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


/* ============================================================================
 * KONFIGURACJA CZASOWA
 * ============================================================================ */
#define ADVERTISING_DURATION_MS  10000   // 30 sekund advertising (30000)
#define DEEP_SLEEP_DURATION_S    60     // 10 minut deep sleep (600)
#define ADV_UPDATE_INTERVAL_MS   1000    // Aktualizacja danych co 1s podczas advertising

/* ============================================================================
 * STRUKTURA DANYCH SENSORYCZNYCH
 * ============================================================================ */

/**
 * Ramka zawierająca wszystkie dane z czujników
 * Pakowana bez paddingu dla optymalnej transmisji BLE
 */
struct __packed sensor_frame_t {
    uint8_t  dev_id_3[3];    // 24-bitowy ID urządzenia (z FICR)
    uint32_t date_hour;      // Timestamp (na razie nieużywany)
    int16_t  temp_x10;       // Temperatura * 10 (np. 235 = 23.5°C)
    int16_t  hum_x10;        // Wilgotność * 10 (np. 650 = 65.0%)
    uint16_t lux;            // Natężenie światła [lux]
    uint32_t weight;         // Waga ula [gramy]
    uint16_t in_bees;        // Liczba pszczół wlatujących
    uint16_t out_bees;       // Liczba pszczół wylatujących
    uint8_t  stat;           // Status (0/1)
    char     pp[2];          // Marker "PP"
};

// Globalna instancja ramki danych
static struct sensor_frame_t frame;

/* ============================================================================
 * BLUETOOTH LOW ENERGY - ADVERTISING
 * ============================================================================ */

// Manufacturer Data: Company ID (0x0059) + sensor_frame
static uint8_t mfg_data[2 + sizeof(frame)] = {0x59, 0x00};

// Advertising Data - zawiera flagi i manufacturer data
static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
    BT_DATA(BT_DATA_MANUFACTURER_DATA, mfg_data, sizeof(mfg_data)),
};

// Scan Response Data - nazwa urządzenia
static const struct bt_data sd[] = {
    BT_DATA(BT_DATA_NAME_COMPLETE, "BeeNode", 7),
};

/* ============================================================================
 * BLUETOOTH LOW ENERGY - GATT SERVICE
 * ============================================================================ */

/**
 * UUID dla BeeNode Service: 12345678-1234-5678-1234-56789abcdef0
 * UUID dla Sensor Data Characteristic: 12345678-1234-5678-1234-56789abcdef1
 * 
 * RPi będzie szukać tego UUID i odczytywać dane
 */

#define BT_UUID_BEENODE_SERVICE_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef0)

#define BT_UUID_BEENODE_SENSOR_DATA_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef1)

static struct bt_uuid_128 beenode_service_uuid = BT_UUID_INIT_128(
    BT_UUID_BEENODE_SERVICE_VAL
);

static struct bt_uuid_128 sensor_data_uuid = BT_UUID_INIT_128(
    BT_UUID_BEENODE_SENSOR_DATA_VAL
);

/**
 * Callback do odczytu danych z charakterystyki
 * RPi wywoła to podczas bt_gatt_read()
 */
static ssize_t read_sensor_data(struct bt_conn *conn,
                                const struct bt_gatt_attr *attr,
                                void *buf, uint16_t len, uint16_t offset)
{
    printk("GATT: Central czyta dane sensoryczne\n");
    
    // Zwróć całą strukturę sensor_frame
    return bt_gatt_attr_read(conn, attr, buf, len, offset,
                            &frame, sizeof(frame));
}

// Definicja GATT Service
BT_GATT_SERVICE_DEFINE(beenode_svc,
    BT_GATT_PRIMARY_SERVICE(&beenode_service_uuid),
    BT_GATT_CHARACTERISTIC(&sensor_data_uuid.uuid,
                          BT_GATT_CHRC_READ,
                          BT_GATT_PERM_READ,
                          read_sensor_data, NULL, NULL),
);

/* ============================================================================
 * PRZYCISK (opcjonalny - do testowania)
 * ============================================================================ */

static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET_OR(DT_ALIAS(sw0), gpios, {0});
// static struct gpio_callback button_cb;
// static volatile bool button_pressed = false;

// static void button_pressed_isr(const struct device *dev, 
//                                struct gpio_callback *cb, 
//                                uint32_t pins)
// {
//     button_pressed = true;
//     printk("Przycisk wciśnięty!\n");
// }

static int button_setup(void)
{
    if (!device_is_ready(button.port)) {
        printk("WARN: Przycisk niedostępny\n");
        return -ENODEV;
    }

    gpio_pin_configure_dt(&button, GPIO_INPUT | GPIO_PULL_UP);
    // gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
    // gpio_init_callback(&button_cb, button_pressed_isr, BIT(button.pin));
    // gpio_add_callback(button.port, &button_cb);
    
    printk("Przycisk skonfigurowany (pin %d)\n", button.pin);
    return 0;
}

/* ============================================================================
 * GENERATORY FAKE DANYCH (do testów bez czujników)
 * ============================================================================ */

static double fake_temp(void)   { 
    return 20.0 + (sys_rand32_get() % 1000) / 100.0;  // 20.0 - 30.0°C
}

static double fake_hum(void)    { 
    return 50.0 + (sys_rand32_get() % 500) / 10.0;    // 50.0 - 100.0%
}

static uint16_t fake_lux(void)  { 
    return 100 + (sys_rand32_get() % 1000);           // 100 - 1100 lux
}

static uint32_t fake_weight(void) { 
    return 100000 + (sys_rand32_get() % 50000);       // 100-150 kg
}

static uint16_t fake_in(void)   { 
    return sys_rand32_get() % 300;                    // 0 - 300 pszczół
}

static uint16_t fake_out(void)  { 
    return sys_rand32_get() % 300;                    // 0 - 300 pszczół
}

static uint8_t fake_stat(void) { 
    return sys_rand32_get() % 2;                      // 0 lub 1
}

/* ============================================================================
 * ODCZYT DEVICE ID Z NRF FICR
 * ============================================================================ */

/**
 * Odczytuje 24-bitowy Device ID z Factory Information Configuration Registers
 * Każdy chip NRF ma unikalny 64-bitowy ID, my używamy dolnych 24 bitów
 */
static void read_dev_id_24(uint8_t out[3])
{
    uint32_t d0 = NRF_FICR->DEVICEID[0];
    uint32_t d1 = NRF_FICR->DEVICEID[1];

    // Kombinujemy dolne 16 bitów z d0 i dolne 8 bitów z d1
    uint32_t dev24 = ((d1 & 0xFF) << 16) | (d0 & 0xFFFF);
    
    out[0] = (dev24 >> 16) & 0xFF;
    out[1] = (dev24 >> 8) & 0xFF;
    out[2] = dev24 & 0xFF;
    
    printk("Device ID: 0x%02X%02X%02X\n", out[0], out[1], out[2]);
}

/* ============================================================================
 * AKTUALIZACJA RAMKI DANYCH
 * ============================================================================ */

/**
 * Odświeża wszystkie dane w strukturze sensor_frame
 * Kopiuje dane do manufacturer data dla advertising
 */
static void update_sensor_frame(void)
{
    // Odczytaj Device ID (tylko raz na początku, ale dla pewności każdorazowo)
    read_dev_id_24(frame.dev_id_3);
    
    // Timestamp (TODO: dodać real-time clock)
    frame.date_hour = k_uptime_get_32() / 1000; // uptime w sekundach
    
    // Odczyt czujników (na razie fake data)
    frame.temp_x10 = (int16_t)(fake_temp() * 10);
    frame.hum_x10  = (int16_t)(fake_hum() * 10);
    frame.lux      = fake_lux();
    frame.weight   = fake_weight();
    frame.in_bees  = fake_in();
    frame.out_bees = fake_out();
    frame.stat     = fake_stat();
    
    // Marker
    frame.pp[0] = 'P';
    frame.pp[1] = 'P';

    // Skopiuj strukturę do manufacturer data (dla advertising)
    memcpy(&mfg_data[2], &frame, sizeof(frame));

    printk("Dane zaktualizowane: T=%.1f°C, H=%.1f%%, Lux=%u, Waga=%ug, In=%u, Out=%u, Stat=%u\n",
           frame.temp_x10 / 10.0, frame.hum_x10 / 10.0, 
           frame.lux, frame.weight, frame.in_bees, frame.out_bees, frame.stat);
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
    printk("DEBUG: sizeof(sensor_frame_t) = %u\n", sizeof(struct sensor_frame_t));
    printk("DEBUG: sizeof(mfg_data) = %u\n", sizeof(mfg_data));
    printk("DEBUG: Total AD payload = %u bytes\n", 
           3 + sizeof(mfg_data) + 2);  // FLAGS + MFG_DATA + overhead
    
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
    ARG_UNUSED(seconds);

    printk("Dobranoc...\n");

    // BUTTON 1 = P0.11 (active low)
    NRF_GPIO->PIN_CNF[NRF_GPIO_PIN_MAP(0, 11)] =
        (GPIO_PIN_CNF_SENSE_Low << GPIO_PIN_CNF_SENSE_Pos) |
        (GPIO_PIN_CNF_PULL_Pullup << GPIO_PIN_CNF_PULL_Pos) |
        (GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos) |
        (GPIO_PIN_CNF_DIR_Input << GPIO_PIN_CNF_DIR_Pos);

    printk("SYSTEM OFF (HW)\n");

    // Bez żadnego sleep!
    NRF_POWER->RESETREAS = 0xFFFFFFFF;
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

    // Inicjalizacja przycisku (opcjonalna)
    button_setup();

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
        update_sensor_frame();
        
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
                update_sensor_frame();
                bt_le_adv_update_data(ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
            }
            
            k_sleep(K_MSEC(100));
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

 
