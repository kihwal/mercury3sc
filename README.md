# mercury3sc: Mercury IIIS Controller
## Overview
mercury3sc acts as a man-in-the-middle for the existing serial connection
between the internal Arduino Nano and the Nextion LCD.  The USB
serial port is used for control and status reporting.

## Harware
It was written for the Teensy 2.0, but can be ported to other controllers.
Although PJRC stopped making the Teensy 2.0, the clones can still be purchased.
For communicating with the internal controller and the LCD, you need two
serial ports. For external/remote communication the built-in USB serial
port is used. Since the Teensy 2.0 only has one harware serial port, 
AltSoftSerial is used to emulate one.

This is how I wired it.
| Teensy pins | Mercury LCD connector |
| --- | --- |
| 8 (hw tx) | LCD pin 2 |
| 7 (hw rx) | LCD pin 3 |
| 9 (sw tx) | Control board pin 3 |
| 10 (sw rx) | Control board pin 2 |

For the power on/off control, PIN_B0 is used. This is connected to the gate of 
a 2N7000. The 53.6V switching power supply is turned on by grounding the slot
detection pin of the supply. The voltage on this pin is measured about 3.3V.
The Mercury III's power switch grounds this pin. Connect this pin to the drain
of the 2N7000 and the source of the 2N7000 to ground. If you make the switch
and the FET parallel, you will still be able to turn on the amp using the power
switch.

The Teensy pin PIN_B1 is connected to another 2N7000 for extra control. I use
it for switching the input attenuator. I have modified the input attenuator to
better accommodate low power transmitters. By default the attenuation is about
17dB, just like the standard factory configuration. When the switching relay is
energized, it shorts the 10dB attenuator, requiring lower drive power.

## Control commands
mercury3sc was written on Arduino 1.8 with the Teensy support package from PJRC.
You can issue commands through the USB-serial interface.

| Command | Function |
| --- | --- |
| a | Select BPF for 160m |
| b | Select BPF for 80m |
| c | Select BPF for 40m |
| d | Select BPF for 20m |
| e | Select BPF for 15m |
| f | Select BPF for 10m |
| g | Select BPF for 6m |
| h | Enter auto detect mode |
| i | Help |
| j | Fan normal |
| k | Fan max |
| p | Power on |
| q | Power off |
| r | reset |
| s | Toggle beep |
| t | Dump status (human readable) |
| u | Dump status (short form) |
| v | Toggle verbos mode |
| x | Attenuator off |
| y | Attenuator on (default) |
| 1 | Select ant 1 |
| 2 | Select ant 2 |
| 3 | Select ant 3 |

Please note that a band selection causes the internal controller to issue an antenna
selection command based on the settings. If you want a different antenna for the band,
you need to issue an antenna select command after changing the band.

Beep and verbose mode selections are saved to EEPROM.

## Notes on RP2040, Raspberry PI Pico

The code is in the rp2040 branch. Since it is a 3.3V device, level converters are 
needed for the four UART signal lines. For the digital outputs, the default low
power output won't be enough to drive most cirscuits. The 12mA pad is selected.

