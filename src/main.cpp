
// ========= Libraries  =========
#include <Arduino.h>
#include "driver/ledc.h"

// ========= Defines    =========
#define TRIG_OUT_GPIO 25
#define TRIG_IN_GPIO 26
#define TRIG_RESPONSE 27

// ========= Setup      =========

static TaskHandle_t s_workTask = NULL;
volatile uint32_t g_edges = 0;

volatile uint32_t g_t_isr = 0;
volatile uint32_t g_last_latency_us = 0;

/*
    This function lives in IRAM (due to IRAM_ATTR),
    when called, garantee call time, call takes
    the same time every time to be fetched deterministic.
    This is ignorant to what ever is happening in flash.
*/
void IRAM_ATTR trig_isr(void)
{
    g_t_isr = (uint32_t)esp_timer_get_time();   // microseconds, 64-bit source
    g_edges++;
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_workTask, &woken);
    // force reschedule on ISR exit; without this
    // the woken task waits for the next tick (~1 ms)
    portYIELD_FROM_ISR(woken);
}

// ========= Functions  =========
/*
    Generates the 1 kHz trigger square wave on GPIO 25, jumpered to GPIO 26.
    LEDC is a PWM peripheral (marketed for LED dimming); at 50% duty it is a
    square wave. Chosen because the peripheral produces it in hardware with
    zero CPU involvement — the wave keeps its timing even if the CPU stalls,
    which is what makes it usable as a measurement reference.
    Stands in for a sensor's data-ready pin.

https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/ledc.html
*/
void trigger_output_start(void)
{
    // configure timer 0 for 1 kHz, 10-bit resolution
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,      // low speed mode is the only mode that can use GPIO 25
        .duty_resolution = LEDC_TIMER_10_BIT,   // 10-bit resolution, 0-1023
        .timer_num = LEDC_TIMER_0,              // timer 0
        .freq_hz = 1000,                        // 1 kHz
        .clk_cfg = LEDC_USE_APB_CLK,            // use APB clock
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    // configure channel 0 to use timer 0, output on GPIO 25
    ledc_channel_config_t chan = {
        .gpio_num = TRIG_OUT_GPIO,              // GPIO 25
        .speed_mode = LEDC_LOW_SPEED_MODE,      // low speed mode is the only mode that can use GPIO 25
        .channel = LEDC_CHANNEL_0,              // channel 0
        .intr_type = LEDC_INTR_DISABLE,         // disable interrupts
        .timer_sel = LEDC_TIMER_0,              // use timer 0
        .duty = 512,                            // how long the pin is high in each cycle, 50% of 2^10
        .hpoint = 0,                            // start at 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&chan));
}

/*
    This function blockes it self, using NO resources but the
    storage it's holding, waiting for a notification.
    Function is put into ready state by trig_isr. When ran,
    sets GPIO 27 high, runs some work, then sets 27 low.
*/
void workTask(void *pvParameters)
{
    for (;;)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY); // blocks, zero CPU

        uint32_t t_wake = (uint32_t)esp_timer_get_time();
        GPIO.out_w1ts = (1UL << TRIG_RESPONSE); // work starts
        
        g_last_latency_us = t_wake - g_t_isr;
        GPIO.out_w1tc = (1UL << TRIG_RESPONSE); // work ends
    }
}
// ========= Setup & Loop   =========

void setup()
{
    Serial.begin(115200);

    pinMode(TRIG_RESPONSE, OUTPUT);
    pinMode(TRIG_IN_GPIO, INPUT_PULLDOWN);

    trigger_output_start();

    xTaskCreatePinnedToCore(
        workTask,               // function
        "work",                 // name
        4096,                   // stack size
        NULL,                   // parameters
        20,                     // priority
        &s_workTask,            // task handle
        1                       // core ID
    ); 

    // Configures GPIO 26's to run trig_isr on a rising edge. 
    // Nothing polls; straight hardware.
    // Must come after xTaskCreate, an edge arriving while s_workTask
    // is still NULL would notify a null handle.
    attachInterrupt(
        digitalPinToInterrupt(TRIG_IN_GPIO), // interrupt tirgger pin
        trig_isr,               // function to call
        RISING                  // trigger on rising edge
    );

    Serial.println("running");
}

void loop()
{
    static uint32_t last = 0;
    uint32_t now = g_edges;
    Serial.printf("edges: %u\n", now - last);
    last = now;
    delay(1000);
}