# 🐝 BeeNode - IoT Beehive Monitoring System

![Zephyr RTOS](https://img.shields.io/badge/Zephyr-RTOS-blue)
![nRF Connect SDK](https://img.shields.io/badge/nRF_Connect_SDK-v2.6.0-green)
![Bluetooth](https://img.shields.io/badge/Bluetooth-LE_5.0-0082FC)
![C](https://img.shields.io/badge/Language-C-A8B9CC)

BeeNode is a low-power, autonomous IoT node designed for real-time monitoring of beehives. Built on the **nRF52840** System-on-Chip using **Zephyr RTOS**, it tracks hive weight, ambient temperature, and battery life, transmitting the data via standardized Bluetooth Low Energy (BLE) GATT services.

## ✨ Key Features

* **Advanced Weight Measurement:** Reads data from an HX711 load cell amplifier. Features an intuitive 3-button physical interface for independent testing, zeroing (Tare), and weight calibration, with parameters permanently saved to Non-Volatile Storage (**NVS**).
* **Hardware-Accelerated 1-Wire (DS18B20):** Bypasses Zephyr's GPIO bit-banging limitations by routing 1-Wire communication through a hardware UART interface (`w1-serial` loopback). This ensures strict microsecond timing without being interrupted by BLE radio events.
* **Accurate Battery Telemetry:** Utilizes the SoC's hardware ADC to read battery voltage via a voltage divider. Implements an 11-sample averaging algorithm (discarding the first sample) for stable percentage and voltage mapping.
* **Standardized BLE GATT Services:** Fully compliant with Bluetooth SIG standards, allowing immediate recognition by apps like nRF Connect:
  * ⚖️ `0x181D` - Weight Scale Service
  * 🔋 `0x180F` - Battery Service
  * 🌡️ `0x181A` - Environmental Sensing (Temperature)
* **Ultra-Low Power Management:** Operates cyclically, utilizing Zephyr's Deep Sleep (System OFF) mode. Wakes up seamlessly via physical button presses utilizing hardware LATCH registers to immediately identify user intent, maximizing battery life in the field while remaining responsive.

## 🛠️ Hardware Requirements

* **Main Board:** Nordic nRF52840-DK (Migratable to Seeed Studio XIAO nRF52840)
* **Weight Sensor:** HX711 Load Cell Amplifier + Load Cell
* **Temperature Sensor:** DS18B20 (1-Wire) + 4.7kΩ pull-up resistor
* **Power:** Lithium-Ion Battery + Voltage divider (100kΩ / 100kΩ)

### Pin Configuration (nRF52840-DK)

| Component | Pin | Notes |
| :--- | :--- | :--- |
| **HX711 DT** | `P0.27` | Data line |
| **HX711 SCK** | `P0.26` | Clock line |
| **DS18B20 Data** | `P0.03` | Uses UART1 TX/RX internal loopback |
| **Battery ADC** | `P0.02` | Analog input (AIN0) |
| **TEST Button** | `P0.11` | (Button 1 / SW1) Wakes up and broadcasts a quick measurement |
| **TARE Button** | `P0.12` | (Button 2 / SW2) Captures the zero point |
| **CAL Button** | `P0.24` | (Button 3 / SW3) Calibrates with a known weight |
| **Status LED 1 (Green)** | `P0.13` | (LED 0) Test and Calibration indicator |
| **Status LED 2 (Red)** | `P0.14` | (LED 1) Tare status indicator |

## 🗄️ BLE GATT Architecture

The device acts as a BLE Peripheral. Once connected, a Central device (e.g., a smartphone or a Raspberry Pi) can read the following characteristics:

| Service Name | Service UUID | Characteristic UUID | Format |
| :--- | :--- | :--- | :--- |
| **Weight Scale** | `0x181D` | `0x2A9D` | 3 Bytes (1B Flags + 2B Weight in kg * 200) |
| **Battery Level** | `0x180F` | `0x2A19` | 1 Byte (0-100%) |
| **Battery Voltage** | `0x180F` | `0xFFF2` | 2 Bytes (Voltage in Volts * 100, Custom) |
| **Temperature** | `0x181A` | `0x2A6E` | 2 Bytes (Signed Int, °C * 100) |

## 🚀 Building and Flashing

This project is built using the **nRF Connect SDK (v2.6.0)** and `west` command-line tool.

1. Clone this repository.
2. Open your terminal in the project directory.
3. Build the project for the nRF52840-DK:
   ```bash
   west build -b nrf52840dk_nrf52840 --pristine
   ```
4. Flash the compiled firmware to the board:
   ```bash
   west flash
   ```

## ⚙️ How to Operate the Scale

The device remains in Deep Sleep (System OFF) mode to conserve battery. Pressing one of the physical buttons wakes it up, performs a specific action, and puts it immediately back to sleep.

### 🟢 1. TEST Button (Button 1 / P0.11)
Used for a quick manual measurement.
1. Press the **TEST** button.
2. The **Green LED** turns on for 1 second.
3. The device wakes up, takes readings (weight, battery, temperature), broadcasts data via BLE for 5 seconds, and then goes back to sleep.

### 🔴 2. TARE Button (Button 2 / P0.12)
Used to set the zero point of the scale (e.g., empty hive).
1. Ensure the scale is completely empty.
2. Press the **TARE** button.
3. The **Red LED** turns on for ~1 second.
4. The device captures the new zero point, saves it to persistent memory (NVS), and goes back to sleep.

### 🟢⏳ 3. CAL Button (Button 3 / P0.24)
Used to calibrate the scaling factor with a known weight (default configured to `0.1835 kg`).
1. Have your known weight ready.
2. Press the **CAL** button.
3. **Phase 1 (Preparation):** The **Green LED** will blink rapidly for 10 seconds. Use this time to carefully place the weight on the scale and step back.
4. **Phase 2 (Calibration):** The blinking stops and the **Green LED** stays on solid for 2 seconds. The device reads the weight, calculates the scaling factor, saves it to NVS, and goes back to sleep.

## 🧠 Development Notes & AI Integration

This project was developed with a heavy emphasis on **AI-Assisted Engineering**. Generative AI tools (LLMs) were actively utilized to architect the BLE GATT structure, write custom Zephyr DeviceTree overlays, and rapidly debug complex RTOS timing constraints (such as the 1-Wire UART workaround). This approach significantly reduced development cycles and ensured strict adherence to official Bluetooth specifications.
