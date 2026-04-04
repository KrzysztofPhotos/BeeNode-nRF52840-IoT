# 🐝 BeeNode - IoT Beehive Monitoring System

![Zephyr RTOS](https://img.shields.io/badge/Zephyr-RTOS-blue)
![nRF Connect SDK](https://img.shields.io/badge/nRF_Connect_SDK-v2.6.0-green)
![Bluetooth](https://img.shields.io/badge/Bluetooth-LE_5.0-0082FC)
![C](https://img.shields.io/badge/Language-C-A8B9CC)

BeeNode is a low-power, autonomous IoT node designed for real-time monitoring of beehives. Built on the **nRF52840** System-on-Chip using **Zephyr RTOS**, it tracks hive weight, ambient temperature, and battery life, transmitting the data via standardized Bluetooth Low Energy (BLE) GATT services.

## ✨ Key Features

* **Advanced Weight Measurement:** Reads data from an HX711 load cell amplifier. Features on-the-fly calibration (Tare and Scaling Factor) triggered by a physical button, with parameters permanently saved to Non-Volatile Storage (**NVS**).
* **Hardware-Accelerated 1-Wire (DS18B20):** Bypasses Zephyr's GPIO bit-banging limitations by routing 1-Wire communication through a hardware UART interface (`w1-serial` loopback). This ensures strict microsecond timing without being interrupted by BLE radio events.
* **Accurate Battery Telemetry:** Utilizes the SoC's hardware ADC to read battery voltage via a voltage divider. Implements an 11-sample averaging algorithm (discarding the first sample) for stable percentage and voltage mapping.
* **Standardized BLE GATT Services:** Fully compliant with Bluetooth SIG standards, allowing immediate recognition by apps like nRF Connect:
  * ⚖️ `0x181D` - Weight Scale Service
  * 🔋 `0x180F` - Battery Service
  * 🌡️ `0x181A` - Environmental Sensing (Temperature)
* **Ultra-Low Power Management:** Operates cyclically, utilizing Zephyr's Deep Sleep (System OFF) mode and waking up via RTC to broadcast data, maximizing battery life in the field.

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
| **Calibration Button** | `P0.11` | (Button 1) Triggers Tare/Calibration |
| **Status LED** | `P0.13` | (LED 1) Calibration status indicator |

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

## ⚙️ How to Calibrate the Scale

1. Power on the device. Wait for the initial advertising phase.
2. Press **Button 1** to enter Calibration Mode.
3. **Phase 1 (Tare):** The LED will blink slowly. Ensure the scale is completely empty. You have 5 seconds. The system will calculate and save the Tare offset to NVS.
4. **Phase 2 (Scale Factor):** The LED will blink fast. Place a known weight (configured in code, default `0.1835 kg`) on the scale. You have 5 seconds. The system will calculate the scaling factor and save it to NVS.
5. The LED will turn off, and the scale is now ready for accurate BLE broadcasting.

## 🧠 Development Notes & AI Integration

This project was developed with a heavy emphasis on **AI-Assisted Engineering**. Generative AI tools (LLMs) were actively utilized to architect the BLE GATT structure, write custom Zephyr DeviceTree overlays, and rapidly debug complex RTOS timing constraints (such as the 1-Wire UART workaround). This approach significantly reduced development cycles and ensured strict adherence to official Bluetooth specifications.
