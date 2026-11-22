/*
 * THE CITADEL - DEPARTMENT OF ELECTRICAL & COMPUTER ENGINEERING
 * Engineer:  Thomas Bassett
 * Created:   2026-11-21
 * Modified:  2026-11-22
 *
 * Project:   Smart Grid Test Bench
 * Module:    M4 - Supervisory Control and Data Acquisition (SCADA)
 * Level:     1C - Local Controllers (link between two local controllers)
 * File:      Controller Link Frames & Link Supervision (ICD-4 rev 0.2, Tables ICD-4.3, ICD-4.4).
 *
 * Purpose:   Messages exchanged between switchyard controller (Module 2) and its measurement node,
 *            and check that link between them is alive.
 *
 * Notes:     Controller frame: message sent by controller to node.
 *            Node frame: message sent by node to controller.
 *            Stale: no valid controller frame has arrived within timeout.
 *            PROVISIONAL until Module 2 fw header is shared: byte order, framing, CRC, and enum values.
 *            Transfer of fault record (ICD-4_4.4.1) is not coded: OI-28.
 */
#ifndef ICD4_H
#define ICD4_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ICD4_CTRL_LEN  32u    /* controller frame length (bytes) */
#define ICD4_NODE_LEN  26u    /* node frame length (bytes) */
#define ICD4_STALE_MS  300u   /* time w/o frame after which link is stale (ms) */

/* fault types (OBJ 2.4.1). */
enum { ICD4_FAULT_NONE = 0, ICD4_FAULT_SHORT = 1, ICD4_FAULT_OPEN = 2, ICD4_FAULT_INTERMITTENT = 3, ICD4_FAULT_OVERLOAD = 4 };
#define ICD4_REASON_MAX 13u   /* highest reason code (Table ICD-4.5) */

/* controller frame. */
typedef struct {
    uint16_t seq;           /* heartbeat */
    uint32_t t_ms;          /* controller time of latest state change (ms) */
    uint16_t sw_state;      /* switch states */
    uint8_t  trip_evt;      /* trip event */
    uint8_t  fault_type;    /* fault type */
    uint8_t  flisr_state;   /* FLISR state */
    float    i_prot[3];     /* protection currents (A) */
    uint8_t  reason_code;   /* reason code */
    uint8_t  donor_id;      /* donor switchyard of restoration */
    uint32_t restore_ms;    /* duration of restoration (ms) */
    uint8_t  alarms;        /* alarm flags */
} icd4_ctrl_t;

/* node frame. */
typedef struct {
    uint16_t seq;                   /* heartbeat */
    uint32_t t_ms;                  /* node time of measurement (ms) */
    float    v_sub, i_sub, p_sub;   /* switchyard side: voltage (V), current (A), active power (W) */
    float    p_load;                /* load power (W) */
    uint16_t health;                /* node status word */
} icd4_node_t;

/* result of decoding frame. */
typedef enum { ICD4_OK = 0, ICD4_ERR_LENGTH = 1, ICD4_ERR_CRC = 2 } icd4_err_t;

/* encode controller frame; return its length. */
size_t     icd4_encode_ctrl(const icd4_ctrl_t *f, uint8_t out[ICD4_CTRL_LEN]);

/* decode controller frame. */
icd4_err_t icd4_decode_ctrl(const uint8_t *buf, size_t len, icd4_ctrl_t *f);

/* encode node frame; return its length. */
size_t     icd4_encode_node(const icd4_node_t *f, uint8_t out[ICD4_NODE_LEN]);

/* decode node frame. */
icd4_err_t icd4_decode_node(const uint8_t *buf, size_t len, icd4_node_t *f);

/* change flags: fields that differ between two controller frames. */
#define ICD4_CHG_SWITCH   0x01u   /* switch states */
#define ICD4_CHG_TRIP     0x02u   /* trip event or fault type */
#define ICD4_CHG_FLISR    0x04u   /* FLISR state */
#define ICD4_CHG_REASON   0x08u   /* reason code */
#define ICD4_CHG_RESTORE  0x10u   /* restoration report */
#define ICD4_CHG_ALARMS   0x20u   /* alarm flags */

/* return change flags between two controller frames. */
uint8_t icd4_changes(const icd4_ctrl_t *before, const icd4_ctrl_t *after);

/* node side of link. */
typedef struct {
    icd4_ctrl_t last;      /* latest controller frame */
    bool     valid;        /* true = valid frame has been received */
    bool     stale;        /* true = link is stale */
    uint32_t last_rx_ms;   /* time of latest valid frame (ms) */
    uint8_t  changed;      /* change flags not yet taken */
    uint32_t frames, crc_errors, length_errors, lost;   /* frames received, CRC errors, length errors, frames lost */
} icd4_link_t;

/* prepare link for use. */
void    icd4_link_init(icd4_link_t *l);

/* take in one received controller frame; return true when it is valid. */
bool    icd4_link_rx(icd4_link_t *l, const uint8_t *buf, size_t len, uint32_t now_ms);

/* update stale state; called periodically. */
void    icd4_link_tick(icd4_link_t *l, uint32_t now_ms);

/* return change flags not yet taken, and clear them. */
uint8_t icd4_link_take(icd4_link_t *l);

#endif   /* ICD4_H */
