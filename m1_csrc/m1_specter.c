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

#include "rfal_nfc.h"      /* rfalNfcInitialize(), rfalNfcDeactivate()           */
#include "rfal_chip.h"     /* rfalChipMeasureAmplitude()                         */
#include "rfal_rf.h"       /* rfalIsExtFieldOn()                                 */
#include "rfal_utils.h"    /* RFAL_ERR_NONE                                      */

/******************************** D E F I N E S *******************************/

/* Amplitude (0..255) at or above which we call the field "present" even if the
 * hardware EFD comparator has not latched. Tuned conservatively; raise it if
 * ambient noise triggers false positives on your unit. */
#define SPECTER_DETECT_THRESHOLD    16U

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
 * Redraw the dynamic parts: numeric amplitude, gauge fill, status + peak.
 *   ema      smoothed amplitude 0..255
 *   peak     peak-hold amplitude 0..255
 *   present  true if an external field is currently detected
 */
/*============================================================================*/
static void specter_draw_dynamic(uint8_t ema, uint8_t peak, bool present)
{
    char line[24];
    uint16_t fill;

    /* Bar fill proportional to smoothed amplitude, kept inside the frame. */
    fill = (uint16_t)(((uint32_t)ema * (SPECTER_BAR_W - 2)) / 255U);

    /* Clear the numeric row and the status rows, then redraw. */
    u8g2_SetDrawColor(&m1_u8g2, M1_DISP_DRAW_COLOR_BG);
    u8g2_DrawBox(&m1_u8g2, 0, 14, M1_LCD_DISPLAY_WIDTH, 18);                 /* numeric area */
    u8g2_DrawBox(&m1_u8g2, SPECTER_BAR_X + 1, SPECTER_BAR_Y + 1, SPECTER_BAR_W - 2, SPECTER_BAR_H - 2); /* inside frame */
    u8g2_DrawBox(&m1_u8g2, 0, INFO_BOX_Y_POS_ROW_2 - 9, M1_LCD_DISPLAY_WIDTH, 22); /* status rows */
    u8g2_SetDrawColor(&m1_u8g2, M1_DISP_DRAW_COLOR_TXT);

    /* Big numeric amplitude + label. */
    u8g2_SetFont(&m1_u8g2, M1_DISP_LARGE_FONT_2B);
    snprintf(line, sizeof(line), "%3u", (unsigned)ema);
    u8g2_DrawStr(&m1_u8g2, 10, 30, line);
    u8g2_SetFont(&m1_u8g2, M1_DISP_MAIN_MENU_FONT_N);
    u8g2_DrawStr(&m1_u8g2, 78, 28, "/255");

    /* Gauge fill. */
    if (fill > 0)
    {
        u8g2_DrawBox(&m1_u8g2, SPECTER_BAR_X + 1, SPECTER_BAR_Y + 1, (uint8_t)fill, SPECTER_BAR_H - 2);
    }

    /* Status line: detection banner or idle. */
    u8g2_SetFont(&m1_u8g2, M1_DISP_SUB_MENU_FONT_N);
    if (present)
    {
        u8g2_DrawStr(&m1_u8g2, 1, INFO_BOX_Y_POS_ROW_2, "** FIELD DETECTED **");
    }
    else
    {
        u8g2_DrawStr(&m1_u8g2, 1, INFO_BOX_Y_POS_ROW_2, "Scanning... (passive)");
    }
    snprintf(line, sizeof(line), "Peak: %3u   BACK=exit", (unsigned)peak);
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
    ReturnCode err;
    uint8_t amp, ema, peak;
    bool present, was_present;
    int i;

    platformLog("specter_field_detector()\r\n");

    specter_draw_static();

    /* Bring up the reader IC. Retry a couple of times like the poller does.
     * On failure, show a message and bail out gracefully. */
    err = RFAL_ERR_NONE;
    for (i = 0; i < 2; i++)
    {
        err = rfalNfcInitialize();
        if (err == RFAL_ERR_NONE) break;
        vTaskDelay(5);
    }
    if (err != RFAL_ERR_NONE)
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
    else
    {
        /* Keep our own field OFF -- purely passive listening. */
        rfalNfcDeactivate(RFAL_NFC_DEACTIVATE_IDLE);
    }

    ema = 0;
    peak = 0;
    was_present = false;

    while (1)
    {
        if (err == RFAL_ERR_NONE)
        {
            /* Passive amplitude read on the receiver inputs (our TX stays off). */
            amp = 0;
            if (rfalChipMeasureAmplitude(&amp) != RFAL_ERR_NONE)
            {
                amp = 0;
            }

            /* Smooth for a steady gauge. */
            ema = (uint8_t)(ema + (((int)amp - (int)ema) >> SPECTER_EMA_SHIFT));
            if (ema > peak) peak = ema;

            /* Field present if the hardware EFD says so, or amplitude is high. */
            present = rfalIsExtFieldOn() || (ema >= SPECTER_DETECT_THRESHOLD);

            /* Buzz once on the rising edge of a detection. */
            if (present && !was_present)
            {
                m1_buzzer_notification();
            }
            was_present = present;

            specter_draw_dynamic(ema, peak, present);
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
                        /* Leave the IC idle with the field off, then return. */
                        if (err == RFAL_ERR_NONE)
                        {
                            rfalNfcDeactivate(RFAL_NFC_DEACTIVATE_IDLE);
                        }
                        xQueueReset(main_q_hdl);
                        break;
                    }
                    else if (this_button_status.event[BUTTON_LEFT_KP_ID] == BUTTON_EVENT_CLICK)
                    {
                        peak = 0; /* clear peak-hold */
                    }
                }
            }
        }

        vTaskDelay(SPECTER_LOOP_DELAY_MS);
    }

    platformLog("specter_field_detector()-exit\r\n");
}
