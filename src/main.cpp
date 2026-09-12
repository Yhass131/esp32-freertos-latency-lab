
// ========= Libraries  =========
#include <Arduino.h>
#include "driver/ledc.h"
#include "esp32/clk.h"

// ========= Defines    =========
#define TRIG_OUT_GPIO 25
#define TRIG_IN_GPIO 26
#define TRIG_RESPONSE 27

#define TO_CYCLE(us) (uint32_t)((uint64_t)us  * 240)
#define TO_US(us) (uint32_t)((uint64_t)us / 240)
#define TO_US_X1000(us) (uint32_t)((uint64_t)us * 1000 / 240)

#define LATENCY_DEADLINE_US 100 // if the work task is not woken up within this time, count it as a missed interrupt

// ========= Setup      =========

static TaskHandle_t s_workTask = NULL;  // handle to the work task
static TaskHandle_t s_trackTask = NULL; // handle to the track task

volatile uint32_t g_edges = 0; // counts the number of rising edges seen on GPIO 26
volatile uint32_t g_t_isr = 0; // time the ISR was called, in microseconds

volatile uint32_t g_count = 0; // counts the number of samples taken
volatile uint32_t g_average = 0; // average latency in microseconds
volatile uint32_t g_max = 0; // maximum latency in microseconds
volatile uint32_t g_min = 0xFFFFFFFF; // minimum latency in microseconds
volatile uint32_t g_last_latency_us = 0; // last latency measured in microseconds
volatile uint32_t g_miss_count = 0; // counts the number of missed interrupts (if the work task is not woken up in time)

volatile uint32_t g_reset_request = 0; // flag to request a reset of the latency statistics
/*
    This function lives in IRAM (due to IRAM_ATTR),
    when called, garantee call time, call takes
    the same time every time to be fetched deterministic.
    This is ignorant to what ever is happening in flash.
*/
void IRAM_ATTR trig_isr(void)
{
    g_t_isr = esp_cpu_get_ccount(); // microseconds, 64-bit source
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
        .speed_mode = LEDC_LOW_SPEED_MODE,    // low speed mode is the only mode that can use GPIO 25
        .duty_resolution = LEDC_TIMER_10_BIT, // 10-bit resolution, 0-1023
        .timer_num = LEDC_TIMER_0,            // timer 0
        .freq_hz = 1000,                      // 1 kHz
        .clk_cfg = LEDC_USE_APB_CLK,          // use APB clock
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    // configure channel 0 to use timer 0, output on GPIO 25
    ledc_channel_config_t chan = {
        .gpio_num = TRIG_OUT_GPIO,         // GPIO 25
        .speed_mode = LEDC_LOW_SPEED_MODE, // low speed mode is the only mode that can use GPIO 25
        .channel = LEDC_CHANNEL_0,         // channel 0
        .intr_type = LEDC_INTR_DISABLE,    // disable interrupts
        .timer_sel = LEDC_TIMER_0,         // use timer 0
        .duty = 512,                       // how long the pin is high in each cycle, 50% of 2^10
        .hpoint = 0,                       // start at 0
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

    uint64_t latency_sum = 0; // sum of latencies for average calculation

    for (;;)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY); // blocks, zero CPU
        uint32_t t_wake = esp_cpu_get_ccount(); // capture the time the task was woken up

        GPIO.out_w1ts = (1UL << TRIG_RESPONSE); // work starts

        // ========== Latency Statistics =========

        // reset the latency statistics if requested form the printData task
        if (g_reset_request)
        {
            g_count = 0;
            latency_sum = 0;
            g_average = 0;
            g_max = 0;
            g_min = 0xFFFFFFFF;
            g_miss_count = 0;

            g_reset_request = false;
        }

        g_count++; 

        /*
            t_wake is the time the task was woken up, t_isr is the time the ISR was called.
            The difference is the latency of the task being woken up by the ISR.
        */
        g_last_latency_us = t_wake - g_t_isr;
        latency_sum += g_last_latency_us;

        if(TO_US(g_last_latency_us) > LATENCY_DEADLINE_US)
        {
            g_miss_count++;
        }

        g_average = (uint32_t)(latency_sum / g_count);

        if (g_last_latency_us > g_max)
            g_max = g_last_latency_us;

        if (g_last_latency_us < g_min)
            g_min = g_last_latency_us;

        // ==========  =========
        
        GPIO.out_w1tc = (1UL << TRIG_RESPONSE); // work ends
    }
}

void printData(void *pvParameters)
{
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    for (;;)
    {

        uint32_t count = g_count;
        uint32_t miss  = g_miss_count;
        uint32_t avg   = g_average;
        uint32_t mx    = g_max;
        uint32_t mn    = g_min;

        if (count == 0)
        {
            printf("No data yet\n");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        printf("Count: %5u || Average: %u.%2.3u || max: %u.%2.3u || min: %u.%2.3u || latency: %u.%2.3u || missed: %5u || miss rate: %u %%\n",
            count, 
            TO_US_X1000(avg) / 1000, TO_US_X1000(avg) % 1000, 
            TO_US_X1000(mx) / 1000, TO_US_X1000(mx) % 1000, 
            TO_US_X1000(mn) / 1000, TO_US_X1000(mn) % 1000, 
            TO_US_X1000(g_last_latency_us) / 1000, TO_US_X1000(g_last_latency_us) % 1000,
            miss, 
            (miss * 100) / count
        );

        

        g_reset_request = true; // request to reset the latency statistics
        vTaskDelay(pdMS_TO_TICKS(1000));
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
        workTask,    // function
        "work",      // name
        4096,        // stack size
        NULL,        // parameters
        20,           // priority
        &s_workTask, // task handle
        1            // core ID
    );

    xTaskCreatePinnedToCore(
        printData,    // function
        "track",      // name
        4096,         // stack size
        NULL,         // parameters
        1,            // priority
        &s_trackTask, // task handle
        0             // core ID
    );

    /*
     Configures GPIO 26's to run trig_isr on a rising edge.
     Nothing polls; straight hardware.
     Must come after xTaskCreate, an edge arriving while s_workTask
     is still NULL would notify a null handle.
    */
    attachInterrupt(
        digitalPinToInterrupt(TRIG_IN_GPIO), // interrupt tirgger pin
        trig_isr,                            // function to call
        RISING                               // trigger on rising edge
    );

    Serial.println("running");
}

void loop()
{
    delay(1000);
}