/*
 * Copyright 2023 Roger Feese
 */

#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/wdt.h>
#include <avr/eeprom.h>
#include <util/delay.h>
#include <stdlib.h>

/******************************************************************
 * Conrol LED lights for Leah's sign using Attiny85
 *
 * PWM output on Pins 2,3,5,6 goes to MOSFETs.
 * Use Pin 1 (RESET) as a button input using a voltage divider.
 * Analog read from amplified microphone signal on Pin 7.
 */

// disable watchdog on reset
void wdt_init(void) __attribute__((naked)) __attribute__((section(".init3")));
void wdt_init(void){
    MCUSR = 0;
    wdt_disable();
}

// software reset using the watchdog timer
void reset(){
	// enable watchdog
	wdt_enable(WDTO_30MS);
	// wait for watchdog timeout
	while(1){};
}

/**
 * Since ATTiny85 unfortunately only has built-in support for PWM output on Pins 3, 5, and 6,
 * emulate Fast PWM on Pin 2 via timer1 interrupts
 */

// set pin on overflow
ISR(TIMER1_OVF_vect){
	// don't turn on at all for low values of OCR1A to compensate for interrupt latency
	if(OCR1A > 4){
		PORTB |= (1 << PB3);
	}
}

// clear pin on match
ISR(TIMER1_COMPA_vect){
	PORTB &= ~(1 << PB3);
}

// Read analog value (0-1024) from ADC0-3 (PB5, PB2, PB4, PB3)
// ch must be in range 0..3
uint16_t adc_read(uint8_t ch){
  // set MUX values to control which AD channel to sample
  // clear bottom 3 bits and then assign ch
  ADMUX = (ADMUX & 0b11111000) | ch;

  // start single conversion
  ADCSRA |= (1 << ADSC);

  // wait for conversion to complete
  while(ADCSRA & (1 << ADSC));

  // ADC contains converted value
  return (ADC);
}

/**
 * Save and load settings from EEPROM
 */
uint8_t settings_start_seq_ee EEMEM = 0;
uint8_t settings_start_seq = 0;

void load_settings(){
	settings_start_seq = eeprom_read_byte(&settings_start_seq_ee);
}

void save_settings(){
	eeprom_update_byte(&settings_start_seq_ee, settings_start_seq);
}

// Using reset pin (1) as a button input.
// Check for RESET going low (use voltage divider)
// but not low enough to trigger actual reset (0.9 Vcc)
int check_button_input(){
	if (adc_read(0) < 1000){
		// debounce button presses by delaying positive return
		_delay_ms(300);
		// reset on long press
		if (adc_read(0) < 1000){
			_delay_ms(3000);
			if (adc_read(0) < 1000){
				// blink the lights
				for(int i = 0; i < 10; i++){
					OCR0A = 255; // OC0A PB0 Pin 5
					OCR0B = 255; // OC0B PB1 Pin 6
					OCR1B = 255; // OC1B PB4 PIN 3
					OCR1A = 255; // OC1A PB3 PIN 2
					_delay_ms(50);
					OCR0A = 0; // OC0A PB0 Pin 5
					OCR0B = 0; // OC0B PB1 Pin 6
					OCR1B = 0; // OC1B PB4 PIN 3
					OCR1A = 0; // OC1A PB3 PIN 2
					_delay_ms(50);
				}
				_delay_ms(2000);
				save_settings();
				reset();
			}
		}
		return 1;
	}
	return 0;
}

// Custom time reference since powerup.
// Use timer0 overflow interrupt to count "millis" (probably not really accurate)
uint16_t timer_interrupt_ticks = 0;
uint64_t millis = 0; // eventually this overflows...
ISR(TIMER0_OVF_vect){
	timer_interrupt_ticks ++;
	// assume timer interrupts about every 0.25ms
	if(timer_interrupt_ticks > 4){
		timer_interrupt_ticks = 0;
		millis++;
	}
}

/**
 * Ring buffer for MIC samples
 */
// buffer size should be a power of 2 for efficiency
#define	MIC_BUFFER_SIZE	16
uint16_t mic_buffer[MIC_BUFFER_SIZE];
int mic_buffer_current;

void init_mic_buffer(){
	for(int i = 0; i < MIC_BUFFER_SIZE; i++){
		mic_buffer[i] = (uint16_t)512;
	}
	mic_buffer_current = 0;
}

// Measure "sound" in mic_buffer
// using Mean Absolute Deviation (MAD)
uint16_t get_mic_buffer_mad(){
	// mean
	uint16_t total = 0;
	for(int i = 0; i < MIC_BUFFER_SIZE; i++){
		total = total + mic_buffer[i];
	}
	uint16_t mean = total / MIC_BUFFER_SIZE;

	// MAD
	uint16_t mad = 0;
	for(int i = 0; i < MIC_BUFFER_SIZE; i++){
		mad = mad + abs(mic_buffer[i] - mean);
	}
	return mad / MIC_BUFFER_SIZE;
}


// function prototype for LED sequences
// return 1 if button pressed, return after timeout millis
typedef int (*led_sequence)(int timeout);
int seq1(int timeout);
int seq2(int timeout);
int seq3(int timeout);
int seq4(int timeout);
int seq5(int timeout);
int seq6(int timeout);
int seq7(int timeout);
int seq8(int timeout);
int seq9(int timeout);
int seq10(int timeout);
int seq11(int timeout);
int seq12(int timeout);
#define	NUM_SEQ 11
led_sequence seq[NUM_SEQ] = {&seq1,&seq2,&seq3,&seq4,&seq5,&seq6,&seq7,&seq8,&seq9,&seq10,&seq11};

int main(){
	init_mic_buffer();

	// Set up ADC
	// Voltage reference = Vcc, disconnected from PB0
	ADMUX = (0 << REFS1) | (0 << REFS0);
	// ADC Enable and prescaler of 128
	ADCSRA = (1 << ADEN) | (1 << ADPS2) | (1 << ADPS1) | (1 << ADPS0);

	// Set port B Data Direction, output pins PB4, PB3, PB1, PB0 (3, 2, 6, 5)
	DDRB = (1 << DDB4) | (1 << DDB3) | (1 << DDB1) | (1 << DDB0);

	// Set OCOA/OCOB on Compare Match, clear at bottom
	// Configure Timer/Counter-0 for fast PWM
	TCCR0A = (2 << COM0A0) | (2 << COM0B0) | (1 << WGM00);
	// Enable fast PWM mode, no prescaling
	TCCR0B = (0 << WGM02) | (1 << CS00);

	// Configure Timer/Counter-1
	// Don't output to 0C1A, no prescaling
	TCCR1 = (1 << PWM1A) | (3 << COM1A0) | (7 << CS10);
	// Enable output on OC1B
	GTCCR = (1 << PWM1B) | (2 << COM1B0);

	// Enable interrupt on matcha and overflow Timer/Counter-1, overflow t/c-0
	TIMSK = (1 << OCIE1A) | (1 << TOIE1) | (1 << TOIE0);

	// enable interrupts
	sei();

	load_settings();
	uint8_t settings_start_seq_applied = 0;

	while (1) {

		// demo mode -- cycle through sequences until button press
		int button_pressed = 0;
		if(!settings_start_seq_applied){
			if(settings_start_seq > 0){
				button_pressed = 1;
			}
			else {
				settings_start_seq_applied = 1;
			}
		}
		while(!button_pressed){
			settings_start_seq = 0;

			// random led sequence
			uint8_t randseq = (rand() % NUM_SEQ);
			if(!button_pressed)
				button_pressed = seq[randseq](8000);
		}

		// stay on each cycle
		if(settings_start_seq_applied || settings_start_seq < 2){
			settings_start_seq_applied = 1;
			settings_start_seq = 1;
			seq1(-1);
		}
		if(settings_start_seq_applied || settings_start_seq < 3){
			settings_start_seq_applied = 1;
			settings_start_seq = 2;
			seq2(-1);
		}
		if(settings_start_seq_applied || settings_start_seq < 4){
			settings_start_seq_applied = 1;
			settings_start_seq = 3;
			seq3(-1);
		}
		if(settings_start_seq_applied || settings_start_seq < 5){
			settings_start_seq_applied = 1;
			settings_start_seq = 4;
			seq4(-1);
		}
		if(settings_start_seq_applied || settings_start_seq < 6){
			settings_start_seq_applied = 1;
			settings_start_seq = 5;
			seq5(-1);
		}
		if(settings_start_seq_applied || settings_start_seq < 7){
			settings_start_seq_applied = 1;
			settings_start_seq = 6;
			seq6(-1);
		}
		if(settings_start_seq_applied || settings_start_seq < 8){
			settings_start_seq_applied = 1;
			settings_start_seq = 7;
			seq7(-1);
		}
		if(settings_start_seq_applied || settings_start_seq < 9){
			settings_start_seq_applied = 1;
			settings_start_seq = 8;
			seq8(-1);
		}
		if(settings_start_seq_applied || settings_start_seq < 10){
			settings_start_seq_applied = 1;
			settings_start_seq = 9;
			seq9(-1);
		}
		if(settings_start_seq_applied || settings_start_seq < 11){
			settings_start_seq_applied = 1;
			settings_start_seq = 10;
			seq10(-1);
		}
		if(settings_start_seq_applied || settings_start_seq < 12){
			settings_start_seq_applied = 1;
			settings_start_seq = 11;
			seq11(-1);
		}
		if(settings_start_seq_applied || settings_start_seq < 13){
			settings_start_seq_applied = 1;
			settings_start_seq = 12;
			seq12(-1);
		}
	}
	return 0;
}

int delay_millis_check_button(int millisdelay){
	int button_pressed = 0;

	uint64_t start_time = millis;
	while(millis < (start_time + millisdelay)){
		if(check_button_input()) button_pressed = 1;
	}

	return button_pressed;
}

// fade each in and then out sequentially
int seq1(int timeout){
	OCR0A = 0; // OC0A PB0 Pin 5
	OCR0B = 0; // OC0B PB1 Pin 6
	OCR1B = 0; // OC1B PB4 PIN 3
	OCR1A = 0; // OC1A PB3 PIN 2

	uint8_t led1 = 0, led2 = 0, led3 = 0, led4 = 0;

	uint64_t start_time = millis;
	while((timeout < 0) || (millis < (start_time + (uint64_t)timeout))){
		while(led1 < 255){
			if(check_button_input()) return 1;
			led1++;
			OCR0A = led1; // OC0A PB0 Pin 5
			_delay_ms(1);
		}
		while(led2 < 255){
			if(check_button_input()) return 1;
			led2++;
			OCR0B = led2; // OC0B PB1 Pin 6
			_delay_ms(1);
		}
		while(led3 < 255){
			if(check_button_input()) return 1;
			led3++;
			OCR1B = led3; // OC1B PB4 PIN 3
			_delay_ms(1);
		}
		while(led4 < 255){
			if(check_button_input()) return 1;
			led4++;
			OCR1A = led4; // OC1A PB3 PIN 2
			_delay_ms(1);
		}

		while(led1 > 0){
			if(check_button_input()) return 1;
			led1--;
			OCR0A = led1; // OC0A PB0 Pin 5
			_delay_ms(1);
		}
		while(led2 > 0){
			if(check_button_input()) return 1;
			led2--;
			OCR0B = led2; // OC0B PB1 Pin 6
			_delay_ms(1);
		}
		while(led3 > 0){
			if(check_button_input()) return 1;
			led3--;
			OCR1B = led3; // OC1B PB4 PIN 3
			_delay_ms(1);
		}
		while(led4 > 0){
			if(check_button_input()) return 1;
			led4--;
			OCR1A = led4; // OC1A PB3 PIN 2
			_delay_ms(1);
		}
	}

	return 0;
}

// fade each in sequentially and then fade all out
int seq2(int timeout){
	OCR0A = 0; // OC0A PB0 Pin 5
	OCR0B = 0; // OC0B PB1 Pin 6
	OCR1B = 0; // OC1B PB4 PIN 3
	OCR1A = 0; // OC1A PB3 PIN 2

	uint8_t led1 = 0, led2 = 0, led3 = 0, led4 = 0;

	uint64_t start_time = millis;
	while((timeout < 0) || (millis < (start_time + (uint64_t)timeout))){
		while(led1 < 255){
			if(check_button_input()) return 1;
			led1++;
			OCR0A = led1; // OC0A PB0 Pin 5
			_delay_ms(2);
		}
		while(led2 < 255){
			if(check_button_input()) return 1;
			led2++;
			OCR0B = led2; // OC0B PB1 Pin 6
			_delay_ms(2);
		}
		while(led3 < 255){
			if(check_button_input()) return 1;
			led3++;
			OCR1B = led3; // OC1B PB4 PIN 3
			_delay_ms(2);
		}
		while(led4 < 255){
			if(check_button_input()) return 1;
			led4++;
			OCR1A = led4; // OC1A PB3 PIN 2
			_delay_ms(2);
		}

		for(uint8_t i = 255; i > 0; i--){
			if(check_button_input()) return 1;
			led1--;
			led2--;
			led3--;
			led4--;
			OCR0A = led1; // OC0A PB0 Pin 5
			OCR0B = led2; // OC0B PB1 Pin 6
			OCR1B = led3; // OC1B PB4 PIN 3
			OCR1A = led4; // OC1A PB3 PIN 2
			_delay_ms(4);
		}
	}

	return 0;
}

// flash each in sequentially and then fade all out
int seq3(int timeout){
	OCR0A = 0; // OC0A PB0 Pin 5
	OCR0B = 0; // OC0B PB1 Pin 6
	OCR1B = 0; // OC1B PB4 PIN 3
	OCR1A = 0; // OC1A PB3 PIN 2

	uint64_t start_time = millis;
	while((timeout < 0) || (millis < (start_time + (uint64_t)timeout))){
		if(check_button_input()) return 1;
		OCR0A = 255; // OC0A PB0 Pin 5
		if(delay_millis_check_button(50)) return 1;
		OCR0A = 0; // OC0A PB0 Pin 5
		if(delay_millis_check_button(50)) return 1;
		OCR0A = 255; // OC0A PB0 Pin 5
		if(delay_millis_check_button(50)) return 1;
		OCR0A = 0; // OC0A PB0 Pin 5
		if(delay_millis_check_button(50)) return 1;
		OCR0A = 255; // OC0A PB0 Pin 5
		
		if(delay_millis_check_button(100)) return 1;

		if(check_button_input()) return 1;
		OCR0B = 255; // OC0B PB1 Pin 6
		if(delay_millis_check_button(50)) return 1;
		OCR0B = 0; // OC0B PB1 Pin 6
		if(delay_millis_check_button(50)) return 1;
		OCR0B = 255; // OC0B PB1 Pin 6
		if(delay_millis_check_button(50)) return 1;
		OCR0B = 0; // OC0B PB1 Pin 6
		if(delay_millis_check_button(50)) return 1;
		OCR0B = 255; // OC0B PB1 Pin 6
	
		if(delay_millis_check_button(100)) return 1;

		if(check_button_input()) return 1;
		OCR1B = 255; // OC1B PB4 PIN 3
		if(delay_millis_check_button(50)) return 1;
		OCR1B = 0; // OC1B PB4 PIN 3
		if(delay_millis_check_button(50)) return 1;
		OCR1B = 255; // OC1B PB4 PIN 3
		if(delay_millis_check_button(50)) return 1;
		OCR1B = 0; // OC1B PB4 PIN 3
		if(delay_millis_check_button(50)) return 1;
		OCR1B = 255; // OC1B PB4 PIN 3

		if(delay_millis_check_button(100)) return 1;

		if(check_button_input()) return 1;
		OCR1A = 255; // OC1A PB3 PIN 2
		if(delay_millis_check_button(50)) return 1;
		OCR1A = 0; // OC1A PB3 PIN 2
		if(delay_millis_check_button(50)) return 1;
		OCR1A = 255; // OC1A PB3 PIN 2
		if(delay_millis_check_button(50)) return 1;
		OCR1A = 0; // OC1A PB3 PIN 2
		if(delay_millis_check_button(50)) return 1;
		OCR1A = 255; // OC1A PB3 PIN 2

		if(delay_millis_check_button(500)) return 1;

		for(uint8_t i = 255; i > 0; i--){
			if(check_button_input()) return 1;
			OCR0A = i; // OC0A PB0 Pin 5
			OCR0B = i; // OC0B PB1 Pin 6
			OCR1B = i; // OC1B PB4 PIN 3
			OCR1A = i; // OC1A PB3 PIN 2
			_delay_ms(1);
		}
		OCR0A = 0; // OC0A PB0 Pin 5
		OCR0B = 0; // OC0B PB1 Pin 6
		OCR1B = 0; // OC1B PB4 PIN 3
		OCR1A = 0; // OC1A PB3 PIN 2

		if(delay_millis_check_button(500)) return 1;
	}

	return 0;
}

// fade all in and then all out
int seq4(int timeout){
	OCR0A = 0; // OC0A PB0 Pin 5
	OCR0B = 0; // OC0B PB1 Pin 6
	OCR1B = 0; // OC1B PB4 PIN 3
	OCR1A = 0; // OC1A PB3 PIN 2

	uint64_t start_time = millis;
	while((timeout < 0) || (millis < (start_time + (uint64_t)timeout))){
		for(int j = 0; j < 255; j++){
			if(check_button_input()) return 1;
			OCR0A = j; // OC0A PB0 Pin 5
			OCR0B = j; // OC0B PB1 Pin 6
			OCR1B = j; // OC1B PB4 PIN 3
			OCR1A = j; // OC1A PB3 PIN 2
			_delay_ms(1);
		}
		for(int j = 255; j >= 0; j--){
			if(check_button_input()) return 1;
			OCR0A = j; // OC0A PB0 Pin 5
			OCR0B = j; // OC0B PB1 Pin 6
			OCR1B = j; // OC1B PB4 PIN 3
			OCR1A = j; // OC1A PB3 PIN 2
			_delay_ms(1);
		}
		if(delay_millis_check_button(100)) return 1;
	}

	return 0;
}

// flash all quickly
int seq5(int timeout){
	OCR0A = 0; // OC0A PB0 Pin 5
	OCR0B = 0; // OC0B PB1 Pin 6
	OCR1B = 0; // OC1B PB4 PIN 3
	OCR1A = 0; // OC1A PB3 PIN 2

	uint64_t start_time = millis;
	while((timeout < 0) || (millis < (start_time + (uint64_t)timeout))){
		for(int j = 0; j < 255; j+=4){
			if(check_button_input()) return 1;
			OCR0A = j; // OC0A PB0 Pin 5
			OCR0B = j; // OC0B PB1 Pin 6
			OCR1B = j; // OC1B PB4 PIN 3
			OCR1A = j; // OC1A PB3 PIN 2
			_delay_ms(1);
		}
		for(int j = 255; j >= 0; j-=4){
			if(check_button_input()) return 1;
			OCR0A = j; // OC0A PB0 Pin 5
			OCR0B = j; // OC0B PB1 Pin 6
			OCR1B = j; // OC1B PB4 PIN 3
			OCR1A = j; // OC1A PB3 PIN 2
			_delay_ms(1);
		}
		if(delay_millis_check_button(30)) return 1;
	}

	return 0;
}

// chase first to last
int seq6(int timeout){
	OCR0A = 0; // OC0A PB0 Pin 5
	OCR0B = 0; // OC0B PB1 Pin 6
	OCR1B = 0; // OC1B PB4 PIN 3
	OCR1A = 0; // OC1A PB3 PIN 2

	uint64_t start_time = millis;
	while((timeout < 0) || (millis < (start_time + (uint64_t)timeout))){

		for(int j = 0; j < 200; j+=2){
			if(check_button_input()) return 1;
			OCR0A = j; // OC0A PB0 Pin 5
			// _delay_ms(1);
		}

		for(int j = 0; j <= 200; j+=2){
			if(check_button_input()) return 1;
			OCR0A = 200 - j; // OC0A PB0 Pin 5
			OCR0B = j; // OC0B PB1 Pin 6
			// _delay_ms(1);
		}

		for(int j = 0; j <= 200; j+=2){
			if(check_button_input()) return 1;
			OCR0B = 200 - j; // OC0B PB1 Pin 6
			OCR1B = j; // OC1B PB4 PIN 3
			// _delay_ms(1);
		}

		for(int j = 0; j <= 200; j+=2){
			if(check_button_input()) return 1;
			OCR1B = 200 - j; // OC1B PB4 PIN 3
			OCR1A = j; // OC1A PB3 PIN 2
			// _delay_ms(1);
		}

		for(int j = 0; j <= 200; j+=2){
			if(check_button_input()) return 1;
			OCR1A = 200 - j; // OC1A PB3 PIN 2
			// _delay_ms(1);
		}

		if(delay_millis_check_button(100)) return 1;
	}

	return 0;
}

// chase first to last then last to first
int seq7(int timeout){
	OCR0A = 0; // OC0A PB0 Pin 5
	OCR0B = 0; // OC0B PB1 Pin 6
	OCR1B = 0; // OC1B PB4 PIN 3
	OCR1A = 0; // OC1A PB3 PIN 2

	uint64_t start_time = millis;
	while((timeout < 0) || (millis < (start_time + (uint64_t)timeout))){

		for(int j = 0; j < 200; j+=2){
			if(check_button_input()) return 1;
			OCR0A = j; // OC0A PB0 Pin 5
			// _delay_ms(1);
		}

		for(int j = 0; j <= 200; j+=2){
			if(check_button_input()) return 1;
			OCR0A = 200 - j; // OC0A PB0 Pin 5
			OCR0B = j; // OC0B PB1 Pin 6
			// _delay_ms(1);
		}

		for(int j = 0; j <= 200; j+=2){
			if(check_button_input()) return 1;
			OCR0B = 200 - j; // OC0B PB1 Pin 6
			OCR1B = j; // OC1B PB4 PIN 3
			// _delay_ms(1);
		}

		for(int j = 0; j <= 200; j+=2){
			if(check_button_input()) return 1;
			OCR1B = 200 - j; // OC1B PB4 PIN 3
			OCR1A = j; // OC1A PB3 PIN 2
			// _delay_ms(1);
		}

		for(int j = 0; j <= 200; j+=2){
			if(check_button_input()) return 1;
			OCR1A = 200 - j; // OC1A PB3 PIN 2
			// _delay_ms(1);
		}

		if(delay_millis_check_button(100)) return 1;

		for(int j = 0; j < 200; j+=2){
			if(check_button_input()) return 1;
			OCR1A = j; // OC0A PB0 Pin 5
			// _delay_ms(1);
		}

		for(int j = 0; j <= 200; j+=2){
			if(check_button_input()) return 1;
			OCR1A = 200 - j; // OC0A PB0 Pin 5
			OCR1B = j; // OC0B PB1 Pin 6
			// _delay_ms(1);
		}

		for(int j = 0; j <= 200; j+=2){
			if(check_button_input()) return 1;
			OCR1B = 200 - j; // OC0B PB1 Pin 6
			OCR0B = j; // OC1B PB4 PIN 3
			// _delay_ms(1);
		}

		for(int j = 0; j <= 200; j+=2){
			if(check_button_input()) return 1;
			OCR0B = 200 - j; // OC1B PB4 PIN 3
			OCR0A = j; // OC1A PB3 PIN 2
			// _delay_ms(1);
		}

		for(int j = 0; j <= 200; j+=2){
			if(check_button_input()) return 1;
			OCR0A = 200 - j; // OC1A PB3 PIN 2
			// _delay_ms(1);
		}

		if(delay_millis_check_button(100)) return 1;
	}

	return 0;
}

// emulate some type of analog brownouts, spikes, etc.
int seq8(int timeout){
	OCR0A = 0; // OC0A PB0 Pin 5
	OCR0B = 0; // OC0B PB1 Pin 6
	OCR1B = 0; // OC1B PB4 PIN 3
	OCR1A = 0; // OC1A PB3 PIN 2

	uint64_t start_time = millis;
	for(int i = 0; i < 60; i++){
		if(check_button_input()) return 1;
		OCR0A = i; // OC0A PB0 Pin 5
		OCR0B = i; // OC0B PB1 Pin 6
		OCR1B = i; // OC1B PB4 PIN 3
		OCR1A = i; // OC1A PB3 PIN 2
		_delay_ms(2);
	}

	while((timeout < 0) || (millis < (start_time + (uint64_t)timeout))){
		for(int i = 0; i < 100; i++){
			if(check_button_input()) return 1;
			_delay_ms(5);
		}
		for(int i = 0; i < 60; i++){
			if(check_button_input()) return 1;
			OCR0A--; // OC0A PB0 Pin 5
			OCR0B--; // OC0B PB1 Pin 6
			OCR1B--; // OC1B PB4 PIN 3
			OCR1A--; // OC1A PB3 PIN 2
			_delay_ms(3);
		}
		for(int i = 0; i < 60; i++){
			if(check_button_input()) return 1;
			OCR0A++; // OC0A PB0 Pin 5
			OCR0B++; // OC0B PB1 Pin 6
			OCR1B++; // OC1B PB4 PIN 3
			OCR1A++; // OC1A PB3 PIN 2
			_delay_ms(4);
		}
		for(int i = 0; i < 100; i++){
			if(check_button_input()) return 1;
			_delay_ms(10);
		}
		for(int i = 0; i < 40; i++){
			if(check_button_input()) return 1;
			OCR0A++; // OC0A PB0 Pin 5
			OCR0B++; // OC0B PB1 Pin 6
			OCR1B++; // OC1B PB4 PIN 3
			OCR1A++; // OC1A PB3 PIN 2
			_delay_ms(2);
		}
		for(int i = 0; i < 40; i++){
			if(check_button_input()) return 1;
			OCR0A--; // OC0A PB0 Pin 5
			OCR0B--; // OC0B PB1 Pin 6
			OCR1B--; // OC1B PB4 PIN 3
			OCR1A--; // OC1A PB3 PIN 2
			_delay_ms(2);
		}
		for(int i = 0; i < 30; i++){
			if(check_button_input()) return 1;
			OCR0A--; // OC0A PB0 Pin 5
			OCR0B--; // OC0B PB1 Pin 6
			OCR1B--; // OC1B PB4 PIN 3
			OCR1A--; // OC1A PB3 PIN 2
			_delay_ms(5);
		}

		for(int i = 0; i < 100; i++){
			if(check_button_input()) return 1;
			_delay_ms(20);
		}
		for(int i = 0; i < 30; i++){
			if(check_button_input()) return 1;
			OCR0A++; // OC0A PB0 Pin 5
			OCR0B++; // OC0B PB1 Pin 6
			OCR1B++; // OC1B PB4 PIN 3
			OCR1A++; // OC1A PB3 PIN 2
			_delay_ms(5);
		}
		for(int i = 0; i < 100; i++){
			if(check_button_input()) return 1;
			_delay_ms(30);
		}
		for(int i = 0; i < 40; i++){
			if(check_button_input()) return 1;
			OCR0A++; // OC0A PB0 Pin 5
			OCR0B++; // OC0B PB1 Pin 6
			OCR1B++; // OC1B PB4 PIN 3
			OCR1A++; // OC1A PB3 PIN 2
			_delay_ms(2);
		}
		for(int i = 0; i < 100; i++){
			if(check_button_input()) return 1;
			_delay_ms(20);
		}
		for(int i = 0; i < 40; i++){
			if(check_button_input()) return 1;
			OCR0A--; // OC0A PB0 Pin 5
			OCR0B--; // OC0B PB1 Pin 6
			OCR1B--; // OC1B PB4 PIN 3
			OCR1A--; // OC1A PB3 PIN 2
			_delay_ms(2);
		}
	}

	return 0;
}

// flash all and fade out
int seq9(int timeout){
	OCR0A = 0; // OC0A PB0 Pin 5
	OCR0B = 0; // OC0B PB1 Pin 6
	OCR1B = 0; // OC1B PB4 PIN 3
	OCR1A = 0; // OC1A PB3 PIN 2
	
	uint64_t start_time = millis;
	while((timeout < 0) || (millis < (start_time + (uint64_t)timeout))){
		for(int i = 0; i < 100; i++){
			if(check_button_input()) return 1;
			OCR0A += 2; // OC0A PB0 Pin 5
			OCR0B += 2; // OC0B PB1 Pin 6
			OCR1B += 2; // OC1B PB4 PIN 3
			OCR1A += 2; // OC1A PB3 PIN 2
		}
		for(int i = 0; i < 8; i++){
			if(check_button_input()) return 1;
			_delay_ms(1);
		}
		for(int i = 0; i < 200; i++){
			if(check_button_input()) return 1;
			OCR0A--; // OC0A PB0 Pin 5
			OCR0B--; // OC0B PB1 Pin 6
			OCR1B--; // OC1B PB4 PIN 3
			OCR1A--; // OC1A PB3 PIN 2
			_delay_ms(35);
		}
		for(int i = 0; i < 100; i++){
			if(check_button_input()) return 1;
			_delay_ms(10);
		}
	}

	return 0;
}

// fade letters in and out randomly
int seq10(int timeout){
	OCR0A = 0; // OC0A PB0 Pin 5
	OCR0B = 0; // OC0B PB1 Pin 6
	OCR1B = 0; // OC1B PB4 PIN 3
	OCR1A = 0; // OC1A PB3 PIN 2

	// led status (4 leds)
	int8_t direction[4] = { 0, 0, 0, 0 };
	uint8_t brightness[4] = { 0, 0, 0, 0 };

	uint64_t start_time = millis;
	while((timeout < 0) || (millis < (start_time + (uint64_t)timeout))){

		if(check_button_input()) return 1;

		// update leds
		for(uint8_t i = 0; i < 4; i++){
			// if inc but not max, inc
			if((direction[i] > 0) && brightness[i] < 255){
				brightness[i]++;
				// if max, dec
				if(brightness[i] >= 255){
					brightness[i] = 255;
					direction[i] = -1;
				}
			}
			// if dec but not min, dec
			else if(direction[i] < 0){
				if(brightness[i] > 0){
					brightness[i]--;
				}
				// if min, stop
				if(brightness[i] <= 0){
					brightness[i] = 0;
					direction[i] = 0;
				}
			}
		}


		OCR0A = brightness[0];
		OCR0B = brightness[1];
		OCR1A = brightness[2];
		OCR1B = brightness[3];


		// chance of choosing an led
		if((rand() % 50) == 0){
			// choose random led
			uint8_t led = (rand() % 4);
			// if led not doing anything, make it light up
			if(0 == direction[led]){
				direction[led] = 1;
			}
		}
	}

	return 0;
}

// fade letters out and in randomly
int seq11(int timeout){
	OCR0A = 255; // OC0A PB0 Pin 5
	OCR0B = 255; // OC0B PB1 Pin 6
	OCR1B = 255; // OC1B PB4 PIN 3
	OCR1A = 255; // OC1A PB3 PIN 2

	// led status (4 leds)
	int8_t direction[4] = { 0, 0, 0, 0 };
	uint8_t brightness[4] = { 255, 255, 255, 255 };

	uint64_t start_time = millis;
	while((timeout < 0) || (millis < (start_time + (uint64_t)timeout))){

		if(check_button_input()) return 1;

		// update leds
		for(uint8_t i = 0; i < 4; i++){
			// if dec but not min, dec
			if((direction[i] < 0) && brightness[i] > 0){
				brightness[i]--;
				// if min, inc 
				if(brightness[i] <= 0){
					brightness[i] = 0;
					direction[i] = 1;
				}
			}
			// if inc but not max, inc
			else if(direction[i] > 0){
				if(brightness[i] < 255){
					brightness[i]++;
				}
				// if max, stop
				if(brightness[i] >= 255){
					brightness[i] = 255;
					direction[i] = 0;
				}
			}
		}


		OCR0A = brightness[0];
		OCR0B = brightness[1];
		OCR1A = brightness[2];
		OCR1B = brightness[3];


		// chance of choosing an led
		if((rand() % 100) == 0){
			// choose random led
			uint8_t led = (rand() % 4);
			// if led not doing anything, make it fade out
			if(0 == direction[led]){
				direction[led] = -1;
			}
		}
	}

	return 0;
}

// flash lights when we hear sounds
int seq12(int timeout){
	OCR0A = 0; // OC0A PB0 Pin 5
	OCR0B = 0; // OC0B PB1 Pin 6
	OCR1B = 0; // OC1B PB4 PIN 3
	OCR1A = 0; // OC1A PB3 PIN 2

	init_mic_buffer();

	uint64_t start_time = millis;
	uint8_t leds = 0;
	uint16_t led_time = millis;
	while((timeout < 0) || (millis < (start_time + (uint64_t)timeout))){
		// update mic buffer
		mic_buffer[mic_buffer_current] = adc_read(1);
		mic_buffer_current++;
		mic_buffer_current = mic_buffer_current % MIC_BUFFER_SIZE;
	
		uint16_t mic = get_mic_buffer_mad();
	

		// try to eliminate noise floor
		if(mic > 40){ 
			mic = mic - 40;
		}
	      	else {
			mic = 0;
		}

		// scale up, avoiding overflow
		if(mic > 63) mic = 63;
		mic = mic<<2;

		leds = (uint8_t)mic;

		OCR0A = leds;
		OCR0B = leds;
		OCR1B = leds;
		OCR1A = leds;

		// fade out leds
		if(millis > led_time + 1){
			led_time = millis;
			if(leds > 0){
				leds--;
			}
		}
		if(check_button_input()) return 1;
	}

	return 0;
}
