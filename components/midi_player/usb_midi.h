#ifndef USB_MIDI_H
#define USB_MIDI_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool usb_midi_init(void);
void usb_midi_deinit(void);

/* Отправка MIDI */
void usb_midi_send_note_on(uint8_t channel, uint8_t note, uint8_t velocity);
void usb_midi_send_note_off(uint8_t channel, uint8_t note, uint8_t velocity);
void usb_midi_send_control_change(uint8_t channel, uint8_t control, uint8_t value);
void usb_midi_send_program_change(uint8_t channel, uint8_t program);
void usb_midi_send_pitch_bend(uint8_t channel, int value);

/* Функции для проверки наличия данных и чтения из очереди */
bool usb_midi_available(void);
bool usb_midi_read(uint8_t packet[4]);

#ifdef __cplusplus
}
#endif

#endif