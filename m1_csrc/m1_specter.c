/* See COPYING.txt for license details. */

/*
*
*  m1_specter.c
*
*  Specter: passive 13.56 MHz NFC field detector (NipTek flavor).
*
*  Sweeps for an external HF reader's carrier without ever transmitting. The
*  primary sense is the ST25R3916 hardware external-field detector, sampled
*  rapidly and reported as a duty-cycle (percent of samples in which a field was
*  present). This tracks pulsed reader fields (e.g. a game console's amiibo
*  reader) far better than a single amplitude read, which barely moves on an
*  external field. Raw receiver amplitude is still shown as a tuning aid.
*
*  Modeled on sub_ghz_frequency_reader()'s display/keypad loop, and brought up
*  via the same NFC_Polling_Init() path the NFC read menu uses.
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
#include "rfal_rf.h"         /* rfalIsExtFieldOn()                                */

/* Set true by NFC_Polling_Init() when the ST25R3916 answered over SPI. */
extern bool isNFCDeviceOk;

/******************************** D E F I N E S *******************************/

/* Field-detect samples taken per display frame, ~1 ms apart. The count of
 * "field present" samples over the window gives a 0..100% duty cycle -- this is
 * the meter value. A ~40 ms window catches short reader pulses. */
#define SPECTER_EFD_SAMPLES         40U

/* Duty-cycle percent at/above which we declare a field present (buzz + banner).
 * Above single-sample noise so it doesn't false-trigger while idle. */
#define SPECTER_DUTY_THRESHOLD      5U

/* Frames to keep the "FIELD DETECTED" banner latched after the last hit, so a
 * brief pulsed field (e.g. amiibo) stays readable. ~40 ms/frame -> ~1.2 s. */
#define SPECTER_DETECT_HOLD         30U

/* The meter jumps up to a new reading instantly but eases back down by this many
 * percent per frame, so a brief pulse doesn't flash straight back to 0. ~4/frame
 * over ~40 ms/frame gives a ~1 s fall from full scale. */
#define SPECTER_LEVEL_DECAY         4U

/* Gauge bar geometry (display is 128x64). */
#define SPECTER_BAR_X               4
#define SPECTER_BAR_Y               34
#define SPECTER_BAR_W               120
#define SPECTER_BAR_H               12

/**************** F U N C T I O N   I M P L E M E N T A T I O N ****************/

/*============================================================================*/
/*
 * Draw the static chrome (title + empty gauge frame). Called once up front.
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
        u8g2_DrawFrame(&m1_u8g2, SPECTER_BAR_X, SPECTER_BAR_Y, SPECTER_BAR_W, SPECTER_BAR_H);
    } while (m1_u8g2_nextpage());
}

/*============================================================================*/
/*
 * Redraw the dynamic parts.
 *   duty     current field-present duty cycle, 0..100 (the meter value)
 *   peak     peak-hold of duty
 *   hits     running count of detection events (each new field seen)
 *   held     true while the detection banner is latched
 */
/*============================================================================*/
static void specter_draw_dynamic(uint8_t duty, uint8_t peak, uint16_t hits, bool held)
{
    char line[26];
    uint16_t fill;

    if (duty > 100U) duty = 100U;
    fill = (uint16_t)(((uint32_t)duty * (SPECTER_BAR_W - 2)) / 100U);

    /* Clear numeric row, bar interior, and status rows. */
    u8g2_SetDrawColor(&m1_u8g2, M1_DISP_DRAW_COLOR_BG);
    u8g2_DrawBox(&m1_u8g2, 0, 14, M1_LCD_DISPLAY_WIDTH, 18);
    u8g2_DrawBox(&m1_u8g2, SPECTER_BAR_X + 1, SPECTER_BAR_Y + 1, SPECTER_BAR_W - 2, SPECTER_BAR_H - 2);
    u8g2_DrawBox(&m1_u8g2, 0, INFO_BOX_Y_POS_ROW_2 - 9, M1_LCD_DISPLAY_WIDTH, 22);
    u8g2_SetDrawColor(&m1_u8g2, M1_DISP_DRAW_COLOR_TXT);

    /* Big duty-cycle number + unit. */
    u8g2_SetFont(&m1_u8g2, M1_DISP_LARGE_FONT_2B);
    snprintf(line, sizeof(line), "%3u", (unsigned)duty);
    u8g2_DrawStr(&m1_u8g2, 10, 30, line);
    u8g2_SetFont(&m1_u8g2, M1_DISP_MAIN_MENU_FONT_N);
    u8g2_DrawStr(&m1_u8g2, 78, 28, "%fld");

    if (fill > 0)
    {
        u8g2_DrawBox(&m1_u8g2, SPECTER_BAR_X + 1, SPECTER_BAR_Y + 1, (uint8_t)fill, SPECTER_BAR_H - 2);
    }

    /* Status line: latched detection banner. */
    u8g2_SetFont(&m1_u8g2, M1_DISP_SUB_MENU_FONT_N);
    u8g2_DrawStr(&m1_u8g2, 1, INFO_BOX_Y_POS_ROW_2,
                 held ? "** FIELD DETECTED **" : "Scanning... (passive)");

    /* Info line: running hit count + peak duty (LEFT resets). */
    snprintf(line, sizeof(line), "Hits:%u  Peak:%u%%", (unsigned)hits, (unsigned)peak);
    u8g2_DrawStr(&m1_u8g2, 1, INFO_BOX_Y_POS_ROW_3, line);

    m1_u8g2_nextpage();
}

/*============================================================================*/
/*
 * specter_field_detector - passive HF field detector.
 *
 * Registered as an NFC sub-menu leaf. Blocks until BACK. LEFT clears peak-hold.
 */
/*============================================================================*/
void specter_field_detector(void)
{
    S_M1_Buttons_Status this_button_status;
    S_M1_Main_Q_t q_item;
    BaseType_t ret;
    bool nfc_ok;
    uint8_t duty, peak, level;
    bool present;
    uint8_t i, hold;
    uint16_t hits, detections;

    platformLog("specter_field_detector()\r\n");

    specter_draw_static();

    /* Full reader bring-up, same as the NFC read menu: registers the ST25R3916
     * IRQ callback, enables the EN_EXT_5V rail, and runs rfalNfcInitialize +
     * discovery config. rfalNfcInitialize() alone is NOT enough (unpowered chip,
     * init IRQ never fires). ReadIni() leaves the field idle, so we stay passive. */
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

    peak = 0;
    level = 0;
    hold = 0;
    detections = 0;

    while (1)
    {
        if (nfc_ok)
        {
            /* Sample the hardware external-field detector across a ~40 ms window
             * (this loop also paces the frame). */
            hits = 0;
            for (i = 0; i < SPECTER_EFD_SAMPLES; i++)
            {
                if (rfalIsExtFieldOn())
                {
                    hits++;
                }
                vTaskDelay(1);
            }
            duty = (uint8_t)(((uint32_t)hits * 100U) / SPECTER_EFD_SAMPLES);
            if (duty > peak) peak = duty;

            /* Peak-hold meter: snap up instantly, ease down slowly so a brief
             * pulse stays readable instead of flashing back to 0. */
            if (duty >= level)
            {
                level = duty;
            }
            else
            {
                level = (level > SPECTER_LEVEL_DECAY) ? (uint8_t)(level - SPECTER_LEVEL_DECAY) : 0U;
            }

            present = (duty >= SPECTER_DUTY_THRESHOLD);
            if (present)
            {
                if (hold == 0)
                {
                    /* rising edge of a new detection: count it and buzz */
                    detections++;
                    m1_buzzer_notification();
                }
                hold = SPECTER_DETECT_HOLD; /* (re)latch the banner */
            }
            else if (hold > 0)
            {
                hold--;
            }

            specter_draw_dynamic(level, peak, detections, (hold > 0));
        }
        else
        {
            vTaskDelay(40);
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
                        peak = 0;        /* clear peak-hold */
                        detections = 0;  /* clear hit counter */
                    }
                }
            }
        }
    }

    platformLog("specter_field_detector()-exit\r\n");
}
