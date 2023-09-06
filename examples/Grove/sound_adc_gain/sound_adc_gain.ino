#include <Arduino.h>
#include <Adafruit_TinyUSB.h> 
#include <grove_sensor.hpp>

int adcin    = A0;
int adcvalue = 0;
float mv_per_lsb = 3600.0F/1024.0F; // 10-bit ADC with 3.6V input range

void setup()
{
    digitalWrite(PIN_POWER_SUPPLY_GROVE, HIGH);   //grove power on
    pinMode(PIN_POWER_SUPPLY_GROVE, OUTPUT);  

	delay(100);

	Serial.begin(115200);
	while (!Serial) {
		delay(100);
	}
    Serial.println("Grove - Sound Sensor Test...");
}

void loop()
{
    long sum = 0;
    for(int i=0; i<32; i++)
    {
        sum += analogRead(adcin);
        delay(2);
    }
    adcvalue = sum >>5;

    Serial.print(adcvalue);
    Serial.print(" [");
    Serial.print((float)adcvalue * mv_per_lsb);
    Serial.println(" mV]");

    delay(2);
    // delay(1000);
}