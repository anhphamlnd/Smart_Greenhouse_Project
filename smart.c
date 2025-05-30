#include <mega328p.h>
#include <delay.h>
#include "oled_display.h"
#include <stdint.h>
#include <stdio.h>

// === PIN DEFINITIONS ===
// Smart Gate pins
#define DHT_PIN PIND6
#define FC51_PIN PINC1
#define BUZZER_PIN PIND2
#define SERVO_PIN PINB1

// Clap Switch pins
#define LED_PIN PIND5
#define SOUND_SENSOR_PIN PINC3

// === GLOBAL VARIABLES ===
// Timer & Millis 
volatile unsigned long millis_counter = 0;
volatile unsigned char timer_overflow_count = 0;

// DHT11 variables
unsigned int I_RH, D_RH, I_Temp, D_Temp, CheckSum;
char display_buffer[20];
unsigned char dht_error = 0;

// Gate control variables
unsigned char hasObstacle = 0, past_obstacle = 0;

// Clap switch variables 
unsigned char led_state = 0;
unsigned char sound_detected = 0;
unsigned char last_sound_state = 1; // KY-037 idle = HIGH
unsigned long clap_timer = 0;
unsigned char clap_count = 0;
unsigned char waiting_for_second_clap = 0;

// System timing
unsigned long last_dht_read = 0;
unsigned long last_display_update = 0;

// Debug variables 
unsigned int debug_seconds = 0;
unsigned long debug_last_second = 0;

// Temperature alert variables
unsigned char temp_alert_active = 0;
unsigned long temp_alert_start_time = 0;
unsigned long temp_alert_last_toggle = 0;
unsigned char temp_alert_buzzer_state = 0;

// === TIMER & MILLIS FUNCTIONS ===
void timer0_init() {
    // Clear timer registers
    TCCR0A = 0x00;
    TCCR0B = 0x00;
    TCNT0 = 0x00;
    
    // Configure for CTC mode with proper prescaler
    TCCR0A = (1 << WGM01);  // CTC mode
    TCCR0B = (1 << CS01) | (1 << CS00);  // Prescaler 64
    
    // Set compare value for 1ms interrupt at 16MHz
    // (16MHz / 64) / 1000Hz = 250
    OCR0A = 249;  // 250 - 1
    
    // Enable compare match interrupt
    TIMSK0 = (1 << OCIE0A);
    
    // Enable global interrupts
    #asm("sei");
}

// Interrupt handler
interrupt [TIM0_COMPA] void timer0_compa_isr(void) {
    millis_counter++;
    timer_overflow_count++;
    
    // Reset overflow counter every second for debug
    if (timer_overflow_count >= 1000) {  // 250 * 4ms = 1000ms
        timer_overflow_count = 0;
    }
}

unsigned long millis() {
    unsigned long temp;
    #asm("cli");
    temp = millis_counter;
    #asm("sei");
    return temp;
}

// Function to get seconds with proper handling
unsigned int get_uptime_seconds() {
    unsigned long current_millis = millis();
    return (unsigned int)(current_millis / 1000UL);
}

// Function to format uptime in human readable format
void format_uptime(char* buffer, unsigned long total_seconds) {
    unsigned int days, hours, minutes, seconds;
    
    days = total_seconds / 86400UL;     // 86400 seconds in a day
    total_seconds %= 86400UL;
    
    hours = total_seconds / 3600UL;     // 3600 seconds in an hour
    total_seconds %= 3600UL;
    
    minutes = total_seconds / 60UL;     // 60 seconds in a minute
    seconds = total_seconds % 60UL;
    
    // Format based on time duration
    if (days > 0) {
        if (hours > 0) {
            sprintf(buffer, "%ud%uh%um%us", days, hours, minutes, seconds);
        } else if (minutes > 0) {
            sprintf(buffer, "%ud%um%us", days, minutes, seconds);
        } else {
            sprintf(buffer, "%ud%us", days, seconds);
        }
    } else if (hours > 0) {
        if (minutes > 0) {
            sprintf(buffer, "%uh%um%us", hours, minutes, seconds);
        } else {
            sprintf(buffer, "%uh%us", hours, seconds);
        }
    } else if (minutes > 0) {
        sprintf(buffer, "%um%us", minutes, seconds);
    } else {
        sprintf(buffer, "%us", seconds);
    }
}

// === SERVO CONTROL FUNCTIONS ===
void servo_setup() {
    DDRB |= (1 << 1); // PB1 output (SERVO_PIN)
    
    // Timer1 setup
    TCCR1A = 0x00;
    TCCR1B = 0x00;
    TCCR1A |= (1 << WGM11);
    TCCR1B |= (1 << WGM12) | (1 << WGM13);
    TCCR1A |= (1 << COM1A1);
    TCCR1B |= (1 << CS11); // Prescaler 8
    
    // Set ICR1 = 40000 for 20ms period (exactly like your code)
    ICR1H = 0b10011100;  // 0x9C
    ICR1L = 0b01000000;  // 0x40
    
    // Initial position (closed) - using your exact values
    OCR1AH = 0b00000100;  // 0x04
    OCR1AL = 0b00100100;  // 0x24
}

void open_gate() {
    OCR1AH = 0b00001011;  // 0x0B
    OCR1AL = 0b01111100;  // 0x7C
}

void close_gate() {
    OCR1AH = 0b00000100;  // 0x04
    OCR1AL = 0b00100100;  // 0x24
}

// === DHT11 FUNCTIONS ===
void dht_request() {
    DDRD |= (1 << DHT_PIN);
    PORTD &= ~(1 << DHT_PIN);
    delay_ms(20);
    PORTD |= (1 << DHT_PIN);
    delay_us(30);
}

void dht_response() {
    DDRD &= ~(1 << DHT_PIN);
    while (PIND & (1 << DHT_PIN));
    while (!(PIND & (1 << DHT_PIN)));
    while (PIND & (1 << DHT_PIN));
}

uint8_t dht_receive_data() {
    uint8_t c = 0;
    int q;
    for (q = 0; q < 8; q++) {
        while (!(PIND & (1 << DHT_PIN)));
        delay_us(30);
        if (PIND & (1 << DHT_PIN))
            c = (c << 1) | 1;
        else
            c = (c << 1);
        while (PIND & (1 << DHT_PIN));
    }
    return c;
}

unsigned char read_dht11() {
    dht_request();
    dht_response();
    
    I_RH = dht_receive_data();
    D_RH = dht_receive_data();
    I_Temp = dht_receive_data();
    D_Temp = dht_receive_data();
    CheckSum = dht_receive_data();
    
    // Verify checksum
    if ((I_RH + D_RH + I_Temp + D_Temp) == CheckSum) {
        dht_error = 0;
        return 1; // Success
    } else {
        dht_error = 1;
        return 0; // Error
    }
}

// === TEMPERATURE ALERT FUNCTIONS ===
void check_temperature_alert() {
    unsigned long current_time = millis();
    
    // Check if temperature is above 38�C and no error
    if (!dht_error && I_Temp >= 38) {
        if (!temp_alert_active) {
            // Start temperature alert
            temp_alert_active = 1;
            temp_alert_start_time = current_time;
            temp_alert_last_toggle = current_time;
            temp_alert_buzzer_state = 0;
        }
    } else {
        // Temperature is normal or DHT error - stop alert
        temp_alert_active = 0;
        if (temp_alert_buzzer_state) {
            PORTD &= ~(1 << BUZZER_PIN);  // Turn off buzzer
            temp_alert_buzzer_state = 0;
        }
    }
    
    // Handle temperature alert blinking
    if (temp_alert_active) {
        // Check if 5 seconds have passed
        if (current_time - temp_alert_start_time >= 5000) {
            // 5 seconds completed - stop alert
            temp_alert_active = 0;
            if (temp_alert_buzzer_state) {
                PORTD &= ~(1 << BUZZER_PIN);
                temp_alert_buzzer_state = 0;
            }
        } else {
            // Blink buzzer every 200ms (5Hz frequency)
            if (current_time - temp_alert_last_toggle >= 200) {
                temp_alert_buzzer_state = !temp_alert_buzzer_state;
                temp_alert_last_toggle = current_time;
                
                if (temp_alert_buzzer_state) {
                    PORTD |= (1 << BUZZER_PIN);   // Buzzer on
                } else {
                    PORTD &= ~(1 << BUZZER_PIN);  // Buzzer off
                }
            }
        }
    }
}

// === CLAP SWITCH FUNCTIONS ===
void set_led(unsigned char state) {
    led_state = state;
    if (led_state) {
        PORTD |= (1 << LED_PIN);
    } else {
        PORTD &= ~(1 << LED_PIN);
    }
}

void handle_clap_switch() {
    unsigned long current_time;
    unsigned char current_sound;
    
    current_time = millis();
    current_sound = (PINC & (1 << SOUND_SENSOR_PIN)) ? 1 : 0;
    
    // Detect falling edge (HIGH -> LOW) = sound detected
    if (last_sound_state == 1 && current_sound == 0) {
        if (!waiting_for_second_clap) {
            // First clap
            waiting_for_second_clap = 1;
            clap_timer = current_time;
            clap_count = 1;
        } else if (current_time - clap_timer > 30 && current_time - clap_timer < 200) {
            // Second clap (within 30ms - 200ms) - very fast
            clap_count = 2;
            // Toggle LED immediately
            set_led(!led_state);
            // Reset state
            waiting_for_second_clap = 0;
            clap_count = 0;
            clap_timer = 0;
        }
    }
    
    // Timeout - reset if no second clap within 200ms
    if (waiting_for_second_clap && (current_time - clap_timer > 200)) {
        waiting_for_second_clap = 0;
        clap_count = 0;
        clap_timer = 0;
    }
    
    last_sound_state = current_sound;
}

// === DISPLAY FUNCTIONS ===
void update_display() {
    // Declare all variables at the beginning
    unsigned long current_millis;
    unsigned long uptime_seconds_long;
    unsigned int uptime_seconds;
    unsigned int deciseconds;
    unsigned int raw_counter;
    char uptime_formatted[16];
    
    ssd1306_clear();
    
    // Title
    ssd1306_set_cursor(0, 0);
    ssd1306_print("Smart Garden v2.1");
    
    // DHT11 data with temperature alert indicator
    if (!dht_error) {
        ssd1306_set_cursor(0, 2);
        if (temp_alert_active && I_Temp >= 38) {
            sprintf(display_buffer, "Temp : %d.%d C !HOT!", I_Temp, D_Temp);
        } else {
            sprintf(display_buffer, "Temp : %d.%d C", I_Temp, D_Temp);
        }
        ssd1306_print(display_buffer);
        
        ssd1306_set_cursor(0, 3);
        sprintf(display_buffer, "Hum  : %d.%d %%", I_RH, D_RH);
        ssd1306_print(display_buffer);
    } else {
        ssd1306_set_cursor(0, 2);
        ssd1306_print("DHT11: Error");
    }
    
    // Gate status
    ssd1306_set_cursor(0, 5);
    if (hasObstacle) {
        ssd1306_print("Gate: OPEN");
    } else {
        ssd1306_print("Gate: CLOSE");
    }
    
    // LED status
    ssd1306_set_cursor(0, 6);
    if (led_state) {
        ssd1306_print("LED: ON");
    } else {
        ssd1306_print("LED: OFF");
    }
    
    // Temperature alert status
    ssd1306_set_cursor(68, 5);
    if (temp_alert_active) {
        unsigned long remaining = 5000 - (millis() - temp_alert_start_time);
        sprintf(display_buffer, "ALERT:%us", (unsigned int)(remaining/1000 + 1));
        ssd1306_print(display_buffer);
    }
    
    // Clap debug info (can be removed after testing)
    ssd1306_set_cursor(64, 6);
    if (waiting_for_second_clap) {
        ssd1306_print("Wait2nd");
    } else {
        sprintf(display_buffer, "C:%d", clap_count);
        ssd1306_print(display_buffer);
    }
    
    // System uptime - ENHANCED with human readable format
    ssd1306_set_cursor(0, 7);
    
    // Get current values
    current_millis = millis();
    uptime_seconds = get_uptime_seconds();
    
    if (current_millis > 0 && uptime_seconds > 0) {
        // Format uptime in human readable format
        uptime_seconds_long = (unsigned long)uptime_seconds;
        format_uptime(uptime_formatted, uptime_seconds_long);
        sprintf(display_buffer, "Up:%s", uptime_formatted);
        ssd1306_print(display_buffer);
    } else if (current_millis > 0) {
        // Fallback 1: Show milliseconds/100
        deciseconds = (unsigned int)(current_millis / 100UL);
        sprintf(display_buffer, "Up: %u.%us", deciseconds/10, deciseconds%10);
        ssd1306_print(display_buffer);
    } else {
        // Fallback 2: Show raw counter
        #asm("cli");
        raw_counter = (unsigned int)(millis_counter & 0xFFFF);
        #asm("sei");
        sprintf(display_buffer, "Cnt: %u", raw_counter);
        ssd1306_print(display_buffer);
    }
    
    ssd1306_display();
}

// Debug function to test timer
void debug_timer() {
    unsigned long current_time;
    static unsigned char debug_led = 0;
    
    current_time = millis();
    
    // Update debug seconds counter
    if (current_time - debug_last_second >= 1000) {
        debug_seconds++;
        debug_last_second = current_time;
        
        // Blink built-in LED every second for visual confirmation
        debug_led = !debug_led;
        if (debug_led) {
            PORTB |= (1 << 5);  // Arduino pin 13
        } else {
            PORTB &= ~(1 << 5);
        }
    }
}

// === SYSTEM INITIALIZATION ===
void system_init() {
    int i;
    
    // Configure debug LED (pin 13)
    DDRB |= (1 << 5);
    PORTB &= ~(1 << 5);
    
    // Initialize OLED
    SSD1306_Init();
    ssd1306_clear();
    
    // Show startup message
    ssd1306_set_cursor(0, 2);
    ssd1306_print("Initializing...");
    ssd1306_set_cursor(0, 4);
    ssd1306_print("Anh va Hoi design");
    ssd1306_display();
    delay_ms(2000);
    
    // Initialize timer FIRST - IMPORTANT
    timer0_init();
    
    // Initialize servo
    servo_setup();
    close_gate();
    
    // Configure pins
    // Gate control pins
    DDRC &= ~(1 << FC51_PIN);    // FC51 input
    PORTC |= (1 << FC51_PIN);    // Pull-up
    DDRD |= (1 << BUZZER_PIN);   // Buzzer output
    PORTD &= ~(1 << BUZZER_PIN); // Buzzer off
    
    // Clap switch pins
    DDRD |= (1 << LED_PIN);      // LED output
    PORTD &= ~(1 << LED_PIN);    // LED off
    DDRC &= ~(1 << SOUND_SENSOR_PIN); // Sound sensor input
    PORTC |= (1 << SOUND_SENSOR_PIN); // Pull-up
    
    // Startup LED sequence
    for (i = 0; i < 3; i++) {
        PORTD |= (1 << LED_PIN);
        delay_ms(200);
        PORTD &= ~(1 << LED_PIN);
        delay_ms(200);
    }
    
    // Initialize variables
    led_state = 0;
    sound_detected = 0;
    last_sound_state = 1;
    clap_timer = 0;
    clap_count = 0;
    waiting_for_second_clap = 0;
    hasObstacle = 0;
    past_obstacle = 0;
    last_dht_read = 0;
    last_display_update = 0;
    debug_seconds = 0;
    debug_last_second = 0;
    
    // Initialize temperature alert variables
    temp_alert_active = 0;
    temp_alert_start_time = 0;
    temp_alert_last_toggle = 0;
    temp_alert_buzzer_state = 0;
    
    // Wait for timer to start
    delay_ms(100);
    
    ssd1306_clear();
    ssd1306_set_cursor(0, 3);
    ssd1306_print("System Ready!");
    
    // Show timer status
    ssd1306_set_cursor(0, 5);
    if (millis() > 0) {
        ssd1306_print("Timer: OK");
    } else {
        ssd1306_print("Timer: ERROR");
    }
    
    ssd1306_display();
    delay_ms(1000);
}

// === MAIN FUNCTION ===
void main(void) {
    unsigned long current_time;
    
    // System initialization
    system_init();
    
    while (1) {
        current_time = millis();
        
        // Debug timer function
        debug_timer();
        
        // Read DHT11 every 2 seconds
        if (current_time - last_dht_read >= 2000) {
            read_dht11();
            last_dht_read = current_time;
        }
        
        // Check temperature alert - NEW
        check_temperature_alert();
        
        // Check obstacle sensor
        hasObstacle = !(PINC & (1 << FC51_PIN));
        
        if (hasObstacle) {
            // Object detected - open gate
            if (past_obstacle != hasObstacle) {
                open_gate();
                // Only sound obstacle buzzer if not in temperature alert mode
                if (!temp_alert_active) {
                    PORTD |= (1 << BUZZER_PIN);   // Buzzer on
                    delay_ms(200);
                    PORTD &= ~(1 << BUZZER_PIN);  // Buzzer off
                }
                delay_ms(1000);
            }
        } else {
            // No object - close gate
            close_gate();
        }
        past_obstacle = hasObstacle;
        
        // Process clap switch - CALL ONLY 1 FUNCTION
        handle_clap_switch();
        
        // Update display every 500ms
        if (current_time - last_display_update >= 500) {
            update_display();
            last_display_update = current_time;
        }
        
    }
}
