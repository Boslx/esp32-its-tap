# ESP32-C5 802.11p (ITS-G5) Frame Transmitter

> [!CAUTION]
> **5.9 GHz ITS spectrum is safety-critical and may require a license.**
> Transmitting without authorisation can interfere with real-world transportation systems and may be illegal in your country.
> In no event shall the authors or copyright holders be liable for any claim, damages or other liability, whether in an action of contract, tort or otherwise, arising from, out of or in connection with the software or the use or other dealings in the software.
>
> Check local regulations. Use at your own risk.

---

Example that transmits and receives ITS-G5 frames on channel 180 (5900 MHz) using undocumented Espressif PHY functions and a
reverse-engineered internal Wi-Fi TX/RX path on the **ESP32-C5**.

## Credits

This project is a fork/adaptation of [**TheEnbyperor/esp32-c-its**](https://github.com/TheEnbyperor/esp32-c-its).
The reverse-engineering of the internal Wi-Fi TX path, buffer structures, and undocumented function calls was performed by [TheEnbyperor](https://github.com/TheEnbyperor).