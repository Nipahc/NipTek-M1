/* See COPYING.txt for license details. */

/*
*
*  m1_specter.h
*
*  Specter: passive 13.56 MHz NFC field detector (NipTek flavor).
*
*  Inspired by the Flipper Zero "Specter" app. Detects an external HF reader's
*  field using the ST25R3916's receiver amplitude measurement and hardware
*  external-field detector. The M1 never transmits in this mode -- it only listens.
*
*  NipTek M1 (Nipahc Technologies) -- addition on top of Monstatek/M1.
*
*/

#ifndef M1_SPECTER_H_
#define M1_SPECTER_H_

/* Menu entry point: live passive field-strength meter. Blocks until the user
 * presses BACK, then returns to the NFC sub-menu. */
void specter_field_detector(void);

#endif /* M1_SPECTER_H_ */
