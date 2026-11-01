# ChangeLog

## v0.2.0 - 2026-07-15

* Added support for ESP-IDF v6.0 to v6.2 and ESP32-S31
* **BREAKING CHANGE:** Removed support for the legacy I2C driver configuration
* Added ESP32-P4 revision-specific prebuilt library selection for Rev 1 and Rev 3
* Updated CI build and package rules for ESP-IDF v6.0 to v6.2, ESP32-S31, and ESP32-P4 Rev 3
* Updated examples to use local component overrides and explicit default CI profiles

## v0.1.1 - 2025-12-31

* Optimize FreeRTOS tick rate detection to avoid timing issue

## v0.1.0 - 2025-10-19

* Add initial version of the BMI270 sensor component
