/* See COPYING.txt for license details. */

/*
*
*  m1_specter.c
*
*  Specter: passive 13.56 MHz NFC field detector (NipTek flavor).
*
*  Sweeps for an external HF reader's carrier without ever transmitting. Uses:
*    - rfalChipMeasureAmplitude() : analog receiver-input amplitude (0..255)
*    - rfalIsExtFieldOn()         : ST25R3916 hardware external-field detector
*
*  The M1's own field stays OFF the whole time, so any amplitude read is energy
*  coupling in from a nearby reader. Modeled on sub_ghz_frequency_reader()'s
*  display/keypad loop so it matches the rest of the firmware's UX.
*
*  NipTek M1 (Nipahc Technologies) -- addition on top of Monstatek/M1.
*
*/

/******************************** I N C L U D E S *****************************/

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "stm32h5xx_hal.h"
/* main.h pulls in, in the required order: m1_system.h (buttons/keys),
 * m1_display.h + m1_lcd.h (u8g2, fonts, colours, display dims),
 * m1_tasks.h (main_q_hdl, S_M1_Main_Q_t), and m1_buzzer.h. Mirrors the include
 * pattern used by m1_menu.c / m1_sub_ghz.c so header ordering resolves. */
#include "main.h"
#include "m1_specter.h"

#include "app_x-cube-nfcx.h" /* NFC_Polling_Init(), NFC_Polling_DeInit()          */
#include "rfal_chip.h"       /* rfalChipMeasureAmplitude()                        */
#include "rfal_rf.h"         /* rfalIsExtFieldOn()                                */
#include "rfal_utils.h"      /* RFAL_ERR_NONE                                     */

/* Set true by NFC_Polling_Init() when the ST25R3916 answered over SPI. */
extern bool isNFCDeviceOk;

/******************************** D E F I N E S *******************************/

/* The receiver reports a non-zero resting amplitude (measured ~53/255 on
 * hardware) even with no external field, so detection is done RELATIVE to a
 * baseline sampled at startup: strength = amplitude - baseline. */

/* Strength (counts above baseline) at/above which we call a field "present",
 * when the hardware EFD hasn't latched. Set above the resting jitter (~+/-4). */
#define SPECTER_DELTA_THRESHOLD     10U

/* Gauge full-scale in strength counts. External reader fields are far smaller
 * than the full 0..255 range, so scale to this for a lively meter. */
#define SPECTER_FULL_SCALE          120U

/* Samples averaged to establish the no-field baseline at startup / on re-zero. */
#define SPECTER_CAL_SAMPLES         24U

/* Exponential-moving-average smoothing shift for the gauge (higher = smoother,
 * slower). newEMA = ema + (sample - ema) >> SHIFT. */
#define SPECTER_EMA_SHIFT           2U

/* Loop pacing. ~40 ms gives a responsive meter without hogging the CPU. */
#define SPECTER_LOOP_DELAY_MS       40U

/* Gauge bar geometry (display is 128x64). */
#define SPECTER_BAR_X               4
#define SPECTER_BAR_Y               34
#define SPECTER_BAR_W               120
#define SPECTER_BAR_H               12

/**************** F U N C T I O N   I M P L E M E N T A T I O N ****************/

/*============================================================================*/
/*
 * Draw the static chrome (title + bar frame). Called once up front.
 */
/*============================================================================*/
static void specter_draw_static(void)
{
    m1_u8g2_firstpage();
    do
    {
        u8g2_SetDrawColor(&m1_u8g2, M1_DISP_DRAW_COLOR_TXT);
        u8g2_SetFont(&m1_u8g2, M1_DISP_MAIN_MENU_FONT_N);
        u8g2_DrawStr(&m1_u8g2, 1, 10, "Field Detector");
        /* Empty gauge frame */
        u8g2_DrawFrame(&m1_u8g2, SPECTER_BAR_X, SPECTER_BAR_Y, SPECTER_BAR_W, SPECTER_BAR_H);
    } while (m1_u8g2_nextpage());
}

/*============================================================================*/
/*
 * One passive amplitude read on the receiver inputs (our TX stays off).
 * Returns 0..255, or 0 on measurement error.
 */
/*============================================================================*/
static uint8_t specter_read_amp(void)
{
    uint8_t amp = 0;
    if (rfalChipMeasureAmplitude(&amp) != RFAL_ERR_NONE)
    {
        amp = 0;
    }
    return amp;
}

/*============================================================================*/
/*
 * Sample the no-field baseline (average of several reads). Called at startup
 * and on re-zero so "strength" reads ~0 with no external field present.
 */
/*============================================================================*/
static uint8_t specter_calibrate(void)
{
    uint32_t acc = 0;
    uint8_t i;
    for (i = 0; i < SPECTER_CAL_SAMPLES; i++)
    {
        acc += specter_read_amp();
        vTaskDelay(5);
    }
    return (uint8_t)(acc / SPECTER_CAL_SAMPLES);
}

/*============================================================================*/
/*
 * Redraw the dynamic parts: strength number, gauge, status, and a tuning line.
 *   strength  smoothed counts above baseline (0 = no field)
 *   peak      peak-hold of strength
 *   raw       last raw amplitude 0..255 (for tuning)
 *   baseline  calibrated no-field level (for tuning)
 *   present   true if an external field is currently detected
 */
/*============================================================================*/
static void specter_draw_dynamic(uint8_t strength, uint8_t peak, uint8_t raw, uint8_t baseline, bool present)
{
    char line[26];
    uint16_t fill;
    uint8_t clamped = (strength > SPECTER_FULL_SCALE) ? SPECTER_FULL_SCALE : strength;

    /* Bar fill proportional to strength (relative to baseline), inside frame. */
    fill = (uint16_t)(((uint32_t)clamped * (SPECTER_BAR_W - 2)) / SPECTER_FULL_SCALE);

    /* Clear the numeric row, the bar interior, and the status rows. */
    u8g2_SetDrawColor(&m1_u8g2, M1_DISP_DRAW_COLOR_BG);
    u8g2_DrawBox(&m1_u8g2, 0, 14, M1_LCD_DISPLAY_WIDTH, 18);                 /* numeric area */
    u8g2_DrawBox(&m1_u8g2, SPECTER_BAR_X + 1, SPECTER_BAR_Y + 1, SPECTER_BAR_W - 2, SPECTER_BAR_H - 2);
    u8g2_DrawBox(&m1_u8g2, 0, INFO_BOX_Y_POS_ROW_2 - 9, M1_LCD_DISPLAY_WIDTH, 22); /* status rows */
    u8g2_SetDrawColor(&m1_u8g2, M1_DISP_DRAW_COLOR_TXT);

    /* Big strength number. */
    u8g2_SetFont(&m1_u8g2, M1_DISP_LARGE_FONT_2B);
    snprintf(line, sizeof(line), "%3u", (unsigned)strength);
    u8g2_DrawStr(&m1_u8g2, 10, 30, line);
    u8g2_SetFont(&m1_u8g2, M1_DISP_MAIN_MENU_FONT_N);
    u8g2_DrawStr(&m1_u8g2, 78, 28, "fld");

    /* Gauge fill. */
    if (fill > 0)
    {
        u8g2_DrawBox(&m1_u8g2, SPECTER_BAR_X + 1, SPECTER_BAR_Y + 1, (uint8_t)fill, SPECTER_BAR_H - 2);
    }

    /* Status line. */
    u8g2_SetFont(&m1_u8g2, M1_DISP_SUB_MENU_FONT_N);
    u8g2_DrawStr(&m1_u8g2, 1, INFO_BOX_Y_POS_ROW_2,
                 present ? "** FIELD DETECTED **" : "Scanning... (passive)");

    /* Tuning line: raw amplitude, calibrated baseline, peak strength. */
    snprintf(line, sizeof(line), "r%3u b%3u pk%3u", (unsigned)raw, (unsigned)baseline, (unsigned)peak);
    u8g2_DrawStr(&m1_u8g2, 1, INFO_BOX_Y_POS_ROW_3, line);

    m1_u8g2_nextpage();
}

/*============================================================================*/
/*
 * specter_field_detector - passive HF field-strength meter.
 *
 * Registered as an NFC sub-menu leaf. Blocks in its own loop (like the other
 * M1 sub-functions) until BACK is pressed. LEFT clears the peak-hold value.
 */
/*============================================================================*/
void specter_field_detector(void)
{
    S_M1_Buttons_Status this_button_status;
    S_M1_Main_Q_t q_item;
    BaseType_t ret;
    bool nfc_ok;
    uint8_t amp, baseline, strength, ema, peak;
    bool present, was_present;

    platformLog("specter_field_detector()\r\n");

    specter_draw_static();

    /* Full reader bring-up, same as the NFC read menu: registers the ST25R3916
     * IRQ callback, enables the EN_EXT_5V rail, and runs rfalNfcInitialize +
     * discovery config. Calling rfalNfcInitialize() alone is NOT enough -- the
     * chip is unpowered and its init-complete interrupt never fires. ReadIni()
     * (inside here) leaves the field deactivated to idle, so we stay passive. */
    NFC_Polling_Init();
    nfc_ok = isNFCDeviceOk;
    if (!nfc_ok)
    {
        m1_u8g2_firstpage();
        do
        {
            u8g2_SetDrawColor(&m1_u8g2, M1_DISP_DRAW_COLOR_TXT);
            u8g2_SetFont(&m1_u8g2, M1_DISP_MAIN_MENU_FONT_N);
            u8g2_DrawStr(&m1_u8g2, 1, 10, "Field Detector");
            u8g2_SetFont(&m1_u8g2, M1_DISP_SUB_MENU_FONT_N);
            u8g2_DrawStr(&m1_u8g2, 1, 34, "NFC init failed.");
            u8g2_DrawStr(&m1_u8g2, 1, 46, "Press BACK to exit.");
        } while (m1_u8g2_nextpage());
    }

    baseline = nfc_ok ? specter_calibrate() : 0;  /* sample the no-field level */
    amp = baseline;
    ema = 0;
    peak = 0;
    was_present = false;

    while (1)
    {
        if (nfc_ok)
        {
            amp = specter_read_amp();

            /* Strength = amount above the calibrated no-field baseline. */
            strength = (amp > baseline) ? (uint8_t)(amp - baseline) : 0;

            /* Smooth for a steady gauge. */
            ema = (uint8_t)(ema + (((int)strength - (int)ema) >> SPECTER_EMA_SHIFT));
            if (ema > peak) peak = ema;

            /* Field present if the hardware EFD latched, or strength rose clearly. */
            present = rfalIsExtFieldOn() || (ema >= SPECTER_DELTA_THRESHOLD);

            /* Buzz once on the rising edge of a detection. */
            if (present && !was_present)
            {
                m1_buzzer_notification();
            }
            was_present = present;

            specter_draw_dynamic(ema, peak, amp, baseline, present);
        }

        /* --- keypad handling: same pattern as sub_ghz_frequency_reader() --- */
        ret = xQueueReceive(main_q_hdl, &q_item, 0);
        if (ret == pdTRUE)
        {
            if (q_item.q_evt_type == Q_EVENT_KEYPAD)
            {
                ret = xQueueReceive(button_events_q_hdl, &this_button_status, 0);
                if (ret == pdTRUE)
                {
                    if (this_button_status.event[BUTTON_BACK_KP_ID] == BUTTON_EVENT_CLICK)
                    {
                        /* Deactivate + deinit the reader and drop the 5V rail,
                         * mirroring the worker's DONE state, then return. */
                        if (nfc_ok)
                        {
                            NFC_Polling_DeInit();
                            HAL_GPIO_WritePin(EN_EXT_5V_GPIO_Port, EN_EXT_5V_Pin, GPIO_PIN_RESET);
                        }
                        xQueueReset(main_q_hdl);
                        break;
                    }
                    else if (this_button_status.event[BUTTON_LEFT_KP_ID] == BUTTON_EVENT_CLICK)
                    {
                        /* Re-zero: resample the no-field baseline and clear peak. */
                        baseline = specter_calibrate();
                        ema = 0;
                        peak = 0;
                    }
                }
            }
        }

        vTaskDelay(SPECTER_LOOP_DELAY_MS);
    }

    platformLog("specter_field_detector()-exit\r\n");
}
