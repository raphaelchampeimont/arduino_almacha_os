
volatile byte keyboardBitCounter = 0;
volatile byte incompleteKeycode = 0;

volatile unsigned long lastKeyboardIntrruptTime = 0;
volatile byte keyboardInterruptDeltasIndex = 0;
const byte KEYBOARD_INTERRUPT_DELTA_ARRAY_SIZE = 15;
volatile byte keyboardInterruptDeltas[KEYBOARD_INTERRUPT_DELTA_ARRAY_SIZE];


const byte KEYBOARD_BUFFER_SIZE = 4;
volatile byte keyboardBuffer[KEYBOARD_BUFFER_SIZE];
volatile byte keyboardBufferIndex = 0;
volatile byte keyboardErrorStatus = 0;

volatile bool unreadKey = false;
volatile bool unreadKeyByOS = false;
volatile unsigned long lastCombinedCode = 0;

volatile byte lockState = 0x0; // num/caps/scroll lock state
volatile bool currentlySettingLEDs = false;

const long AUTOMATIC_RESET_AFTER = 200; // microseconds


// called as an Interrupt Service Routine
void processKeyboardInterrupt() {
  unsigned long currentTime = micros();
  unsigned long delta = currentTime - lastKeyboardIntrruptTime;
  keyboardInterruptDeltas[keyboardInterruptDeltasIndex] = (delta > 255) ? 255 : (byte) delta;
  lastKeyboardIntrruptTime = currentTime;
  keyboardInterruptDeltasIndex++;
  if (keyboardInterruptDeltasIndex >= KEYBOARD_INTERRUPT_DELTA_ARRAY_SIZE) {
    keyboardInterruptDeltasIndex = 0;
  }

  byte data = digitalRead(PS2_KEYBOARD_DATA_PIN);

  // If a long time occured since the last 11-bit sequence, reset to reading a new value.
  // This allows us to get out of a dead end if an extraneous or missing bit was transmitted previously.
  if (delta > AUTOMATIC_RESET_AFTER) {
    incompleteKeycode = 0;
    keyboardBitCounter = 0;
    keyboardErrorStatus = 0;
  }

  if (keyboardErrorStatus) return; // stop keyboard system on error: useful for debug

  if (keyboardBitCounter == 0) {
    // start bit
    if (data != 0) {
      // invalid start bit, it should be 0
      keyboardErrorStatus = 'S';
      reportKeyboardError();
    }
  } else if (keyboardBitCounter > 0 && keyboardBitCounter < 9) {
    // data bit: least significant bit is received first.
    incompleteKeycode |= data << (keyboardBitCounter - 1);
  } else if (keyboardBitCounter == 9) {
    // we have just read parity bit, so let's check parity
    byte parity = data;
    for (byte i = 0; i < 8; i++) {
      parity += incompleteKeycode >> i & 1;
    }
    // odd parity is expected
    if (parity % 2 != 1 && !keyboardErrorStatus) {
      // incorrect parity
      keyboardErrorStatus = 'P';
      reportKeyboardError();
    }
  }

  if (keyboardBitCounter == 10) {
    // We are at stop bit.
    if (data != 1 && !keyboardErrorStatus) {
      // invalid end bit: it should be 1
      keyboardErrorStatus = 'E';
      reportKeyboardError();
    }
    // All 11 bits have been received.
    if (! keyboardErrorStatus) {
      // process received byte if every check passed
      processKeyboard11BitCode();
    }
    // Reset to read next 11-bit chunk
    incompleteKeycode = 0;
    keyboardBitCounter = 0;
  } else {
    keyboardBitCounter++;
  }
}

// called from an Interrupt Service Routine
void processKeyboard11BitCode() {
  // Add keycode to buffer
  keyboardBuffer[keyboardBufferIndex] = incompleteKeycode;
  keyboardBufferIndex++;
  if (keyboardBufferIndex >= KEYBOARD_BUFFER_SIZE) {
    keyboardBufferIndex = 0;
  }

  unsigned long combinedCode = tryCombineCodes();
  if (combinedCode) {
    if (combinedCode == 0x7E) {
      // Scroll Lock
      toggleLock(0x01);
    } else if (combinedCode == 0x77) {
      // Num Lock
      toggleLock(0x02);
    } else if (combinedCode == 0x58) {
      // Caps Lock
      toggleLock(0x04);
    } else if (combinedCode == 0xFA) {
      ackReceivedFromKeyboard();
    } else if (combinedCode == 0xAA) {
      Serial.println("Keyboard successfully initialized.");
    }
    lastCombinedCode = combinedCode;
    unreadKey = true;
    unreadKeyByOS = true;
  }
}

// called from an Interrupt Service Routine
void toggleLock(byte flags) {
  lockState = lockState ^ flags;
  Serial.print("Keyboard: A lock key was pressed, new state is: ");
  Serial.println(lockState, BIN);

  // Tell the keyboard we want to change LED state
  currentlySettingLEDs = true;
  sendToKeyboard(0xED);
}

// called from Interrupt Service Routine
void ackReceivedFromKeyboard() {
  Serial.println("Keyboard acknowledged command.");
  if (currentlySettingLEDs) {
    // The ACK from the keyboard is in response to a 0xED (request to change LEDs)
    // Send new LED status
    sendToKeyboard(lockState);
    currentlySettingLEDs = false;
  }
}

// This function send data back to the keyboard.
// It may be called from an Interrupt Service Routine.
void sendToKeyboard(byte valueToWrite) {
  Serial.print("Sending to keyboard: 0x");
  Serial.println(valueToWrite, HEX);
  // I used https://www.avrfreaks.net/sites/default/files/PS2%20Keyboard.pdf to implement that

  byte clockCycle = 0;
  byte previousClockState = 1;
  byte clockState = 0;
  byte parity = 0;

  // Compute parity
  for (byte i = 0; i < 8; i++) {
    parity += (valueToWrite >> i) & 1;
  }

  // Pull clock low
  pinMode(PS2_KEYBOARD_CLOCK_PIN, OUTPUT);
  digitalWrite(PS2_KEYBOARD_CLOCK_PIN, LOW);
  delayMicroseconds(100);
  // Pull data low
  pinMode(PS2_KEYBOARD_DATA_PIN, OUTPUT);
  digitalWrite(PS2_KEYBOARD_DATA_PIN, LOW);
  // Release clock
  pinMode(PS2_KEYBOARD_CLOCK_PIN, INPUT_PULLUP);
  // Wait for keyboard to pull clock low, which indicates that we shoud set data line
  while (clockCycle < 11) {
    clockState = digitalRead(PS2_KEYBOARD_CLOCK_PIN);
    // Detect falling edge
    if (clockState != previousClockState && clockState == 0) {
      if (clockCycle < 8) {
        digitalWrite(PS2_KEYBOARD_DATA_PIN, (valueToWrite >> clockCycle) & 1);
      } else if (clockCycle == 8) {
        // send parity bit
        digitalWrite(PS2_KEYBOARD_DATA_PIN, parity % 2 == 1 ? LOW : HIGH);
      } else if (clockCycle == 9) {
        // Release data line
        pinMode(PS2_KEYBOARD_DATA_PIN, INPUT_PULLUP);
      } else if (clockCycle == 10) {
        if (digitalRead(PS2_KEYBOARD_DATA_PIN) != LOW) {
          Serial.println("Keyboard did not ACK our command correctly");
        }
      }
      clockCycle++;
    }
    previousClockState = clockState;
  }
}

// called from an Interrupt Service Routine
unsigned long tryCombineCodes() {
  byte n1 = keyboardBuffer[(keyboardBufferIndex - 1 + KEYBOARD_BUFFER_SIZE) % KEYBOARD_BUFFER_SIZE];
  byte n2 = keyboardBuffer[(keyboardBufferIndex - 2 + KEYBOARD_BUFFER_SIZE) % KEYBOARD_BUFFER_SIZE];
  byte n3 = keyboardBuffer[(keyboardBufferIndex - 3 + KEYBOARD_BUFFER_SIZE) % KEYBOARD_BUFFER_SIZE];

  if (n1 == 0xE0 || n1 == 0xF0) {
    // unfinished code
    return 0;
  } else if (n2 == 0xF0) {
    // key released
    if (n3 == 0xE0) {
      // extended key
      return n1 | 0xE0F000;
    } else {
      // regular key
      return n1 | 0xF000;
    }
  } else if (n2 == 0xE0) {
    // extended key pressed
    return n1 | 0xE000;
  } else {
    // regular key pressed
    return n1;
  }
}

void reportKeyboardError() {
  // print debug information to serial
  Serial.print("Keyboard error: ");
  Serial.write(keyboardErrorStatus);
  Serial.print(" / Partially read key code: ");
  Serial.println(incompleteKeycode, BIN);
}


void initKeyboard() {
  Serial.println("Initializing keyboard");

  for (int i = 0; i < KEYBOARD_BUFFER_SIZE; i++) {
    keyboardBuffer[i] = 0;
  }

  // We are supposed to use pullup resistors in PS/2 spec.
  pinMode(PS2_KEYBOARD_CLOCK_PIN, INPUT_PULLUP);
  pinMode(PS2_KEYBOARD_DATA_PIN, INPUT_PULLUP);

  // Tell keyboard to reset
  sendToKeyboard(0xFF); // 0xFF means RESET

  attachInterrupt(digitalPinToInterrupt(PS2_KEYBOARD_CLOCK_PIN), processKeyboardInterrupt, FALLING);
}

// Reads key and marks it as read
unsigned long keyboardReadKey() {
  if (unreadKey) {
    unreadKey = false;
    // Disable interrupts to read reliably a >1 byte variable modified by an ISR
    //noInterrupts();
    unsigned long code = lastCombinedCode;
    if (code) {
      Serial.print("Keyboard key read: ");
      Serial.println(code, HEX);
    }
    //interrupts();
    return code;
  } else {
    // Now new keycodes have been received since last function call
    return 0;
  }
}

// For OS only, it does not flag the key as read, to let an app read it
unsigned long keyboardOSReadKey() {
  if (unreadKeyByOS) {
    unreadKeyByOS = false;
    // Disable interrupts to read reliably a >1 byte variable modified by an ISR
    //noInterrupts();
    unsigned long code = lastCombinedCode;
    if (code) {
      Serial.print("Keyboard key read by OS: ");
      Serial.println(code, HEX);
    }
    //interrupts();
    return code;
  } else {
    // Now new keycodes have been received since last function call
    return 0;
  }
}

// Get caps/num/scroll lock state
byte keyboardGetLockState() {
  return lockState;
}
