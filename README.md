# ESP32-C5 802.11p (ITS-G5) Frame Transmitter

> [!CAUTION]
> **SAFETY-CRITICAL & LEGAL NOTICE**
>
> 5.9 GHz ITS (Intelligent Transport System) channels are **safety-critical** spectrum allocated for vehicular
> communications. **Transmitting at 5.9 GHz is illegal in most countries without a
> license** and can interfere with real-world transportation safety systems.
>
> - Check your country's laws before using this software.
> - **Use this at your own risk. The author accepts no liability for any misuse or damage.**

> [!WARNING]
> **HARDWARE LIMITATION — 20 MHz vs 10 MHz Channels**
>
> ETSI 802.11p (ITS-G5) specifies **10 MHz** channel bandwidth. The ESP32-C5 PHY is a **20 MHz OFDM design**.
> It is uncertain whether `phy_11p_set(1, 0)` produces a waveform that real 802.11p front-ends can demodulate.
> **At best this is a 20-MHz Wi-Fi-shaped frame on a 5.9 GHz channel that *some* 11p radios may accept.**
> Hardware testing with actual 802.11p/ITS-G5 equipment is required to validate interoperability.

---

Example that transmits ITS-G5 frames on channel 180 (5900 MHz) using undocumented Espressif PHY functions and a
reverse-engineered internal Wi-Fi TX path on the **ESP32-C5**.

## Credits

This project is a fork/adaptation of [**TheEnbyperor/esp32-c-its**](https://github.com/TheEnbyperor/esp32-c-its).
The reverse-engineering of the internal Wi-Fi TX path, buffer structures, and undocumented function calls was performed by TheEnbyperor.