# MIMIC Mini - IR Cloner

MIMIC Mini is a compact infrared (IR) cloner and universal remote device. It is capable of reliably transmitting IR signals at ranges of up to 10–15 meters. It is fully compatible with Flipper Zero `.ir` files and provides up to 3 days of battery life under normal usage. Equipped with microSD card storage and an integrated receiver unit, it can record and save captured signals in both raw `.txt` and standard `.ir` formats.

---

## 🛠 Features

* **Extended Range:** Transmits IR signals up to 10–15 meters.
* **Flipper Zero Compatibility:** Natively supports `.ir` signal files.
* **Long Battery Life:** Up to 3 days of standard operational time.
* **Expandable Storage:** MicroSD card support for storing large databases of IR codes.
* **Signal Capture:** Built-in receiver capable of capturing and saving raw TXT or structured IR formats.

---

## 🏗 Housing & Assembly Instructions

1. **Belt Clip Attachment:**
   * The bill of materials (BOM) does not include the 3mm press-fit brass inserts or corresponding screws. 
   * The belt clip mount should be glued directly to the back of the enclosure for easy belt attachment.

2. **Display Insulation (CRITICAL):**
   * Before inserting the display, **apply insulating adhesive tape (e.g., Kapton or electrical tape) to the back of the display board**.
   * *Warning:* Failure to insulate the back of the display may cause a short circuit against the MicroSD card socket.

3. **IR LED Alignment:**
   * Pay close attention to the positioning and angles of the IR LEDs. 
   * If all LEDs are pointing straight forward, the transmission beam spread will be reduced, making it harder to aim at target devices. Angle them slightly outward to maximize signal coverage.

4. **Battery & Power Switch Wiring:**
   * Solder the battery directly to the `BAT` pads on the bottom of the ESP32-C3 board.
   * Wire the power switch in series with the positive battery lead to break the circuit.
   * Glue the power switch to the side of the PCB as demonstrated in the assembly photos.

---

## 📁 SD Card Structure & Usage

Upon inserting an SD card for the first time, the device will automatically generate three directories:
### 1. `saved_ir/`
* Captured IR signals are saved here. You can set the target capture format (`.ir` or `.txt`) in the device settings menu.
* Files in this folder can be renamed freely.
* If you place a multi-code `.ir` file in this folder, opening it on the device will display a sub-menu containing each individual code, allowing you to trigger them selectively one by one.

### 2. `universal/`
* If a multi-code `.ir` file is placed in the `universal` directory, the device will sequentially play back **all** IR codes inside the file one after another (e.g., universal power-off sequence). You will not be able to select individual codes in this mode.

---

## ⚙️ Settings

* **Transmitter Selection:** Through the system settings, you can toggle between using the internal IR transmitter array or an external IR module/emitter.
