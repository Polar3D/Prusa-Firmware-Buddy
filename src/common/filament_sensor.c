// filament_sensor.c

//there is 10kOhm PU to 5V in filament sensor
//mcu PU/PD is in range 30 - 50 kOhm

//when PD is selected and sensor is connected Vmcu = min 3.75V .. (5V * 30kOhm) / (30 + 10 kOhm)
//pin is 5V tolerant

//mcu has 5pF, transistor D-S max 15pF
//max R is 50kOhm
//Max Tau ~= 20*10^-12 * 50*10^3 = 1*10^-6 s ... about 1us

#include "filament_sensor.h"
#include "hwio_pindef.h" //PIN_FSENSOR
#include "stm32f4xx_hal.h"
#include "gpio.h"
#include "eeprom.h"
#include "FreeRTOS.h" //must apper before include task.h
#include "task.h" //critical sections
#include "cmsis_os.h" //osDelay
#include "marlin_server.h" //enable/disable fs in marlin

static volatile fsensor_t state = FS_NOT_INICIALIZED;
static volatile fsensor_t last_state = FS_NOT_INICIALIZED;
static uint8_t meas_cycle = 0;

/*---------------------------------------------------------------------------*/
//local functions

//simple filter
//without filter fs_meas_cycle1 could set FS_NO_SENSOR (in case filament just runout)
void _set_state(fsensor_t st) {
    taskENTER_CRITICAL();
    if (last_state == st)
        state = st;
    last_state = st;
    taskEXIT_CRITICAL();
}

void _enable() {
    gpio_init(PIN_FSENSOR, GPIO_MODE_INPUT, GPIO_PULLUP, GPIO_SPEED_FREQ_VERY_HIGH); // pullup
    state = FS_NOT_INICIALIZED;
    last_state = FS_NOT_INICIALIZED;
    meas_cycle = 0;
    marlin_fs_enable();
}

void _disable() {
    state = FS_DISABLED;
    last_state = FS_DISABLED;
    meas_cycle = 0;
    marlin_fs_disable();
}

/*---------------------------------------------------------------------------*/
//global thread safe functions
fsensor_t fs_get_state() {
    return state;
}

//value can change during read, but it is not a problem
int fs_did_filament_runout() {
    return state == FS_NO_FILAMENT;
}

/*---------------------------------------------------------------------------*/
//global thread safe functions
//but cannot be called from interrupt
void fs_enable() {
    taskENTER_CRITICAL();
    _enable();
    eeprom_set_var(EEVAR_FSENSOR_ENABLED, variant8_ui8(1));
    taskEXIT_CRITICAL();
}

void fs_disable() {
    taskENTER_CRITICAL();
    _disable();
    eeprom_set_var(EEVAR_FSENSOR_ENABLED, variant8_ui8(0));
    taskEXIT_CRITICAL();
}

fsensor_t fs_wait_inicialized() {
    fsensor_t ret = fs_get_state();
    while (ret == FS_NOT_INICIALIZED) {
        osDelay(0); // switch to other threads
        ret = fs_get_state();
    }
    return ret;
}

/*---------------------------------------------------------------------------*/
//global not thread safe functions
void fs_init() {
    int enabled = eeprom_get_var(EEVAR_FSENSOR_ENABLED).ui8 ? 1 : 0;

    if (enabled)
        _enable();
    else
        _disable();
}

//called only in fs_cycle
void _cycle0() {
    if (gpio_get(PIN_FSENSOR) == 1) {
        gpio_init(PIN_FSENSOR, GPIO_MODE_INPUT, GPIO_PULLDOWN, GPIO_SPEED_FREQ_VERY_HIGH); // pulldown
        meas_cycle = 1; //next cycle shall be 1
    } else {
        _set_state(FS_NO_FILAMENT);
        meas_cycle = 0; //remain in cycle 0
    }
}

//called only in fs_cycle
void _cycle1() {
    //pulldown was set in cycle 0
    _set_state(gpio_get(PIN_FSENSOR) == 1 ? FS_HAS_FILAMENT : FS_NOT_CONNECTED);
    gpio_init(PIN_FSENSOR, GPIO_MODE_INPUT, GPIO_PULLUP, GPIO_SPEED_FREQ_VERY_HIGH); // pullup
    meas_cycle = 0; //next cycle shall be 0
}

//dealay between calls must be 1us or longer
void fs_cycle() {
    //sensor is disabled (only init can enable it)
    if (state == FS_DISABLED)
        return;

    //sensor is enabled
    if (meas_cycle == 0) {
        _cycle0();
    } else {
        _cycle1();
    }
}