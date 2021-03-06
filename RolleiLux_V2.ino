//===========================================================================================
// ROLLEILUX - Rolleiflex Lightmeter Firmware
//
// Version 2, using the uduino (Arduino Leonardo clone) instead of the Arduino Pro Mini
//
// Created: 07.05.2018
//
// Copyright (C) 2018 by Guido Hoss
//===========================================================================================
// RolleiLux is free software: you can redistribute it and/or 
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation, either version 3
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public
// License along with this program.  If not, see
// <http://www.gnu.org/licenses/>.
//
// Target: Arduino Leonardo
//
// Git repository home: <https://github.com/ghoss/RolleiLux>
//===========================================================================================

// Debugging flag
//#define DEBUG

// Prescaler flag to reduce CPU clock speed and energy consumption
#define SLOW_CLOCK


//-------------------------------------------------------------------------------------------
// Hardware / connector pin definitions
//-------------------------------------------------------------------------------------------

// Designator of analog pin connected to photo sensor
#define PHOTO_INPUT A5

// Digital pin powering the photo sensor
#define PHOTO_POWER 5

// Designator of PWM pin connected to voltmeter output
#define METER_OUTPUT 10


//-------------------------------------------------------------------------------------------
// Constants and variables related to photo sensor measurement and ADC
//-------------------------------------------------------------------------------------------

// Resolution of built-in A/D converter
#define BUILTIN_INPUT_BITS 10

// Target A/D resolution in bits (must be at least 10, the built-in A/D resolution, and at
// most 16, since integers are 16 bits long on the Arduino)
#define TARGET_INPUT_BITS 15

// Maximum input measurement value
#define INPUT_MAXVAL ((uint16_t) (1 << TARGET_INPUT_BITS) - 1)

// Number of milliseconds to wait between successive A/D measurements
#define MEASURE_DELAY 1

// Number of readings for smoothing
#define NUM_READINGS 10

// Variables related to smoothing
uint16_t readings[NUM_READINGS];      // the readings from the analog input
uint16_t readIndex = 0;               // the index of the current reading
long total = 0;                       // the running total
bool bufferFull = false;


//-------------------------------------------------------------------------------------------
// Mapping table ev = F(ldr) for LDR resistor response curve
//-------------------------------------------------------------------------------------------

// This is a global exposure correction factor in stops*10 (e.g. 10 = +1 EV) which will be
// applied after the mapping from this table has been performed. Its purpose is to correct for
// different LDR enclosures/covers/filters without having to recalibrate the entire tables.

#define GLOBAL_EV_CORRECTION 10

// ldrX contains data point x coordinates (LDR measurement values)
//
const uint16_t ldrX[] = {
  0,      // #0
  420,    // #1
  900,    // #2
  2010,   // #2.1
  3328,   // #3
  4400,   // #3.1
  6270,   // #4
  10380,  // #5
  14200,  // #6
  19200,  // #7
  26000,  // #8
  27680,  // #9
  28090,  // #10
  30500   // #11
};


// ldrY contains y coordinates (corresponding EV * 10 values)
//
const uint8_t ldrY[] = {
  0,      // #0
  10,     // #1
  50,     // #2
  55,     // #2.1
  80,     // #3
  90,     // #3.1
  95,     // #4
  105,    // #5
  115,    // #6
  130,    // #7
  150,    // #8
  153,    // #9
  155,    // #10
  180     // #11
};

// Number of data points in LDR curve
const uint8_t N_LDR = sizeof(ldrX) / sizeof(ldrX[0]);


//-------------------------------------------------------------------------------------------
// Mapping table pwm = F(ev) for voltmeter response curve
//-------------------------------------------------------------------------------------------

// This mapping tables defines the relationship between EV and PWM output values
// for the voltmeter
const uint8_t pwm[] = {
  0, 0, 0, 0,               // EV 0..3 as filler bytes
  0,                        // EV 4 (= EV_MIN)
  1,                        // EV 5
  3,                        // EV 6
  5,                        // EV 7
  11,                       // EV 8
  25,                       // EV 9
  50,                       // EV 10
  80,                       // EV 11
  100,                      // EV 12
  123,                      // EV 13
  148,                      // EV 14
  165,                      // EV 15
  181,                      // EV 16
  194,                      // EV 17
  200,                      // EV 18 (= EV_MAX)
  200                       // EV 19 as filler byte
};

// Minimum and maximum EV that can be indicated by circuit
const uint16_t EV_MIN = 40;
const uint16_t EV_MAX = 10 * (sizeof(pwm) - 2);


//-------------------------------------------------------------------------------------------
// mapfix
//
// Fixed version of Arduino's flawed "map" function.
//-------------------------------------------------------------------------------------------

long mapfix(long x, long in_min, long in_max, long out_min, long out_max)
{
  return (x - in_min) * (out_max - out_min + 1) / (in_max - in_min + 1) + out_min;
}


//-------------------------------------------------------------------------------------------
// measureInput
//
// Performs several A/D measurements in sequence and sums them up. The result is the
// averaged measurement in the range 0..INPUT_MAXVAL.
//-------------------------------------------------------------------------------------------

uint16_t measureInput()
{
  const uint16_t ITERATIONS = 1 << (TARGET_INPUT_BITS - BUILTIN_INPUT_BITS);
  
  uint16_t sensorVal = 0;

  for (uint16_t i = 0; i < ITERATIONS; i ++)
  {
    // Get the sensor value
    sensorVal += analogRead(PHOTO_INPUT);

    // Allow A/D circuit to settle
    delay(MEASURE_DELAY);
  }
  
  return sensorVal;
}


//-------------------------------------------------------------------------------------------
// smooth
//
// Perform averaging: add the input value to the round-robin buffer and return the average
// of the last NUM_READINGS measurements.
//-------------------------------------------------------------------------------------------

uint16_t smooth(uint16_t x)
{
  total -= readings[readIndex];
  readings[readIndex] = x;
  total += x;
  readIndex = (readIndex + 1) % NUM_READINGS;
  if (! bufferFull) bufferFull = (readIndex == 0);
  
  return (uint16_t) (total / NUM_READINGS);
}


//-------------------------------------------------------------------------------------------
// mapPhotoToEV
//
// Maps photo sensor values to EV with a lookup table derived from measurements.
//
// The returned EV value is actually EV * 10 to allow for fractional EV without using floats.
//-------------------------------------------------------------------------------------------

uint16_t mapPhotoToEV(uint16_t ldr)
{
  uint8_t maxIdx = N_LDR - 1;
  uint16_t ev;

  // Check for measurement values beyond the scope of the data curve
  // We don't check for readings below minimum because 0/0 is already a data point
  if (ldr >= ldrX[maxIdx])
  {
    // Reading above maximum; return maximum EV
    ev = ldrY[maxIdx];
    ev = mapfix(ldr, ldrX[maxIdx -1], ldrX[maxIdx], ldrY[maxIdx - 1], ldrY[maxIdx]);
  }
  else
  {
    // If measurement in range, find left and right data points closest to measurement
    uint8_t minIdx = 0;
    
    while (minIdx < maxIdx - 1) 
    {
      uint8_t medIdx = (maxIdx + minIdx) >> 1;
      uint16_t x = ldrX[medIdx];
      if (ldr <= x) maxIdx = medIdx;
      if (ldr >= x) minIdx = medIdx;
    }
    ev = mapfix(ldr, ldrX[minIdx], ldrX[maxIdx], ldrY[minIdx], ldrY[maxIdx]);
  }

  return ev + GLOBAL_EV_CORRECTION;
}



//-------------------------------------------------------------------------------------------
// mapEVToPWM
//
// Maps EV to PWM values for analog output to the voltmeter
//
// The supplied EV10 value is actually EV * 10 to allow for fractional EV with 1 digit
// precision.
//-------------------------------------------------------------------------------------------

uint8_t mapEVToPWM(uint16_t ev10)
{
  uint16_t lowV, lowV10;

  // Range check input value
  ev10 = constrain(ev10, EV_MIN, EV_MAX);

  // Get next lower EV value
  lowV = ev10 / 10;
  lowV10 = lowV * 10;

  // Interpolate pwm value between discrete points pwm(lowV)..pwm(lowV + 1)
  return mapfix(ev10, lowV10, lowV10 + 10, pwm[lowV], pwm[lowV + 1]);
}


//-------------------------------------------------------------------------------------------
// initVariant
//
// Called automatically before setup(). Reduce CPU clock speed to conserve energy.
//-------------------------------------------------------------------------------------------

#ifdef SLOW_CLOCK

void initVariant()
{
  noInterrupts();
  CLKPR = _BV(CLKPCE);  // enable change of the clock prescaler
  CLKPR = _BV(CLKPS0);  // divide frequency by 2 (= 8 MHz)
  interrupts();
}

#endif


//-------------------------------------------------------------------------------------------
// setup
//
// This microcontroller setup function runs once when you press reset or power the board.
//-------------------------------------------------------------------------------------------

void setup()
{
#ifdef DEBUG
  // initialize serial communications at 9600 bps:
  Serial.begin(9600);
#endif
 
   // Set the reference voltage for the A/D converter to default
  analogReference(DEFAULT);

  // Initialize photo sensor pin as an input
  pinMode(PHOTO_INPUT, INPUT);

  // Initialize photo sensor power supply as output
  pinMode(PHOTO_POWER, OUTPUT);
  digitalWrite(PHOTO_POWER, HIGH);

  // Initialize voltmeter pin as an output
  pinMode(METER_OUTPUT, OUTPUT);

  // Set needle to center position for 1.2 sec to verify battery level
  analogWrite(METER_OUTPUT, mapEVToPWM(100));
  #ifdef SLOW_CLOCK
  delay(500);
  #else
  delay(1000);
  #endif

  // Initialize all the readings in the smoothing buffer
  for (uint16_t i = 0; i < NUM_READINGS; i ++) readings[i] = 0;

  // Read sensor once to move buffer pointer so that bufferFull == false
  smooth(measureInput());
}


//-------------------------------------------------------------------------------------------
// loop
//
// The loop function runs over and over again forever
//-------------------------------------------------------------------------------------------

void loop()
{
  uint16_t photoV, evV, meterV;

  // Read the analog value of photo sensor
  photoV = smooth(measureInput());
  
  // Calculate exposure value
  evV = mapPhotoToEV(photoV);

  // Output exposure value to light meter
  analogWrite(METER_OUTPUT, meterV = mapEVToPWM(evV));

#ifdef DEBUG
  // Print the results to the serial monitor
  Serial.print("photo = ");
  Serial.print(photoV);
  
  Serial.print("   EV = ");
  Serial.print(evV / 10);
  Serial.print(".");
  Serial.print(evV % 10);

  Serial.print("   mV = ");
  Serial.println(meterV * (3300 / 255));
#endif
}
