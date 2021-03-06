/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2018 Johan Kanflo (github.com/kanflo)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "hw.h"
#include "func_cv.h"
#include "uui.h"
#include "uui_number.h"
#include "dbg_printf.h"
#include "mini-printf.h"
#include "cv.h"

/*
 * This is the implementation of the CV screen. It has two editable values, 
 * constant voltage and current limit. When power is enabled it will continously
 * display the current output voltage and current draw. If the user edits one
 * of the values when power is eabled, the other will continue to be updated.
 * Thid allows for ramping the voltage and obsering the current increase.
 */

static void cv_enable(bool _enable);
static void voltage_changed(ui_number_t *item);
static void current_changed(ui_number_t *item);
static void cv_tick(void);
static void past_save(past_t *past);
static void past_restore(past_t *past);
static set_param_status_t set_parameter(char *name, char *value);
static set_param_status_t get_parameter(char *name, char *value, uint32_t value_len);

#define SCREEN_ID  (1)
#define PAST_U     (0)
#define PAST_I     (1)
#define LINE_Y(x) (10 + (x * 24))

/* This is the definition of the voltage item in the UI */
ui_number_t cv_voltage = {
    {
        .type = ui_item_number,
        .id = 10,
        .x = 120,
        .y = LINE_Y(0),
        .can_focus = true,
    },
    .font_size = 24, /** The bigger one, try 0 for kicks */
    .value = 0,
    .min = 0,
    .max = 0, /** Set at init, continously updated in the tick callback */
    .num_digits = 2,
    .num_decimals = 2, /** 2 decimals => value is in centivolts */
    .unit = unit_volt, /** Affects the unit printed on screen */
    .changed = &voltage_changed,
};

/* This is the definition of the current item in the UI */
ui_number_t cv_current = {
    {
        .type = ui_item_number,
        .id = 11,
        .x = 120,
        .y = LINE_Y(1),
        .can_focus = true,
    },
    .font_size = 24,
    .value = 0,
    .min = 0,
    .max = CONFIG_DPS_MAX_CURRENT,
    .num_digits = 1,
    .num_decimals = 3, /** 3 decimals => value is in milliapmere */
    .unit = unit_ampere,
    .changed = &current_changed,
};

/* This is the definition of the voltage item in the UI */
ui_number_t cv_voltage_2 = {
    {
        .type = ui_item_number,
        .id = 10,
        .x = 120,
        .y = LINE_Y(2),
        .can_focus = false,
    },
    .font_size = 24, /** The bigger one, try 0 for kicks */
    .value = 0,
    .min = 0,
    .max = 0, /** Set at init, continously updated in the tick callback */
    .num_digits = 2,
    .num_decimals = 2, /** 2 decimals => value is in centivolts */
    .unit = unit_volt, /** Affects the unit printed on screen */
    .changed = &voltage_changed,
};

/* This is the definition of the current item in the UI */
ui_number_t cv_current_2 = {
    {
        .type = ui_item_number,
        .id = 11,
        .x = 120,
        .y = LINE_Y(3),
        .can_focus = false,
    },
    .font_size = 24,
    .value = 0,
    .min = 0,
    .max = CONFIG_DPS_MAX_CURRENT,
    .num_digits = 1,
    .num_decimals = 3, /** 3 decimals => value is in milliapmere */
    .unit = unit_ampere,
    .changed = &current_changed,
};


/* This is the screen definition */
ui_screen_t cv_screen = {
    .id = SCREEN_ID,
    .name = "cv",
    .icon_data = (uint8_t *) cv,
    .icon_data_len = sizeof(cv),
    .icon_width = cv_width,
    .icon_height = cv_height,
    .enable = &cv_enable,
    .past_save = &past_save,
    .past_restore = &past_restore,
    .tick = &cv_tick,
    .set_parameter = &set_parameter,
    .get_parameter = &get_parameter,
    .num_items = 4,
    .parameters = {
        {
            .name = "voltage",
            .unit = unit_volt,
            .prefix = si_milli
        },
        {
            .name = "current",
            .unit = unit_ampere,
            .prefix = si_milli
        },
        {
            .name = {'\0'} /** Terminator */
        },
    },
    .items = { (ui_item_t*) &cv_voltage, (ui_item_t*) &cv_current, (ui_item_t*) &cv_voltage_2, (ui_item_t*) &cv_current_2 }
};

/**
 * @brief      Set function parameter
 *
 * @param[in]  name   name of parameter
 * @param[in]  value  value of parameter as a string - always in SI units
 *
 * @retval     set_param_status_t status code 
 */
static set_param_status_t set_parameter(char *name, char *value)
{
    int32_t ivalue = atoi(value);
    if (strcmp("voltage", name) == 0 || strcmp("u", name) == 0) {
        if (ivalue < cv_voltage.min || ivalue > cv_voltage.max) {
            emu_printf("[CV] Voltage %d is out of range (min:%d max:%d)\n", ivalue, cv_voltage.min, cv_voltage.max);
            return ps_range_error;
        }
        emu_printf("[CV] Setting voltage to %d\n", ivalue);
        /** value received in millivolt, module internal representation is centivolt */
        cv_voltage.value = ivalue / 10;
        voltage_changed(&cv_voltage);
        return ps_ok;
    } else if (strcmp("current", name) == 0 || strcmp("i", name) == 0) {
        if (ivalue < cv_current.min || ivalue > cv_current.max) {
            emu_printf("[CV] Current %d is out of range (min:%d max:%d)\n", ivalue, cv_current.min, cv_current.max);
            return ps_range_error;
        }
        emu_printf("[CV] Setting current to %d\n", ivalue);
        cv_current.value = ivalue;
        current_changed(&cv_current);
        return ps_ok;
    }
    return ps_unknown_name;
}

/**
 * @brief      Get function parameter
 *
 * @param[in]  name       name of parameter
 * @param[in]  value      value of parameter as a string - always in SI units
 * @param[in]  value_len  length of value buffer
 *
 * @retval     set_param_status_t status code 
 */
static set_param_status_t get_parameter(char *name, char *value, uint32_t value_len)
{
    if (strcmp("voltage", name) == 0 || strcmp("u", name) == 0) {
        /** value returned in millivolt, module internal representation is centivolt */
        (void) mini_snprintf(value, value_len, "%d", 10 * cv_voltage.value);
        return ps_ok;
    } else if (strcmp("current", name) == 0 || strcmp("i", name) == 0) {
        (void) mini_snprintf(value, value_len, "%d", cv_current.value);
        return ps_ok;
    }
    return ps_unknown_name;
}

/**
 * @brief      Callback for when the function is enabled
 *
 * @param[in]  enabled  true when function is enabled
 */
static void cv_enable(bool enabled)
{
    emu_printf("[CV] %s output\n", enabled ? "Enable" : "Disable");
    if (enabled) {
        (void) pwrctl_set_vout(10 * cv_voltage.value);
        (void) pwrctl_set_iout(CONFIG_DPS_MAX_CURRENT);
        (void) pwrctl_set_ilimit(cv_current.value);
        pwrctl_enable_vout(true);
    } else {
        pwrctl_enable_vout(false);
    }
}

/**
 * @brief      Callback for when value of the voltage item is changed
 *
 * @param      item  The voltage item
 */
static void voltage_changed(ui_number_t *item)
{
    cv_voltage.value = item->value;
    (void) pwrctl_set_vout(10 * item->value);
    cv_voltage.ui.draw(&cv_voltage.ui);
}

/**
 * @brief      Callback for when value of the current item is changed
 *
 * @param      item  The current item
 */
static void current_changed(ui_number_t *item)
{
    cv_current.value = item->value;
    (void) pwrctl_set_iout(item->value);
    cv_current.ui.draw(&cv_current.ui);
}

/**
 * @brief      Save persistent parameters
 *
 * @param      past  The past
 */
static void past_save(past_t *past)
{
    /** @todo: past bug causes corruption for units smaller than 4 bytes (#27) */
    if (!past_write_unit(past, (SCREEN_ID << 24) | PAST_U, (void*) &cv_voltage.value, 4 /* sizeof(cv_voltage.value) */ )) {
        /** @todo: handle past write failures */
    }
    if (!past_write_unit(past, (SCREEN_ID << 24) | PAST_I, (void*) &cv_current.value, 4 /* sizeof(cv_current.value) */ )) {
        /** @todo: handle past write failures */
    }
}

/**
 * @brief      Restore persistent parameters
 *
 * @param      past  The past
 */
static void past_restore(past_t *past)
{
    uint32_t length;
    uint32_t *p = 0;
    if (past_read_unit(past, (SCREEN_ID << 24) | PAST_U, (const void**) &p, &length)) {
        cv_voltage.value = *p;
        (void) length;
    }
    if (past_read_unit(past, (SCREEN_ID << 24) | PAST_I, (const void**) &p, &length)) {
        cv_current.value = *p;
        (void) length;
    }
}

/**
 * @brief      Update the UI. We need to be careful about the values shown
 *             as they will differ depending on the current state of the UI
 *             and the current power output mode.
 *             Power off: always show current setting
 *             Power on : show current output value unless the item has focus
 *                        in which case we shall display the current setting.
 */
static void cv_tick(void)
{
    uint16_t i_out_raw, v_in_raw, v_out_raw;
    hw_get_adc_values(&i_out_raw, &v_in_raw, &v_out_raw);

    /** Continously update max voltage output value */
    cv_voltage.max = pwrctl_calc_vin(v_in_raw) / 10;

    /** No focus, update display if necessary */
    int32_t new_u = pwrctl_calc_vout(v_out_raw) / 10;
    if (new_u != cv_voltage_2.value) {
        cv_voltage_2.value = new_u;
        cv_voltage_2.ui.draw(&cv_voltage_2.ui);
    }

    /** No focus, update display if necessary */
    int32_t new_i = pwrctl_calc_iout(i_out_raw);
    if (new_i != cv_current_2.value) {
        cv_current_2.value = new_i;
        cv_current_2.ui.draw(&cv_current_2.ui);
    }
}

/**
 * @brief      Initialise the CV module and add its screen to the UI
 *
 * @param      ui    The user interface
 */
void func_cv_init(uui_t *ui)
{
    cv_voltage.value = 0; /** read from past */
    cv_current.value = 0; /** read from past */
    uint16_t i_out_raw, v_in_raw, v_out_raw;
    hw_get_adc_values(&i_out_raw, &v_in_raw, &v_out_raw);
    (void) i_out_raw;
    (void) v_out_raw;
    cv_voltage.max = pwrctl_calc_vin(v_in_raw) / 10; /** @todo: subtract for LDO */
    number_init(&cv_voltage); /** @todo: add guards for missing init calls */
    /** Start at the second most significant digit preventing the user from 
        accidentally cranking up the setting 10V or more */
    cv_voltage.cur_digit = 2;
    number_init(&cv_current);
    number_init(&cv_voltage_2);
    number_init(&cv_current_2);
    uui_add_screen(ui, &cv_screen);
}
