/**
 * mercury3sc: Mercury IIIS remote controller
 * Copyright (c) 2023 Kihwal Lee, K9SUL
 * 
 * It acts as man in the middle for the existing serial connection
 * between the internal Arduino Nano and the Nextion LCD.  The USB
 * serial port is used for control and status reporting.
 * 
 * It uses a HW serial on and AltSoftSerial because Teensy 2.0 has
 * only one HW serial port.
 * 
 * HW serial pins: 8(tx), 7(rx) - Serial1 - Nextion
 * Alt SW serial pins: 9(tx), 10(rx) - SW Serial - Merc III Nano
 *
 * PIN_B0 is connected to the gate of a 2N7000 for power on/off control
 * EEPROM address 0 stores the beep setting.
 */

#define M3S_BAUD 57600         // Mercury IIIS's internal baud rate
#define M3S_BUFF_SIZE 64       // Internal receive buffer size
#define M3S_LED 11
#define M3S_PCTL PIN_B0

#include <string.h>
#include <AltSoftSerial.h>
#include <EEPROM.h>

#define LCDSerial Serial1     // serial port for communicating with the Nextion LCD
AltSoftSerial CTLSerial;      // serial port for communicating with the onboad Arduino Nano

char buff[M3S_BUFF_SIZE];      // receiver buffer
char outb[128];               // send buffer
boolean dir = true;           // comm direction. Read from nextion when true.
boolean beep = true;          // whether to send a beep or not.
boolean debug = false;        // verbose output
int loop_count;

// Variables to keep track of the amp state.
int vol, cur, swr, ref, pwr, tmp;
boolean transmit = false;

// Prints to the USB serial port. Used to dump the captured commands
// Control characters are printed in hex.

void printUSB(char* buff, int len, boolean lcd) {  
  if (lcd) {
    Serial.print("> ");
  } else {
    Serial.print("< ");
  }

  for (int i = 0; i < len; i++) {
    char c = buff[i];
    if (c > 31 && c < 128) {
      Serial.print(c);
    } else {
      Serial.print("[");
      Serial.print((uint8_t)c, HEX);
      Serial.print("]");
    }
  }
  Serial.println(" ");
}

// Send a command to the amp controller
void sendCtrlMsg(const char* msg) {
  sprintf(outb,"%s%c%c%c", msg, 0xff, 0xff, 0xff);
  CTLSerial.print(outb);
}

// send a command to the LCD
void sendLcdMsg(const char* msg) {
  sprintf(outb,"%s%c%c%c", msg, 0xff, 0xff, 0xff);
  LCDSerial.print(outb);
}

// Does it end with the terminal sequence, 0xff 0xff 0xff?
// The bit pattern is 0xff, which shouldn't be confused with the value of
// a particular type.  E.g. 0xff in char is -1. 0xff in int is 255.
// Be careful with type casting and comparisons.
boolean term_seq(char* data, int len) {
  // false if the input is too short
  if (len < 3)
    return false;

  // examine the last three bytes.
  if (data[len-1] == -1 && data[len-2] == -1 && data[len-3] == -1) {
    return true;
  } else {
    return false;
  }
}

void resetLcdState() {
  sendLcdMsg("s.val=10");
  sendLcdMsg("c.val=0");
  sendLcdMsg("p.val=0");
  sendLcdMsg("r.val=0");    
}

// Parse and update the internal state if needed.
boolean updateState(char* buff, int len) {
  // Is it tx/rx mode indicator? oa.picc=1 (rx), oa.picc=2 (tx)
  if (len == 12) {
    if (!strncmp(buff, "oa.picc=1", 9)) {
      transmit = false;
      digitalWrite(M3S_LED, LOW);
      // Make sure display is reset correctly. During transmit, the high traffic
      // can cause random byte drops. If it happens at the end of transmission,
      // the display might be left in inconsistent state.
      sendLcdMsg("tsw 255,1");
      resetLcdState();
    } else if (!strncmp(buff, "oa.picc=2", 9)) {
      transmit = true;
      digitalWrite(M3S_LED, HIGH);
    } else if (!strncmp(buff, "tsw 255,1", 9) && transmit) {
      // "oa.picc=1" is always immediately followed by "tsw 255,1". If we see this
      // and still in transmit mode, it must mean "oa.picc=1" was lost. 
      transmit = false;
      digitalWrite(M3S_LED, LOW);
      sendLcdMsg("oa.picc=1");
      resetLcdState();
    }
  }
  
  // Is it in the form of "x.val="?
  if (buff[1] == '.' && buff[2] == 'v' && buff[3] == 'a' && buff[4] == 'l' && buff[5] == '=') {
    if (buff[6] == -1) { // 0xff terminator
      // no data after "=".
      return true;
    }

    // parse the integer string
    buff[len-3] = '\0'; // temporarily null terminated
    int val = atoi(buff + 6);
    buff[len-3] = -1; // restore 0xff
    switch(buff[0]) {
      case 'v':
        // skip bad voltages.
        // consider only 3 digit reports (> 10.0V) are valid.
        if (val < 100) return true;
        vol = val;
        break;
      case 'c':
        cur = val;
        break;
      case 's':
        if (val < 10) return true;
        swr = val;
        break;
      case 'r':
        ref = val;
        break;
      case 'p':
        pwr = val;
        break;
      case 't':
        tmp = val;
        break;
      default:
        break;
     }
     return false;
  }
  return false;
}

void printWithDecimal(int val) {
  Serial.print(val/10);
  Serial.print(".");
  Serial.println(val%10);
}

void printStatus(boolean human_readable) {
  if (human_readable) {
    Serial.print("Output Power   : ");
    printWithDecimal(pwr);
    Serial.print("Reflected Power: ");
    printWithDecimal(ref);
    Serial.print("SWR : ");
    printWithDecimal(swr);
    Serial.print("Drain Voltage  : ");
    printWithDecimal(vol);
    Serial.print("Drain Current  :");
    printWithDecimal(cur);
    Serial.print("Temperature(C) : ");
    Serial.println(tmp);
  } else {
    sprintf(outb, "%d %d %d %d %d %d", pwr, ref, swr, vol, cur, tmp);
    Serial.println(outb);
  }
}

// Update the band display on LCD.
//
// q6.picc to q12.picc are the thin lines under the each band button.
// The active one is set to 2 and 1 turns it off.  This is used in the
// auto switching mode.
//
// band0.val to band6.val are for the band buttons. 1 to select, 0 for off.
// band7.val is for the auto button, which is turned off whenever a band is
// selected by this controller.
void setLcdBand(int b) {
  // clear auto-selected band marker
  for (int i = 6; i <= 12; i++) {
    sprintf(outb, "q%d.picc=1%c%c%c", i, 0xff, 0xff, 0xff);
    LCDSerial.print(outb);
  }

  // Select the manual band button
  for (int i = 0; i <= 7; i++) {
    sprintf(outb, "band%d.val=%d%c%c%c", i, (i==b) ? 1:0 ,0xff, 0xff, 0xff);
    LCDSerial.print(outb);
  }
}

void setup() {
  Serial.begin(115200); // USB serial output
  LCDSerial.begin(M3S_BAUD);
  LCDSerial.setTimeout(1); // 1ms timeout
  CTLSerial.begin(M3S_BAUD);
  CTLSerial.setTimeout(1);
  
  pinMode(M3S_PCTL, OUTPUT);  // amp power control
  pinMode(M3S_LED,  OUTPUT);
  
  digitalWrite(M3S_LED,  LOW); // turn on the led
  digitalWrite(M3S_PCTL, LOW);  // amp off

  if (EEPROM.read(0) == 0x30) {
    beep = false;
  }
  if (EEPROM.read(1) == 0x30) {
    debug = true;
  }
  loop_count = 0;
}


void loop() {
  int c;
  int idx;
  unsigned long t;
  boolean terminated = false;
  
  // read one command at a time.
  idx = 0;
  t = millis();
  while (1) {
    // dir tells it to read from LCD or the controller. It alternates between
    // the two unless there are more data readily available in the current port.
    // This is happens a lot when transmitting. 
    c = (dir) ? LCDSerial.read() : CTLSerial.read();

    if (c != -1) {
      buff[idx] = (char)c;
      idx++;
      // check for the terminal condition
      if (term_seq(buff, idx)) {
        terminated = true;
        break;
      }      
    }
    // timeout, buffer full, or nothing read.
    if (idx == 0 || (millis() - t) > 10 || idx == M3S_BUFF_SIZE) {
      // Commands are much shorter than the buffer. If the buffer is full, it
      // means there is corruption/drop. In 10ms, about 60 chars can be sent at 57.6kbps.
      // A timeout means the terminating sequence will never come.  It is better to simply
      // drop it.
      break;
    }
  }

  // Relay, process and print the received command
  if (idx > 0 && terminated) {
    if (dir) {
      // We read from the LCD. Write it to the controller.
      int ecode = 0x1a;
      if (buff[0] == (char)ecode)
        return;
      CTLSerial.write(buff, idx);
    } else {
      if (updateState(buff, idx))
        return;  // bad record
      LCDSerial.write(buff, idx);
    }
    
    if (debug) {
      printUSB(buff, idx, dir);
    }
  }

  // intelligently switch between the sources. If the current source has
  // more data to read, stay with the source.
  loop_count++;
  if (dir && !LCDSerial.available()) {
    loop_count = 0;
    dir = false;
  } else if (!dir && (!CTLSerial.available() || loop_count > 10)) {
    loop_count = 0;
    dir = true;
  }

  // External command processing.
  // BPF selection: a 160, b 80, c 40, d 20, e 15, f 10, g 6, h auto
  // Ant selection: 1, 2, 3
  // reset: r
  // fan: j auto, k max
  // beep: s to toggle
  // status: t for human-readable format, u for short form
  // Verbose: v to toggle
  //
  // The ant is automatically set after a band switch. If a custom ant port
  // needs to be set, be sure to select an ant after setting the band.
  c = Serial.read();
  if (c != -1) {
    char cmd = (char)c;

    if (beep && cmd != 't' && cmd != 'u' && cmd != 'v')
      sendCtrlMsg("psound");
      
    switch(cmd) {
      // BPF selection
      case 'a':
        sendCtrlMsg("pdia=160");
        setLcdBand(0);
        break;
      case 'b':
        sendCtrlMsg("pdia=80");
        setLcdBand(1);
        break;
      case 'c':
        sendCtrlMsg("pdia=40");
        setLcdBand(2);
        break;
      case 'd':
        sendCtrlMsg("pdia=20");
        setLcdBand(3);
        break;
      case 'e':
        sendCtrlMsg("pdia=15");
        setLcdBand(4);
        break;
      case 'f':
        sendCtrlMsg("pdia=10");
        setLcdBand(5);
        break;
      case 'g':
        sendCtrlMsg("pdia=6");
        setLcdBand(6);
        break;
      case 'h':
        setLcdBand(7);
        sendCtrlMsg("pdia=255");
        break;

      // power on
      case 'p':
        digitalWrite(PIN_B0, HIGH);
        break;
      // power off
      case 'q':
        digitalWrite(PIN_B0, LOW);
        break;
        
      // reset
      case 'r':
        sendCtrlMsg("preset_main");
        break;

      // toggle beep
      case 's':
        beep = !beep;
        if (beep) {
          EEPROM.write(0, 0x00);
        } else {
          EEPROM.write(0, 0x30);
        }
        break;

      // status in human readable form
      case 't':
        printStatus(true);
        break;
        
      // raw status data
      case 'u':
        printStatus(false);
        break;

      // toggle debug
      case 'v':
        debug = !debug;
        Serial.print("Verbose mode ");
        Serial.println(debug ? "on":"off");
        if (debug) {
          EEPROM.write(1, 0x30);
        } else {
          EEPROM.write(1, 0x00);
        }
        break;
        
      // antenna selection
      case '1':
        sendCtrlMsg("ponant1");
        sendLcdMsg("ant1.val=1");
        sendLcdMsg("ant2.val=0");
        sendLcdMsg("ant3.val=0");
        break;
      case '2':
        sendCtrlMsg("ponant2");
        sendLcdMsg("ant1.val=0");
        sendLcdMsg("ant2.val=1");
        sendLcdMsg("ant3.val=0");
        break;
      case '3':
        sendCtrlMsg("ponant3");
        sendLcdMsg("ant1.val=0");
        sendLcdMsg("ant2.val=0");
        sendLcdMsg("ant3.val=1");
        break;

      // fan speed. LCD update is done by the controller.
      case 'j':
        sendCtrlMsg("pfanmin");
        break;
      case 'k':
        sendCtrlMsg("pfanmax");
        break;

      default:
        break;
    }
  }
}
