Progress:
feat: implement weight calibration and kg conversion

- Add SW1 button interrupt to trigger the calibration process
- Implement raw data conversion to kilograms
- Save tare and calibration factor to NVS (Non-Volatile Storage)
- Add 5-second delays for stable weight readings