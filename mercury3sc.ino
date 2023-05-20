/**
 * mercury3sc: Mercury IIIS remote controller
 * Copyright (c) 2023 Kihwal Lee, K9SUL
 * 
 * It acts as man in the middle for the existing serial connection
 * between the internal Arduino Nano and the Nextion LCD.  The USB
 * serial port is used for control and status reporting.
 * 
 * The code was written for Raspberry PI Pico based on the RP2040,
 * which has two UARTs, two processor cores and a USB. Since it is 
 * a 3.3V device, a level conversion for the existing 5V system is
 * needed. The pico is powered via the M3s's 5V supply or the USB.
 * See Fig 16 on page 20 of the official pico manual.
 * 
 * Serial ports
 * Serial  : ACM USB serial port
 * Serial1 : uart0 - GP0(tx), GP1(rx) - Nextion display
 * Serial2 : uart1 - GP4(tx), GP5(rx) - M3s Nano
 *
 * GP7 power on/off control. 
 * GP9 input attenuator level select.
 * GP25 LED
 * 
 * EEPROM 
 * addr 0 stores the beep setting.
 * addr 1 stores verbose mode seting.
 * 
 * Dual core
 * - core0 takes care of USB. Responsible for receiving data from the 
 *   LCD and sending to the controller. Takes care of user commands.
 * - core1 receives data from the controller and relays them to the LCD.
 */

#define M3S_BAUD 57600         // Mercury IIIS's internal baud rate
#define M3S_BUFF_SIZE 256       // Internal receive buffer size
#define M3S_LED 25             // on-board LED pin
#define M3S_PCTL 7             // Power on/off control pin
#define M3S_ATTN 9             // Attenuator control pin
#define M3S_ST_WINDOW 4        // outlier drop window size

#include <string.h>
#include <EEPROM.h>

#define LCDSerial Serial1      // serial port for communicating with the Nextion LCD
#define CTLSerial Serial2      // serial port for communicating with the onboad Arduino Nano

char buff[M3S_BUFF_SIZE];      // receiver buffer for the LCD
char buff1[M3S_BUFF_SIZE];     // receiver buffer for the controller
boolean beep = false;           // whether to send a beep or not.
boolean debug = false;         // verbose output
boolean initialized = false;   // init condition indicator to block core1
volatile int usbLocked = 0;

// Variables to keep track of the amp state. Each state keeps a history of the length
// defined by M3S_ST_WINDOW. If a newly added value is an outlier, it will still be
// recorded, but won't be relayed to the LCD.
//
// vol[M3S_ST_WINDOW] contains the curent head index
// vol[M3S_ST_WINDOW + 1] contains last known good value
int vol[M3S_ST_WINDOW+2], cur[M3S_ST_WINDOW+2], swr[M3S_ST_WINDOW+2];
int ref[M3S_ST_WINDOW+2], pwr[M3S_ST_WINDOW+2], tmp[M3S_ST_WINDOW+2];
boolean transmit = false;

// Prints to the USB serial port. Used to dump the captured commands
// Control characters are printed in hex.
void printUSB(char* buff, int len, boolean lcd) {
  // Spin lock for usb output
  // This is to avoid the two cores mixing characters in verbose mode.
  // Due to extra wait time, there might be more random data drops in
  // verbose mode.
  while (usbLocked) { }
  usbLocked = 1;
  
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
      // For non printable chars.
      Serial.print("[");
      Serial.print((uint8_t)c, HEX);
      Serial.print("]");
    }
  }
  Serial.println(" ");
  usbLocked = 0;
}

void printHelp() {
  Serial.println("BPF selection: a 160m, b 80m, c 60/40, d 30/20, e 17/15, f 12/10, g 6, h auto");
  Serial.println("ANT selection: 1, 2, 3");
  Serial.println("Reset: r");
  Serial.println("Fan  : j auto, k max");
  Serial.println("Beep : s to toggle");
  Serial.println("Status: t for human-readable format, u for short form");
  Serial.println("Verbose: v to toggle");
  Serial.println("Power on/off: p/q (normally off)");
  Serial.println("Attenuator on/off: y/x (normally on)");
}

// Send a command to the nano controller
void sendCtrlMsg(const char* msg) {
  char outb[32];
  sprintf(outb,"%s%c%c%c", msg, 0xff, 0xff, 0xff);
  CTLSerial.print(outb);
}

// send a command to the LCD
void sendLcdMsg(const char* msg) {
  char outb[32];
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
  if (data[len-1] == 0xff && data[len-2] == 0xff && data[len-3] == 0xff) {
    return true;
  } else {
    return false;
  }
}

// Reset the Nextion LCD after transitioning from TX to RX. This is to clear
// any inconsistent updates during TX.
void resetLcdState() {
  sendLcdMsg("s.val=10");
  sendLcdMsg("c.val=0");
  sendLcdMsg("p.val=0");
  sendLcdMsg("r.val=0");
  swr[M3S_ST_WINDOW + 1] = 10;
  pwr[M3S_ST_WINDOW + 1] = 0;
  cur[M3S_ST_WINDOW + 1] = 0;
  ref[M3S_ST_WINDOW + 1] = 0;
}

// Add a new value to the array.
boolean addVal(int st[], int val) {
  boolean isGoodVal = true;
  
  int cidx = st[M3S_ST_WINDOW]; // last element is used for current head index
  // is it a good value to report?
  for (int i = 0; i < M3S_ST_WINDOW; i++) {
    if (i == cidx)
      continue; // this is the oldest val that will be replaced.
    int diff = (st[i] > val) ? (st[i] - val) : (val - st[i]);
    // It is an outliner if more than +/- 25%
    if (diff > st[i]/4) {
      isGoodVal = false; // this is an outlier
      break;
    }
  }
  // save the value and update the index
  st[cidx] = val;
  st[M3S_ST_WINDOW] = (cidx + 1) % M3S_ST_WINDOW;
  if (isGoodVal) {
    st[M3S_ST_WINDOW + 1] = val;
  }
  return isGoodVal;
}

void addValNoCheck(int st[], int val) {
  // save the value and update the index
  int cidx = st[M3S_ST_WINDOW];
  st[cidx] = val;
  st[M3S_ST_WINDOW] = (cidx + 1) % M3S_ST_WINDOW;
  st[M3S_ST_WINDOW + 1] = val;
}

// Parse and update the internal state if needed.
// returns true if the record is to be reported.
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
      return false; // discard without updating
    }

    // parse the integer string
    buff[len-3] = '\0'; // temporarily null terminated
    int val = atoi(buff + 6);
    buff[len-3] = -1; // restore 0xff
    switch(buff[0]) {
      case 'v':
        // skip bad voltages.
        // consider only 3 digit reports (> 10.0V) are valid.
        if (val < 100) return false;
        if (transmit)
          return addVal(vol, val);
        addValNoCheck(vol, val);
        break;
      case 'c':
        if (transmit)
          return addVal(cur, val);
        addValNoCheck(cur, val);
        break;
      case 's':
        if (val < 10) return false;
        if (transmit)
          return addVal(swr, val);
        addValNoCheck(swr, val);
        break;
      case 'r':
        if (transmit)
          return addVal(ref, val);
        addValNoCheck(ref, val);
        break;
      case 'p':
        if (transmit)
          return addVal(pwr, val);
        addValNoCheck(pwr, val);
        break;
      case 't':
        if (transmit)
          return addVal(tmp, val);
        addValNoCheck(tmp, val);
        break;
      default:
        break;
     }
     return true;
  }
  return true;
}

void printWithDecimal(int val) {
  Serial.print(val/10);
  Serial.print(".");
  Serial.println(val%10);
}

void printStatus(boolean human_readable) {
  if (human_readable) {
    Serial.print("Output Power   : ");
    printWithDecimal(pwr[M3S_ST_WINDOW + 1]);
    Serial.print("Reflected Power: ");
    printWithDecimal(ref[M3S_ST_WINDOW + 1]);
    Serial.print("SWR : ");
    printWithDecimal(swr[M3S_ST_WINDOW + 1]);
    Serial.print("Drain Voltage  : ");
    printWithDecimal(vol[M3S_ST_WINDOW + 1]);
    Serial.print("Drain Current  :");
    printWithDecimal(cur[M3S_ST_WINDOW + 1]);
    Serial.print("Temperature(C) : ");
    Serial.println(tmp[M3S_ST_WINDOW + 1]);
  } else {
    char outb[32];
    sprintf(outb, "%d %d %d %d %d %d", pwr[M3S_ST_WINDOW + 1], ref[M3S_ST_WINDOW + 1],
        swr[M3S_ST_WINDOW + 1], vol[M3S_ST_WINDOW + 1],
        cur[M3S_ST_WINDOW + 1], tmp[M3S_ST_WINDOW + 1]);
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
  char outb[32];
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
  Serial.begin(115200); // USB serial output. the speed has no meaning.
  LCDSerial.setRX(1);
  LCDSerial.setTX(0);
  LCDSerial.setTimeout(1); // 1ms timeout
  LCDSerial.begin(M3S_BAUD);

  CTLSerial.setRX(5);
  CTLSerial.setTX(4);
  CTLSerial.setTimeout(1);
  CTLSerial.setFIFOSize(128); // sensor updates come at full speed on tx
  CTLSerial.begin(M3S_BAUD);

  pinMode(M3S_PCTL, OUTPUT);  // amp power control
  pinMode(M3S_ATTN, OUTPUT); 
  pinMode(M3S_LED,  OUTPUT);

  digitalWrite(M3S_PCTL, LOW);  // amp off
  digitalWrite(M3S_ATTN, LOW);  // attn on

  if (EEPROM.read(0) == 0x30) {
    beep = false;
  }
  if (EEPROM.read(1) == 0x30) {
    debug = true;
  }

  // init the state storage.
  for (int i = 0; i < M3S_ST_WINDOW + 2; i++) {
    vol[i] = cur[i] = swr[i] = ref[i] = pwr[i] = tmp[i] = 0;
  }
  
  // signal core1 to continue
  initialized = true;
}

void setup1() {
  // block until core0 init is done.
  while(!initialized) {
    delay(1);
  }
  digitalWrite(M3S_LED, LOW);
}

// core 1
// Read from the Nano controller. The traffic is very high during transmissions.
void loop1() {
  int c, idx;
  unsigned long t;

  // read one command at a time from the M3S Nano controller.
  idx = 0;
  t = millis();
  while (1) {
    c = CTLSerial.read();
    if (c != -1) {
      buff1[idx] = (char)c;
      idx++;
      
      // check for the terminal condition
      if (term_seq(buff1, idx)) {
        break;
      }
    }
    // timeout, buffer full, or nothing read.
    if (idx == 0 || (millis() - t) > 10 || idx == M3S_BUFF_SIZE) {
      // Commands are much shorter than the buffer. If the buffer is full, it
      // means there is corruption/drop. In 10ms, about 60 chars can be sent at 57.6kbps.
      // A timeout means the terminating sequence will never come.  It is better to simply
      // drop it.
      return;
    }
  }

  // update internal state. skip if bad.
  if (updateState(buff1, idx))
    LCDSerial.write(buff1, idx);
    
  if (debug) {
    printUSB(buff1, idx, false);
  }
}

// core 0
// - Read from the LCD UART, which does not produce much traffic.
// - USB Serial is processed by this core, between loop().
// - Process user commands from USB serial.
void loop() {
  int c;
  int idx;
  unsigned long t;

  // read one command at a time.
  idx = 0;
  t = millis();
  while (1) {
    c = LCDSerial.read();

    if (c != -1) {
      buff[idx] = (char)c;
      idx++;
      // check for the terminal condition
      if (term_seq(buff, idx)) {
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
  if (idx > 0) {
    CTLSerial.write(buff, idx);
    if (debug) {
      printUSB(buff, idx, true);
    }
  }

  // External command processing.
  // BPF selection: a 160, b 80, c 40, d 20, e 15, f 10, g 6, h auto
  // Ant selection: 1, 2, 3
  // reset: r
  // fan: j auto, k max
  // beep: s to toggle
  // status: t for human-readable format, u for short form
  // Verbose: v to toggle
  // power on/off: p/q (normally off)
  // attn on/off: y/x (normally on)
  //
  // The ant is automatically set after a band switch. If a custom ant port
  // needs to be set, be sure to select an ant after setting the band.
  if (Serial.available()) {
    c = Serial.read();
    if (c == -1) {
      return;
    }

    if (beep && c != 't' && c != 'u' && c != 'v')
      sendCtrlMsg("psound");

    switch(c) {
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
      case 'i':
        printHelp();
        break;

      // power on
      case 'p':
        digitalWrite(M3S_PCTL, HIGH);
        break;
      // power off
      case 'q':
        digitalWrite(M3S_PCTL, LOW);
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

      // attn off
      case 'x':
        digitalWrite(M3S_ATTN, HIGH);
        break;
      // attn on
      case 'y':
        digitalWrite(M3S_ATTN, LOW);
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
