# Wi-Fi Host Driver Expansion (WHDE) 1.0.10 
Please refer to the [README File](./README.md) and the [WHD API Reference Manual](https://infineon.github.io/wifi-host-driver/html/index.html) for a complete description of the Wi-Fi Host Driver.

### WIFI6 Supported Chip v4.3.0
* Supports Wi-Fi Station (STA) and AP mode of operation
* Supports multiple security methods such as WPA2, WPA3, and open

### New Features
* CYW955913 Hosted mode shutdown/wake feature
* CYW955913 CSI Support added

### Defect Fixes

### Known Issues
NA

#### CYW55500
* --- 28.10.522.8 ---

#### CYW55900
* --- 28.10.400.2 ---


### WIFI5 Suppported Chip v3.3.3

* Supports concurrent operation of STA and AP interface
* Supports low-power offloads like ARP, packet filters, TCP Keepalive offload
* Includes WFA pre-certification support for 802.11n, 802.11ac
* Provides API functions for ARP, packet filters
* Provides functions for Advanced Power Management

### New Features
* TLS-1.3 Support for 43022

### Defect Fixes
* Fix Dcache issue on XMC

### Known Issues
NA

#### CYW4343W
* --- 7.45.98.120 ---

#### CYW43012
* --- 13.10.271.305 ---

#### CYW4373
* --- 13.10.246.321 ---

#### CYW43439
* --- 7.95.88 ---

#### CYW43909
* --- 7.15.168.163 ---

#### CYW43022
* --- 13.67.10 ---


Note: [r] is regulatory-related

## Supported Software and Tools
This version of the WHD was validated for compatibility with the following software and tools:

| Software and Tools                                      | Version      |
| :---                                                    | :----        |
| GCC Compiler                                            | 10.3         |
| IAR Compiler                                            | 9.50         |
| Arm Compiler 6                                          | 6.22         |
| Mbed OS                                                 | 6.2.0        |
| ThreadX/NetX-Duo                                        | 5.8          |
| FreeRTOS/LWIP                                           | 2.1.2        |


## More Information
* [Wi-Fi Host Driver README File](./README.md)
* [Wi-Fi Host Driver API Reference Manual and Porting Guide](https://infineon.github.io/wifi-host-driver/html/index.html)
* [Infineon Technologies](http://www.infineon.com)

---
© Infineon Technologies, 2019.
