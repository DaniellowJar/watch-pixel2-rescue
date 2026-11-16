/*
 * jl_usbkey.ino - JieLi USB_KEY (0x16EF) generator for Arduino Mega 2560
 *
 * Sends the 16-bit USB download key 0x16EF (0001 0110 1110 1111) over the
 * USB data lines, MSB-first, D+ = clock @ ~50 kHz, D- = data (latched on
 * the clock's rising edge), until the chip ACKs by pulling both lines low.
 *
 * WHY: the watch's Mask-ROM enters USB download (UBOOT) mode either
 * automatically (bricked = can't boot from flash -> it should just come up
 * as a USB disk when powered and connected to a host) or after this key.
 * Use this sketch only if the automatic entry does not happen.
 *
 * WIRING  (watch debug pads: DP, GND, VCC, DM - left to right, crown right):
 *
 *   DP (D+)  ---------- 470R ---+--- Arduino pin 8   (clock)
 *   DM (D-)  ---------- 470R ---+--- Arduino pin 9   (data)
 *   GND      ----------------------- Arduino GND
 *   VCC      ----------------------- watch power (battery rail 3.7-4.2 V or 5 V,
 *                                    whatever you used before - verify with a meter!)
 *
 *   + external pullups: 1 kOhm from DP to 3.3 V and 1 kOhm from DM to 3.3 V.
 *     (Required to sense the ACK. Built-in AVR pullups are too weak because
 *      the chip keeps 15k pulldowns enabled while waiting for the key.)
 *
 * NOTE ON VOLTAGE: Mega IO is 5 V, the USB lines are 3.3 V logic. The series
 * resistors above limit the current; the pullups to 3.3 V bias the lines into
 * a sane idle level. A proper level shifter is cleaner but this works.
 *
 * PROCEDURE:
 *   1. Upload this sketch, open Serial Monitor @115200.
 *   2. Power the watch from VCC (leave DP/DM disconnected from the Mac for now).
 *   3. Watch the monitor for "ACK!". The moment you see it, switch DP/DM to a
 *      free Mac USB port (the chip pulls D+ up and waits for SOF packets, then
 *      enumerates as a mass-storage "UBOOT" device).
 *
 * Remember: if the watch is bricked hard it may ALREADY be in download mode -
 * try just powering it and plugging DP/DM to the Mac first, no key needed.
 */

#define PIN_CLK   8   // D+  (clock)
#define PIN_DAT   9   // D-  (data)
#define KEY       0x16EF
#define BIT_US    20  // 50 kHz -> 20 us per bit

void setup()
{
    Serial.begin(115200);

    pinMode(PIN_CLK, OUTPUT);
    pinMode(PIN_DAT, OUTPUT);
    digitalWrite(PIN_CLK, LOW);
    digitalWrite(PIN_DAT, LOW);

    Serial.println(F("JL USB_KEY 0x16EF @ 50 kHz - sending key until ACK..."));
    Serial.println(F("Power the watch from VCC now."));
}

void sendKey()
{
    // MSB-first; data must be stable before the clock rising edge
    for (int i = 15; i >= 0; i--)
    {
        digitalWrite(PIN_DAT, (KEY >> i) & 1 ? HIGH : LOW);
        digitalWrite(PIN_CLK, HIGH);
        delayMicroseconds(BIT_US / 2);
        digitalWrite(PIN_CLK, LOW);
        delayMicroseconds(BIT_US / 2);
    }
}

void loop()
{
    sendKey();

    // Release the lines (external pullups hold them high) and look for the ACK
    pinMode(PIN_CLK, INPUT);
    pinMode(PIN_DAT, INPUT);
    delayMicroseconds(2500);

    if (digitalRead(PIN_CLK) == LOW && digitalRead(PIN_DAT) == LOW)
    {
        Serial.println(F("ACK! Watch is in USB download mode."));
        Serial.println(F("Now connect DP/DM to a free Mac USB port."));
        while (1)
        {
            pinMode(PIN_CLK, INPUT);
            pinMode(PIN_DAT, INPUT);
            delay(250);
        }
    }

    pinMode(PIN_CLK, OUTPUT);
    pinMode(PIN_DAT, OUTPUT);
}