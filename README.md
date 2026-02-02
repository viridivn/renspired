# Renspired
<img src="https://github.com/user-attachments/assets/1153a938-a6e2-4621-a894-25f63113bd71" width="512">

Renspired is software that allows the TI-Nspire CX II (CAS) to access LLMs (Gemini or OpenAI-compatible) via an ESP32 board connected to the Nspire's docking connector UART. The ESP32 connects to a WiFi AP and calls the Gemini or an OpenAI-compatible LLM api.

This software is a proof of concept hobby project, and is not intended to be used for academic dishonesty. The creator of this project does not condone academic dishonesty and will not field questions assisting with it.

The ESP32 board used in the reference implementation of this project is an [ESP32-C3 board from AliExpress](https://www.aliexpress.us/item/3256807743067544.html), but it can be easily adapted to any ESP32 module. The ESP32 must be soldered to 3V3 and GND, as well as UART Tx and Rx on the Nspire's bottom dock connector. The pinout of this connector can be found on [Hackspire](https://hackspire.org/index.php/Hardware#Connector_J01_-_Dock_connector). In my testing, the Nspire power supply is strong enough to prevent the ESP32 from browning out, but your mileage may vary. It is best to keep the ESP32's USB port accessible for serial debugging. I simply removed the plastic shutter that normally covers the dock port to accomodate this. You may need to trim the inside of the Nspire's outer case, but it can remain intact.

Disassembling the Nspire is arduous. There are 4 Torx T6 screws on the back, with 2 underneath the top rubber feet. After that, a hair dryer must be used to loosen the adhesive holding the glossy faceplate on the top, and it must be pried off to reveal two small JIS screws. I simply removed these permanently and reapplied the faceplate, as they serve no purpose but to make opening the calculator harder.

Once all the screws are removed, the back case can be removed from the calculator and the ESP32 can be soldered. Use a fine tipped iron, ensure you have no shorts, and cover the ESP32 in Kapton to prevent it from shorting. Reassemble the unit.

Use the Arduino IDE to flash the ESP32. You will need the ArduinoJSON library. Remember edit the sketch to include your configuration details, such as WiFi information and API keys. The Nspire program requires [Ndless](https://ndless.me/) to be installed on the calculator, and requires the [Ndless SDK](https://hackspire.org/index.php/C_and_assembly_development_introduction) to build. Prebuilt binaries will not be provided to discourage cheating, and I suggest you do the same.

This software is licensed under GNU AGPLv3. More information can be found in the LICENSE file.
