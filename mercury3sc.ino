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
#define M3S_LED 11             // LDE pin
#define M3S_PCTL PIN_B0        // Power on/off io pin
#define M3S_ATTN PIN_B1        // Attenuator relay control (K9SUL custom)
#define M3S_ST_WINDOW 3        // outlier drop window size
#define M3S_ST_VAL    4        // M3S_ST_WINDOW + 1

#include <string.h>
#include <AltSoftSerial.h>
#include <EEPROM.h>

#define LCDSerial Serial1      // serial port for communicating with the Nextion LCD
AltSoftSerial CTLSerial;       // serial port for communicating with the onboad Arduino Nano

const char M3S_TERM = 0xff;
char buff[M3S_BUFF_SIZE];      // receiver buffer
char outb[32];                 // send buffer

boolean dir = true;            // comm direction. Read from nextion when true.
boolean beep = false;          // whether to send a beep or not.
boolean debug = false;         // verbose output
boolean power = false;
boolean attn = true;
boolean transmit = false;
uint8_t loop_count = 0;
uint8_t band = 10;
uint8_t ant = 1;

// Variables to keep track of the amp state. Each state keeps a history of the length
// defined by M3S_ST_WINDOW. If a newly added value is an outlier, it will still be
// recorded, but won't be relayed to the LCD.
//
// vol[M3S_ST_WINDOW] contains the curent head index
// vol[M3S_ST_WINDOW + 1] contains last known good value
int vol[M3S_ST_WINDOW+2], cur[M3S_ST_WINDOW+2], swr[M3S_ST_WINDOW+2];
int ref[M3S_ST_WINDOW+2], pwr[M3S_ST_WINDOW+2], tmp[M3S_ST_WINDOW+2];

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
      // For non printable chars.
      Serial.print("[");
      Serial.print((uint8_t)c, HEX);
      Serial.print("]");
    }
  }
  Serial.println(" ");
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
  sprintf(outb,"%s%c%c%c", msg, M3S_TERM, M3S_TERM, M3S_TERM);
  CTLSerial.print(outb);
}

// send a command to the LCD
void sendLcdMsg(const char* msg) {
  sprintf(outb,"%s%c%c%c", msg, M3S_TERM, M3S_TERM, M3S_TERM);
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
  if (data[len-1] == M3S_TERM && data[len-2] == M3S_TERM && data[len-3] == M3S_TERM) {
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
    st[M3S_ST_VAL] = val;
  }
  return isGoodVal;
}

void addValNoCheck(int st[], int val) {
  // save the value and update the index
  int cidx = st[M3S_ST_WINDOW];
  st[cidx] = val;
  st[M3S_ST_WINDOW] = (cidx + 1) % M3S_ST_WINDOW;
  st[M3S_ST_VAL] = val;
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
      return true;
    } else if (!strncmp(buff, "oa.picc=2", 9)) {
      transmit = true;
      digitalWrite(M3S_LED, HIGH);
      return true;
    } else if (!strncmp(buff, "tsw 255,1", 9) && transmit) {
      // "oa.picc=1" is always immediately followed by "tsw 255,1". If we see this
      // and still in transmit mode, it must mean "oa.picc=1" was lost. 
      transmit = false;
      digitalWrite(M3S_LED, LOW);
      sendLcdMsg("oa.picc=1");
      resetLcdState();
      return true;
    }
  }

  // antenna state update
  if (len == 13) {
    if (!strncmp(buff, "ant1.val=1", 10)) {
      ant = 1;
      return true;
    } else if (!strncmp(buff, "ant2.val=1", 10)) {
      ant = 2;
      return true;
    } else if (!strncmp(buff, "ant3.val=1", 10)) {
      ant = 3;
      return true;
    }
  }
  
  // Is it in the form of "x.val="?
  if (buff[1] == '.' && buff[2] == 'v' && buff[3] == 'a' && buff[4] == 'l' && buff[5] == '=') {
    if (buff[6] == M3S_TERM) { // 0xff terminator
      // no data after "=".
      return false; // discard without updating
    }

    // parse the integer string
    buff[len-3] = '\0'; // temporarily null terminated
    int val = atoi(buff + 6);
    buff[len-3] = M3S_TERM; // restore 0xff
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

void printStatus(boolean human_readable) {
  if (human_readable) {
    Serial.print("Power          : ");
    Serial.println((power) ? "on":"off");
    Serial.print("Attenuator   : ");
    Serial.println((attn) ? "on":"off");
    Serial.print("Transmit       : ");
    Serial.println((transmit) ? "yes":"no");
    Serial.print("Band (auto=0)  : ");
    Serial.println(band);
    Serial.print("Antenna        : ");
    Serial.println(ant);
    Serial.print("Temperature(C) : ");
    Serial.println(tmp[M3S_ST_VAL]);
#ifdef M3S_SHOW_RAW_VALS
    // The power levels and the drain current are translated in the
    // display. The power level conversion is clearly non-linear.
    // SWR and Voltage are straightforward 10x values.
    Serial.print("Output Power   : ");
    Serial.println(pwr[M3S_ST_VAL]);
    Serial.print("Reflected Power: ");
    Serial.println(ref[M3S_ST_VAL]);
    Serial.print("SWR : ");
    Serial.println(swr[M3S_ST_VAL]);
    Serial.print("Drain Voltage  : ");
    Serial.println(vol[M3S_ST_VAL]);
    Serial.print("Drain Current  :");
    Serial.println(cur[M3S_ST_VAL]);
#endif
  } else {
    sprintf(outb, "%d %d %d %d %d %d",
        (power) ? 1:0,
        (attn) ? 1:0,
        (transmit) ? 1:0,
        band,
        ant,
        tmp[M3S_ST_VAL]);
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
    sprintf(outb, "q%d.picc=1%c%c%c", i, M3S_TERM, M3S_TERM, M3S_TERM);
    LCDSerial.print(outb);
  }

  // Select the manual band button
  for (int i = 0; i <= 7; i++) {
    sprintf(outb, "band%d.val=%d%c%c%c", i, (i==b) ? 1:0 ,M3S_TERM, M3S_TERM, M3S_TERM);
    LCDSerial.print(outb);
  }
}


void setup() {
  Serial.begin(115200); // USB serial output. the speed has no meaning.
  LCDSerial.setTimeout(1); // 1ms timeout
  LCDSerial.begin(M3S_BAUD);

  CTLSerial.setTimeout(1);
  CTLSerial.begin(M3S_BAUD);

  pinMode(M3S_PCTL, OUTPUT);  // amp power control
  pinMode(M3S_ATTN, OUTPUT);
  pinMode(M3S_LED,  OUTPUT);

  digitalWrite(M3S_LED,  LOW); // turn on the led
  digitalWrite(M3S_PCTL, LOW);  // amp off
  digitalWrite(M3S_ATTN, LOW);  // attn on

  if (EEPROM.read(0) == 0x30) {
    beep = false;
  }
  if (EEPROM.read(1) == 0x30) {
    debug = true;
  }

  loop_count = 0;

  // init the state storage.
  for (int i = 0; i < M3S_ST_WINDOW + 2; i++) {
    vol[i] = cur[i] = swr[i] = ref[i] = pwr[i] = tmp[i] = 0;
  }
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
      if (idx == 0 && (char)c == M3S_TERM) {
        return;
      }
      buff[idx++] = (char)c;
      if (buff[idx-1] == M3S_TERM) {
        // terminating sequence started. Add two more 0xff.
        buff[idx++] = M3S_TERM;
        buff[idx++] = M3S_TERM;
        // now skip up to two 0xff in the stream.
        c = (dir) ? LCDSerial.peek() : CTLSerial.peek();
        if ((char)c == M3S_TERM) {
          c = (dir) ? LCDSerial.read() : CTLSerial.read();
          c = (dir) ? LCDSerial.peek() : CTLSerial.peek();
          if ((char)c == M3S_TERM) {
            c = (dir) ? LCDSerial.read() : CTLSerial.read();
          }
        }
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
        LCDSerial.write(buff, idx);
    }

    if (debug) {
      printUSB(buff, idx, dir);
    }
  }

  // intelligently switch between the sources. If the current source has
  // more data to read, stay with the source.
  loop_count++; // starvation prevention
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
        band = 160;
        break;
      case 'b':
        sendCtrlMsg("pdia=80");
        setLcdBand(1);
        band = 80;
        break;
      case 'c':
        sendCtrlMsg("pdia=40");
        setLcdBand(2);
        band = 40;
        break;
      case 'd':
        sendCtrlMsg("pdia=20");
        setLcdBand(3);
        band = 20;
        break;
      case 'e':
        sendCtrlMsg("pdia=15");
        setLcdBand(4);
        band = 15;
        break;
      case 'f':
        sendCtrlMsg("pdia=10");
        setLcdBand(5);
        band = 10;
        break;
      case 'g':
        sendCtrlMsg("pdia=6");
        setLcdBand(6);
        band = 6;
        break;
      case 'h':
        setLcdBand(7);
        sendCtrlMsg("pdia=255");
        band = 0;
        break;
      case 'i':
        printHelp();
        break;

      // power on
      case 'p':
        digitalWrite(M3S_PCTL, HIGH);
        power = true;
        break;
      // power off
      case 'q':
        digitalWrite(M3S_PCTL, LOW);
        power = false;
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
        attn = false;
        break;
      // attn on
      case 'y':
        digitalWrite(M3S_ATTN, LOW);
        attn = true;
        break;

      // antenna selection
      case '1':
        sendCtrlMsg("ponant1");
        sendLcdMsg("ant1.val=1");
        sendLcdMsg("ant2.val=0");
        sendLcdMsg("ant3.val=0");
        ant = 1;
        break;
      case '2':
        sendCtrlMsg("ponant2");
        sendLcdMsg("ant1.val=0");
        sendLcdMsg("ant2.val=1");
        sendLcdMsg("ant3.val=0");
        ant = 2;
        break;
      case '3':
        sendCtrlMsg("ponant3");
        sendLcdMsg("ant1.val=0");
        sendLcdMsg("ant2.val=0");
        sendLcdMsg("ant3.val=1");
        ant = 3;
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
