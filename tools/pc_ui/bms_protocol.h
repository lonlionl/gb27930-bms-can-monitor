/**
 * tools/pc_ui/bms_protocol.h  --  DEV-ONLY STUB, NOT PART OF THE DELIVERY
 *
 * Mirror of the public part of User/Bms/bms_protocol.h that ui_app.c needs.
 * It lives in front of ..\User\Bms on the include path, so ui_app.c compiles
 * on a PC without pulling in stm32f10x_conf.h and the whole StdPeriph library.
 *
 * IMPORTANT: BMS_UiSnapshot_t below must stay field-for-field identical to the
 * real one in User/Bms/bms_protocol.h. If you add a field there, add it here
 * too, otherwise the PC layout preview will silently disagree with the board.
 */

#ifndef __BMS_PROTOCOL_H
#define __BMS_PROTOCOL_H

#include <stdint.h>

typedef enum
{
    BMS_ST_IDLE = 0,
    BMS_ST_HANDSHAKE,
    BMS_ST_IDENTIFY,
    BMS_ST_PARAM_CONFIG,
    BMS_ST_CHARGING_READY,
    BMS_ST_CHARGING,
    BMS_ST_STOPPING,
    BMS_ST_FAULT,
    BMS_ST_MAX
} BMS_State_t;

typedef enum
{
    BMS_ERR_NONE = 0,
    BMS_ERR_HANDSHAKE_TIMEOUT,
    BMS_ERR_IDENTIFY_TIMEOUT,
    BMS_ERR_PARAM_TIMEOUT,
    BMS_ERR_READY_TIMEOUT,
    BMS_ERR_CHARGE_TIMEOUT,
    BMS_ERR_TP,
    BMS_ERR_CML_TIMEOUT,
    BMS_ERR_DATA_INVALID,
    BMS_ERR_BUS_FAULT,
    BMS_ERR_MAX
} BMS_Error_t;

typedef struct
{
    BMS_State_t state;
    BMS_Error_t error;
    uint32_t    session_id;

    uint16_t    soc_x10;
    uint16_t    voltage_x10;
    uint16_t    current_x10;
    uint16_t    cell_max_mv;
    uint8_t     cell_max_no;
    uint8_t     temp_max_c;
    uint8_t     temp_min_c;

    uint16_t    limit_v_x10;
    uint16_t    limit_i_x10;

    uint32_t    charge_seconds;
    uint32_t    energy_x10;

    uint8_t     charge_mode;
    uint8_t     cro_ready;
    uint32_t    rx_count;
    uint32_t    tx_count;
} BMS_UiSnapshot_t;

const char *BMS_StateStr(BMS_State_t st);
const char *BMS_ErrorStr(BMS_Error_t err);
void BMS_Protocol_GetUiSnapshot(BMS_UiSnapshot_t *out);

#endif /* __BMS_PROTOCOL_H */
