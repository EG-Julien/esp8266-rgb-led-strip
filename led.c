#include <stdio.h>
#include <stdlib.h>
#include <espressif/esp_wifi.h>
#include <espressif/esp_sta.h>
#include <esp/uart.h>
#include <esp8266.h>
#include <FreeRTOS.h>
#include <task.h>
#include <math.h>
#include <multipwm.h>

#include <homekit/homekit.h>
#include <homekit/characteristics.h>
#include "wifi.h"
#include "config.h"

#define H_CUTOFF 1.047196667
#define H_CUTOFF2 H_CUTOFF*2
#define H_CUTOFF4 H_CUTOFF2*2
#define FADE_SPEED 15
#define INITIAL_HUE 0
#define INITIAL_SATURATION 100
#define INITIAL_BRIGHTNESS 100
#define INITIAL_ON true

struct Color {
    float r;
    float g;
    float b;
};

// Constants
struct Color PINK = { 255, 0, 127 };
struct Color BLACK = { 0, 0, 0 };
const char* animateLightPCName = "animate_task";

// Struct used to control the pwm
pwm_info_t pwm_info;
// Task handle used to suspend and resume the animate task (doesn't need to be always running!)
TaskHandle_t animateTH;
// Boolean flag that can be used to gracefully stop the animate task
bool shouldQuitAnimationTask = false;
float led_hue = 0;
float led_saturation = 0;
float led_brightness = 0;
float target_hue = INITIAL_HUE;
float target_saturation = INITIAL_SATURATION;
float target_brightness = INITIAL_BRIGHTNESS;
bool led_on = INITIAL_ON;

const int led_white_gpio = WHITE_GPIO;
bool led_white_on = false;

void led_write(bool on) {
    gpio_write(led_white_gpio, on ? 1 : 0);
}

static void hsi2rgb(float h, float s, float i, struct Color* rgb) {
    // Make sure h is between 0 and 360
    while (h < 0) { h += 360.0f; };
    while (h >= 360) { h -= 360.0f; };

    // Convert h to radians and others from percentage to ratio
    h = M_PI * h / 180.0f;
    s /= 100.0f;
    i /= 100.0f;

    // Clmap s and i
    s = s > 0 ? (s < 1 ? s : 1) : 0;
    i = i > 0 ? (i < 1 ? i : 1) : 0;

    // Shape intensity for higher resolution near 0
    i = i * sqrt(i);

    // Transform depending on h
    if (h < H_CUTOFF2) {
        rgb->r = i / 3 * (1 + s * cos(h) / cos(H_CUTOFF - h));
	rgb->g = i / 3 * (1 + s * (1 - cos(h) / cos(H_CUTOFF - h)));
	rgb->b = i / 3 * (1 - s);
    } else if (h < H_CUTOFF4) {
        h = h - H_CUTOFF2;
	rgb->r = i / 3 * (1 - s);
	rgb->g = i / 3 * (1 + s * cos(h) / cos(H_CUTOFF - h));
	rgb->b = i / 3 * (1 + s * (1 - cos(h) / cos(H_CUTOFF - h)));
    } else {
        h = h - H_CUTOFF4;
	rgb->r = i / 3 * (1 + s * (1 - cos(h) / cos(H_CUTOFF - h)));
	rgb->g = i / 3 * (1 - s);
	rgb->b = i / 3 * (1 + s * cos(h) / cos(H_CUTOFF - h));
    }
}

static void wifi_init() {
    struct sdk_station_config wifi_config = {
        .ssid = "",
        .password = "",
    };

    sdk_wifi_set_opmode(STATION_MODE);
    sdk_wifi_station_set_config(&wifi_config);
    sdk_wifi_station_connect();
}

// Hard write a color
void write_color(struct Color rgb) {
    uint32_t r,g,b;
    r = rgb.r * UINT16_MAX;
    g = rgb.g * UINT16_MAX;
    b = rgb.b * UINT16_MAX;

    multipwm_stop(&pwm_info);
    multipwm_set_duty(&pwm_info, 0, r);
    multipwm_set_duty(&pwm_info, 1, g);
    multipwm_set_duty(&pwm_info, 2, b);
    multipwm_start(&pwm_info);
}

float step_rot(float num, float target, float max, float stepWidth) {
    float distPlus = num < target ? target - num : (max - num + target);
    float distMin = num > target ? num - target : (num + max - target);
    if (distPlus < distMin) {
        if (distPlus > stepWidth) {
            float res = num + stepWidth;
            return res > max ? res - max : res;
        }
    } else {
        if (distMin > stepWidth) {
	    float res = num - stepWidth;
            return res < 0 ? res + max : res;
        }
    }
    return target;
}

float step(float num, float target, float stepWidth) {
    if (num < target) {
        if (num < target - stepWidth) return num + stepWidth;
	return target;
    } else if (num > target) {
        if (num > target + stepWidth) return num - stepWidth;
	return target;
    } 

    return target;
}

void animate_light_transition_task(void* pvParameters) {
    struct Color rgb;
    float t_b;

    while (!shouldQuitAnimationTask) {
	// Compute brightness target value
	if (led_on) t_b = target_brightness;
	else t_b = 0;

	// Do the transition
	while (!(target_hue == led_hue
	    	&& target_saturation == led_saturation
	   	&& t_b == led_brightness)) {

	    // Update led values according to target
	    led_hue = step_rot(led_hue, target_hue, 360, 3.6);
	    led_saturation = step(led_saturation, target_saturation, 1.0);
	    led_brightness = step(led_brightness, t_b, 1.0);

	    // Calculate rgb colors
	    hsi2rgb(led_hue, led_saturation, led_brightness, &rgb);

	    // Write to strip
	    write_color(rgb);

	    // Only do this at most 60 times per second
	    vTaskDelay(FADE_SPEED / portTICK_PERIOD_MS);	
	}
	
	vTaskSuspend(animateTH);
    }


    vTaskDelete(NULL);
}

void led_init() {
    // Initialize pwm
    pwm_info.channels = 3;
    multipwm_init(&pwm_info);
    multipwm_set_pin(&pwm_info, 0, RED_GPIO);
    multipwm_set_pin(&pwm_info, 1, GREEN_GPIO);
    multipwm_set_pin(&pwm_info, 2, BLUE_GPIO);
}

void led_identify_task(void *_args) {
    for (int i=0; i<9; i++) {
	//status_led_write(true);
	write_color(PINK);
	vTaskDelay(100 / portTICK_PERIOD_MS);
	//status_led_write(false);
	write_color(BLACK);
	vTaskDelay(100 / portTICK_PERIOD_MS);
    }

    vTaskDelete(NULL);
}

void led_identify(homekit_value_t _value) {
    xTaskCreate(led_identify_task, "LED identify", 128, NULL, 2, NULL);
}

homekit_value_t led_on_get() {
    return HOMEKIT_BOOL(led_on);
}

void led_on_set(homekit_value_t value) {
    if (value.format != homekit_format_bool) {
        return;
    }

    led_on = value.bool_value;
    vTaskResume(animateTH);
}

homekit_value_t led_brightness_get() {
    return HOMEKIT_INT(target_brightness);
}

void led_brightness_set(homekit_value_t value) {
    if (value.format != homekit_format_int) {
        return;
    }

    target_brightness = value.int_value;
    if (target_brightness > 0) {
	// Turn LED on if value update is received
	    led_on = true;
    }
    vTaskResume(animateTH);
}

homekit_value_t led_hue_get() {
    return HOMEKIT_FLOAT(target_hue);
}

void led_hue_set(homekit_value_t value) {
    if (value.format != homekit_format_float) {
        return;
    }

    target_hue = value.float_value;
    // Always turn on LED on value update
    led_on = true;
    vTaskResume(animateTH);
}

homekit_value_t led_saturation_get() {
    return HOMEKIT_FLOAT(target_saturation);
}

void led_saturation_set(homekit_value_t value) {
    if (value.format != homekit_format_float) {
        return;
    }

    target_saturation = value.float_value;
    // Always turn on LED on value update
    led_on = true;
    vTaskResume(animateTH);
}


homekit_accessory_t *accessories[] = {
    HOMEKIT_ACCESSORY(.id=1, .category=homekit_accessory_category_lightbulb, .services=(homekit_service_t*[]){
        HOMEKIT_SERVICE(ACCESSORY_INFORMATION, .characteristics=(homekit_characteristic_t*[]){
            HOMEKIT_CHARACTERISTIC(NAME, "RGB Led Strip"),
            HOMEKIT_CHARACTERISTIC(MANUFACTURER, "Kariboo"),
            HOMEKIT_CHARACTERISTIC(SERIAL_NUMBER, "17102015JBDH"),
            HOMEKIT_CHARACTERISTIC(MODEL, "Kariboo LED Strip"),
            HOMEKIT_CHARACTERISTIC(FIRMWARE_REVISION, "0.1"),
            HOMEKIT_CHARACTERISTIC(IDENTIFY, led_identify),
            NULL
        }),
        HOMEKIT_SERVICE(LIGHTBULB, .primary=true, .characteristics=(homekit_characteristic_t*[]){
            HOMEKIT_CHARACTERISTIC(NAME, "Led Strip"),
            HOMEKIT_CHARACTERISTIC(
                ON, INITIAL_ON,
                .getter=led_on_get,
                .setter=led_on_set
            ),
	    HOMEKIT_CHARACTERISTIC(
                BRIGHTNESS, INITIAL_BRIGHTNESS,
		.getter=led_brightness_get,
		.setter=led_brightness_set
            ),
	    HOMEKIT_CHARACTERISTIC(
                HUE, INITIAL_HUE,
		.getter=led_hue_get,
		.setter=led_hue_set
            ),
	    HOMEKIT_CHARACTERISTIC(
                SATURATION, INITIAL_SATURATION,
		.getter=led_saturation_get,
		.setter=led_saturation_set
	    ),
            NULL
        }),
        NULL
    }),
    NULL
};

homekit_server_config_t config = {
    .accessories = accessories,
    .password = HOMEKIT_PASSWORD
};

void user_init(void) {
    //uart_set_baud(0, 115200);

    wifi_init();
    led_init();
    homekit_server_init(&config);
    
    // Task for animating the led transition
    xTaskCreate(animate_light_transition_task, animateLightPCName, 1024, NULL, 1, &animateTH);
}
