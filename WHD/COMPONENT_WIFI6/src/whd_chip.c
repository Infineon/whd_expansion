/*
 * Copyright 2025, Cypress Semiconductor Corporation (an Infineon company)
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "bus_protocols/whd_chip_reg.h"
#include "bus_protocols/whd_sdio.h"
#include "bus_protocols/whd_bus_common.h"
#include "bus_protocols/whd_bus_protocol_interface.h"
#include "whd_chip_constants.h"
#ifndef PROTO_MSGBUF
#include "whd_cdc_bdc.h"
#endif /* PROTO_MSGBUF */
#include "whd_thread_internal.h"
#include "whd_buffer_api.h"
#include "whd_debug.h"
#include "whd_utils.h"
#include "whd_wifi_api.h"
#include "whd_chip.h"
#include "whd.h"
#include "whd_wlioctl.h"
#include "whd_proto.h"
#ifdef PROTO_MSGBUF
#include "whd_ring.h"
#endif
/******************************************************
*                      Macros
******************************************************/
#define SPINWAIT_POLL_PERIOD    10

#define SPINWAIT(exp, us) { \
        uint countdown = (us) + (SPINWAIT_POLL_PERIOD - 1); \
        while ( (exp) && (countdown >= SPINWAIT_POLL_PERIOD) ){ \
            cy_rtos_delay_milliseconds(SPINWAIT_POLL_PERIOD); \
            countdown -= SPINWAIT_POLL_PERIOD; \
        } \
}
/******************************************************
*             Constants
******************************************************/

#define PLATFORM_WLAN_RAM_BASE      (0x0)
#define WLAN_BUS_UP_ATTEMPTS        (1000)
#define HT_AVAIL_WAIT_MS            (1)
#define KSO_WAIT_MS                 (1)
#define KSO_WAKE_MS                 (3)
#define MAX_KSO_ATTEMPTS            (64)
#define MAX_CAPS_BUFFER_SIZE        (768)

#define AI_IOCTRL_OFFSET            (0x408)
#define SICF_FGC                    (0x0002)
#define SICF_CLOCK_EN               (0x0001)
#define AI_RESETCTRL_OFFSET         (0x800)
#define AI_RESETSTATUS_OFFSET       (0x804)
#define AIRC_RESET                  (1)
#define WRAPPER_REGISTER_OFFSET     (0x100000)

#define WLAN_SHARED_VERSION_MASK    (0x00ff)
#define WLAN_SHARED_VERSION         (0x0003)
#define WPRINT_WHD_DEBUG_DS(args) WPRINT_WHD_DEBUG(args)

#ifdef CYCFG_ULP_SUPPORT_ENABLED
#define WAKE_FROM_UCODE_TIMEOUT_MS     (5000) //5000 ms
#define WAKE_FROM_UCODE_TIMEOUT_LOOPS  (100)
#define WAKE_FROM_UCODE_CHECK_PER_LOOP (WAKE_FROM_UCODE_TIMEOUT_MS/WAKE_FROM_UCODE_TIMEOUT_LOOPS) //50 ms
#endif

/******************************************************
*             Structures
******************************************************/

/******************************************************
*             Variables
******************************************************/
static const whd_fwcap_t whd_fwcap_map[] =
{
    {WHD_FWCAP_SAE, "sae "},
    {WHD_FWCAP_SAE_EXT, "sae_ext "},
    {WHD_FWCAP_OFFLOADS, "offloads "},
    {WHD_FWCAP_GCMP, "gcmp "},
    {WHD_FWCAP_ICMP, "icmp "},
};
/******************************************************
*             Static Function Declarations
******************************************************/
static whd_bool_t whd_is_fw_sr_capable(whd_driver_t whd_driver);
static whd_result_t whd_kso_enable(whd_driver_t whd_driver, whd_bool_t enable);
static uint32_t whd_get_core_address(whd_driver_t whd_driver, device_core_t core_id);

static whd_result_t whd_enable_save_restore(whd_driver_t whd_driver);

/******************************************************
*             Function definitions
******************************************************/
whd_result_t whd_internal_info_init(whd_driver_t whd_driver)
{
    whd_internal_info_t *internal_info = &whd_driver->internal_info;

    internal_info->whd_wlan_status.state = WLAN_OFF;
    internal_info->whd_wlan_status.country_code = WHD_COUNTRY_AUSTRALIA;
    internal_info->whd_wlan_status.aggregate_code = WHD_COUNTRY_AGGREGATE_XV_0;
    internal_info->whd_wlan_status.keep_wlan_awake = 0;
    internal_info->console_addr = 0;
    internal_info->scan_result_callback = NULL;
    internal_info->whd_scan_result_ptr = NULL;
    internal_info->active_join_mutex_initted = WHD_FALSE;
    internal_info->active_join_semaphore = NULL;
    internal_info->con_lastpos = 0;
    internal_info->whd_wifi_p2p_go_is_up = WHD_FALSE;

#ifdef WHD_IOCTL_LOG_ENABLE
    /* Create the mutex protecting whd_log structure */
    if (cy_rtos_init_semaphore(&whd_driver->whd_log_mutex, 1, 0) != WHD_SUCCESS)
    {
        return WHD_SEMAPHORE_ERROR;
    }
    if (cy_rtos_set_semaphore(&whd_driver->whd_log_mutex, WHD_FALSE) != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR( ("Error setting semaphore in %s at %d \n", __func__, __LINE__) );
        return WHD_SEMAPHORE_ERROR;
    }
#endif
    return WHD_SUCCESS;
}

whd_result_t whd_internal_info_deinit(whd_driver_t whd_driver)
{
#ifdef WHD_IOCTL_LOG_ENABLE
    /* Delete the whd_log mutex */
    (void)cy_rtos_deinit_semaphore(&whd_driver->whd_log_mutex);
#endif
    return WHD_SUCCESS;
}

/*
 * Returns the base address of the core identified by the provided coreId
 */
uint32_t whd_get_core_address(whd_driver_t whd_driver, device_core_t core_id)
{
    if (core_id == WLAN_ARM_CORE)
    {
        return GET_C_VAR(whd_driver, ARM_CORE_BASE_ADDRESS);
    }
    else if (core_id == SOCRAM_CORE)
    {
        return GET_C_VAR(whd_driver, SOCSRAM_WRAPPER_BASE_ADDRESS);
    }
    else if (core_id == SDIOD_CORE)
    {
        return GET_C_VAR(whd_driver, SDIOD_CORE_BASE_ADDRESS);
    }
    else
    {
        WPRINT_WHD_ERROR( ("%s:%d Invalid core ID(%d)\n", __FUNCTION__, __LINE__, core_id) );
    }

    return WHD_BADARG;

}

/*
 * Returns WHD_SUCCESS is the core identified by the provided coreId is up, otherwise WHD result code
 */
whd_result_t whd_device_core_is_up(whd_driver_t whd_driver, device_core_t core_id)
{
    uint8_t regdata;
    uint32_t base;
    whd_result_t result;

    base = whd_get_core_address(whd_driver, core_id);

    /* Read the IO control register */
    result = whd_bus_read_backplane_value(whd_driver, base + AI_IOCTRL_OFFSET, (uint8_t)1, &regdata);
    if (result != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR( ("%s:%d whd_bus_read_backplane_value failed\n", __FUNCTION__, __LINE__) );
        return result;
    }

    /* Verify that the clock is enabled and something else is not on */
    if ( (regdata & (SICF_FGC | SICF_CLOCK_EN) ) != ( uint8_t )SICF_CLOCK_EN )
    {
        return WHD_CORE_CLOCK_NOT_ENABLED;
    }

    /* Read the reset control and verify it is not in reset */
    result = whd_bus_read_backplane_value(whd_driver, base + AI_RESETCTRL_OFFSET, (uint8_t)1, &regdata);
    if (result != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR( ("%s:%d whd_bus_read_backplane_value failed\n", __FUNCTION__, __LINE__) );
        return result;
    }
    if ( (regdata & AIRC_RESET) != 0 )
    {
        return WHD_CORE_IN_RESET;
    }

    return WHD_SUCCESS;
}

/*
 * Resets the core identified by the provided coreId
 */
whd_result_t whd_reset_core(whd_driver_t whd_driver, device_core_t core_id, uint32_t bits, uint32_t resetbits)
{
    uint32_t base = whd_get_core_address(whd_driver, core_id);
    whd_result_t result;
    uint8_t regdata;
    uint32_t loop_counter = 10;
    /* ensure there are no pending backplane operations */
    SPINWAIT( ( ( (result = whd_bus_read_backplane_value(whd_driver, base + AI_RESETSTATUS_OFFSET, (uint8_t)1,
                                                         &regdata) ) == WHD_SUCCESS ) && regdata != 0 ), 300 );

    /* put core into reset state */
    result = whd_bus_write_backplane_value(whd_driver, base + AI_RESETCTRL_OFFSET, (uint8_t)1, ( uint32_t )AIRC_RESET);
    (void)cy_rtos_delay_milliseconds( (uint32_t)10 );   /* Ignore return - nothing can be done if it fails */

    /* ensure there are no pending backplane operations */
    SPINWAIT( ( ( (result = whd_bus_read_backplane_value(whd_driver, base + AI_RESETSTATUS_OFFSET, (uint8_t)1,
                                                         &regdata) ) == WHD_SUCCESS ) && regdata != 0 ), 300 );

    result = whd_bus_write_backplane_value(whd_driver, base + AI_IOCTRL_OFFSET, (uint8_t)1,
                                           (bits | resetbits | SICF_FGC | SICF_CLOCK_EN) );

    /* ensure there are no pending backplane operations */
    SPINWAIT( ( ( (result = whd_bus_read_backplane_value(whd_driver, base + AI_RESETSTATUS_OFFSET, (uint8_t)1,
                                                         &regdata) ) == WHD_SUCCESS ) && regdata != 0 ), 300 );

    while ( ( (result = whd_bus_read_backplane_value(whd_driver, base + AI_RESETCTRL_OFFSET, (uint8_t)1,
                                                     &regdata) ) == WHD_SUCCESS ) && regdata != 0 &&
            --loop_counter != 0 )
    {
        /* ensure there are no pending backplane operations */
        SPINWAIT( ( ( (result = whd_bus_read_backplane_value(whd_driver, base + AI_RESETSTATUS_OFFSET, (uint8_t)1,
                                                             &regdata) ) == WHD_SUCCESS ) && regdata != 0 ), 300 );
        /* take core out of reset */
        result = whd_bus_write_backplane_value(whd_driver, base + AI_RESETCTRL_OFFSET, (uint8_t)1, (uint32_t)0);
        if (result != WHD_SUCCESS)
        {
            WPRINT_WHD_ERROR( ("%s:%d whd_bus_write_backplane_value failed\n", __FUNCTION__, __LINE__) );
            return result;
        }

        /* ensure there are no pending backplane operations */
        SPINWAIT( ( ( (result = whd_bus_read_backplane_value(whd_driver, base + AI_RESETSTATUS_OFFSET, (uint8_t)1,
                                                             &regdata) ) == WHD_SUCCESS ) && regdata != 0 ), 300 );
    }

    result = whd_bus_write_backplane_value(whd_driver, base + AI_IOCTRL_OFFSET, (uint8_t)1, (bits | SICF_CLOCK_EN) );

    (void)cy_rtos_delay_milliseconds( (uint32_t)1 );   /* Ignore return - nothing can be done if it fails */

    if (result != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR( ("%s:%d whd_bus_write_backplane_value failed\n", __FUNCTION__, __LINE__) );
        return result;
    }
    return WHD_SUCCESS;
}

/*
 * Disables the core identified by the provided coreId
 */
whd_result_t whd_disable_device_core(whd_driver_t whd_driver, device_core_t core_id, wlan_core_flag_t core_flag)
{
    uint32_t base = whd_get_core_address(whd_driver, core_id);
    whd_result_t result;
    uint8_t junk;
    uint8_t regdata;

    /* Read the reset control */
    result = whd_bus_read_backplane_value(whd_driver, base + AI_RESETCTRL_OFFSET, (uint8_t)1, &junk);
    if (result != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR( ("%s:%d whd_bus_read_backplane_value failed\n", __FUNCTION__, __LINE__) );
        return result;
    }

    /* Read the reset control and check if it is already in reset */
    result = whd_bus_read_backplane_value(whd_driver, base + AI_RESETCTRL_OFFSET, (uint8_t)1, &regdata);
    if (result != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR( ("%s:%d whd_bus_read_backplane_value failed\n", __FUNCTION__, __LINE__) );
        return result;
    }
    if ( (regdata & AIRC_RESET) != 0 )
    {
        /* Core already in reset */
        return WHD_SUCCESS;
    }

    /* Write 0 to the IO control and read it back */
    result =
        whd_bus_write_backplane_value(whd_driver, base + AI_IOCTRL_OFFSET, (uint8_t)1,
                                      (core_flag == WLAN_CORE_FLAG_CPU_HALT) ? SICF_CPUHALT : 0);
    if (result != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR( ("%s:%d whd_bus_write_backplane_value failed\n", __FUNCTION__, __LINE__) );
        return result;
    }

    result = whd_bus_read_backplane_value(whd_driver, base + AI_IOCTRL_OFFSET, (uint8_t)1, &junk);
    if (result != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR( ("%s:%d whd_bus_read_backplane_value failed\n", __FUNCTION__, __LINE__) );
        return result;
    }

    (void)cy_rtos_delay_milliseconds( (uint32_t)1 );

    result = whd_bus_write_backplane_value(whd_driver, base + AI_RESETCTRL_OFFSET, (uint8_t)1, ( uint32_t )AIRC_RESET);
    if (result != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR( ("%s:%d whd_bus_write_backplane_value failed\n", __FUNCTION__, __LINE__) );
        return result;
    }

    (void)cy_rtos_delay_milliseconds( (uint32_t)1 );

    return result;
}

/*
 * Resets the core identified by the provided coreId
 */
whd_result_t whd_reset_device_core(whd_driver_t whd_driver, device_core_t core_id, wlan_core_flag_t core_flag)
{
    uint32_t base = whd_get_core_address(whd_driver, core_id);
    whd_result_t result;
    uint8_t junk;

    result = whd_disable_device_core(whd_driver, core_id, core_flag);
    if (result != WHD_SUCCESS)
    {
        return result;
    }

    result =
        whd_bus_write_backplane_value(whd_driver, base + AI_IOCTRL_OFFSET, (uint8_t)1,
                                      ( uint32_t )(SICF_FGC | SICF_CLOCK_EN |
                                                   ( (core_flag == WLAN_CORE_FLAG_CPU_HALT) ? SICF_CPUHALT : 0 ) ) );
    if (result != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR( ("%s:%d whd_bus_write_backplane_value failed\n", __FUNCTION__, __LINE__) );
        return result;
    }

    result = whd_bus_read_backplane_value(whd_driver, base + AI_IOCTRL_OFFSET, (uint8_t)1, &junk);
    if (result != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR( ("%s:%d whd_bus_read_backplane_value failed\n", __FUNCTION__, __LINE__) );
        return result;
    }

    result = whd_bus_write_backplane_value(whd_driver, base + AI_RESETCTRL_OFFSET, (uint8_t)1, 0);
    if (result != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR( ("%s:%d whd_bus_write_backplane_value failed\n", __FUNCTION__, __LINE__) );
        return result;
    }

    (void)cy_rtos_delay_milliseconds( (uint32_t)1 );

    result =
        whd_bus_write_backplane_value(whd_driver, base + AI_IOCTRL_OFFSET, (uint8_t)1,
                                      ( uint32_t )(SICF_CLOCK_EN |
                                                   ( (core_flag == WLAN_CORE_FLAG_CPU_HALT) ? SICF_CPUHALT : 0 ) ) );
    if (result != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR( ("%s:%d whd_bus_write_backplane_value failed\n", __FUNCTION__, __LINE__) );
        return result;
    }

    result = whd_bus_read_backplane_value(whd_driver, base + AI_IOCTRL_OFFSET, (uint8_t)1, &junk);
    if (result != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR( ("%s:%d whd_bus_read_backplane_value failed\n", __FUNCTION__, __LINE__) )
        return result;
    }

    (void)cy_rtos_delay_milliseconds( (uint32_t)1 );

    return result;
}

/*
 * Release ARM core to run instructions
 */
whd_result_t whd_wlan_armcore_run(whd_driver_t whd_driver, device_core_t core_id, wlan_core_flag_t core_flag)
{
    uint32_t base = whd_get_core_address(whd_driver, core_id);
    whd_result_t result;
    uint8_t junk;

    /* Only work for WLAN arm core! */
    if (WLAN_ARM_CORE != core_id)
    {
        return WHD_UNSUPPORTED;
    }

    result =
        whd_bus_write_backplane_value(whd_driver, base + AI_IOCTRL_OFFSET, (uint8_t)1,
                                      ( uint32_t )(SICF_FGC | SICF_CLOCK_EN |
                                                   ( (core_flag == WLAN_CORE_FLAG_CPU_HALT) ? SICF_CPUHALT : 0 ) ) );
    if (result != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR( ("%s:%d whd_bus_write_backplane_value failed\n", __FUNCTION__, __LINE__) )
        return result;
    }

    result = whd_bus_read_backplane_value(whd_driver, base + AI_IOCTRL_OFFSET, (uint8_t)1, &junk);
    if (result != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR( ("%s:%d whd_bus_read_backplane_value failed\n", __FUNCTION__, __LINE__) )
        return result;
    }

    result = whd_bus_write_backplane_value(whd_driver, base + AI_RESETCTRL_OFFSET, (uint8_t)1, 0);
    if (result != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR( ("%s:%d whd_bus_write_backplane_value failed\n", __FUNCTION__, __LINE__) )
        return result;
    }

    (void)cy_rtos_delay_milliseconds( (uint32_t)1 );

    result =
        whd_bus_write_backplane_value(whd_driver, base + AI_IOCTRL_OFFSET, (uint8_t)1,
                                      ( uint32_t )(SICF_CLOCK_EN |
                                                   ( (core_flag == WLAN_CORE_FLAG_CPU_HALT) ? SICF_CPUHALT : 0 ) ) );
    if (result != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR( ("%s:%d whd_bus_write_backplane_value failed\n", __FUNCTION__, __LINE__) );
        return result;
    }

    result = whd_bus_read_backplane_value(whd_driver, base + AI_IOCTRL_OFFSET, (uint8_t)1, &junk);
    if (result != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR( ("%s:%d whd_bus_read_backplane_value failed\n", __FUNCTION__, __LINE__) );
        return result;
    }

    (void)cy_rtos_delay_milliseconds( (uint32_t)1 );

    return result;
}

whd_result_t whd_wifi_read_wlan_log_unsafe(whd_driver_t whd_driver, uint32_t wlan_shared_address, char *buffer,
                                           uint32_t buffer_size)
{
    char ch;
    uint32_t n;
    uint32_t index;
    uint32_t address;
    whd_result_t result = WHD_WLAN_ERROR;
    whd_internal_info_t *internal_info = &whd_driver->internal_info;
    wifi_console_t *c = internal_info->c;

    c = &internal_info->console;

    uint32_t shared_addr = 0;

    address = wlan_shared_address;

    /* Once the FW starts executing it will update the Shared region space(4Bytes) - wlan_shared_address with the shared structure address,
    this shared structure address should be the address FW RAM address so we need the boundary check for this */
    while ( (shared_addr == 0) || (shared_addr <= GET_C_VAR(whd_driver, ATCM_RAM_BASE_ADDRESS) ) ||
            (shared_addr >= (GET_C_VAR(whd_driver, ATCM_RAM_BASE_ADDRESS) + GET_C_VAR(whd_driver, CHIP_RAM_SIZE) ) ) )
    {
        result = whd_bus_read_backplane_value(whd_driver, address, 4, (uint8_t *)&shared_addr);
    }

    if (result != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR( ("%s:%d whd_bus_read_backplane_value failed\n", __FUNCTION__, __LINE__) );
        goto done;
    }

#ifndef PROTO_MSGBUF
    result = whd_bus_transfer_backplane_bytes(whd_driver, BUS_READ, shared_addr, sizeof(wlan_shared_t),
                                              (uint8_t *)&internal_info->sh);
#else
    result = whd_bus_mem_bytes(whd_driver, BUS_READ, TRANS_ADDR(
                                   shared_addr), sizeof(internal_info->sh), (uint8_t *)&internal_info->sh);
#endif

    if (result != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR( ("%s:%d whd_bus_transfer_backplane_bytes failed\n", __FUNCTION__, __LINE__) );
        goto done;
    }

    internal_info->sh.flags = dtoh32(internal_info->sh.flags);
    internal_info->sh.trap_addr = dtoh32(internal_info->sh.trap_addr);
    internal_info->sh.assert_exp_addr = dtoh32(internal_info->sh.assert_exp_addr);
    internal_info->sh.assert_file_addr = dtoh32(internal_info->sh.assert_file_addr);
    internal_info->sh.assert_line = dtoh32(internal_info->sh.assert_line);
    internal_info->sh.console_addr = dtoh32(internal_info->sh.console_addr);
    internal_info->sh.msgtrace_addr = dtoh32(internal_info->sh.msgtrace_addr);

#ifndef PROTO_MSGBUF
    if ( (internal_info->sh.flags & WLAN_SHARED_VERSION_MASK) > WLAN_SHARED_VERSION )
    {
        WPRINT_WHD_ERROR( ("Readconsole: WLAN shared version is not valid sh.flags %x\n\r",
                           internal_info->sh.flags) );
        result = WHD_WLAN_INVALID;
        goto done;
    }
#else
    if ( (internal_info->sh.flags & WLAN_M2M_SHARED_VERSION_MASK) > WLAN_M2M_SHARED_VERSION )
    {
        WPRINT_WHD_ERROR( ("ReadShared: WLAN shared version is not valid sh.flags %x\n\r", internal_info->sh.flags) );
        result = WHD_WLAN_INVALID;
        goto done;
    }
#endif

    internal_info->console_addr = internal_info->sh.console_addr;

    /* Read console log struct */
    address = internal_info->console_addr + offsetof(hnd_cons_t, log);

#ifndef PROTO_MSGBUF
    result = whd_bus_transfer_backplane_bytes(whd_driver, BUS_READ, address, sizeof(c->log), (uint8_t *)&c->log);
#else
    result = whd_bus_mem_bytes(whd_driver, BUS_READ, TRANS_ADDR(address), sizeof(c->log), (uint8_t *)&c->log);
#endif

    if (result != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR( ("%s:%d whd_bus_transfer_backplane_bytes failed\n", __FUNCTION__, __LINE__) );
        goto done;
    }

    /* Allocate console buffer (one time only) */
    if (c->buf == NULL)
    {
        c->bufsize = dtoh32(c->log.buf_size);
        c->buf = whd_mem_malloc(c->bufsize);
        if (c->buf == NULL)
        {
            WPRINT_WHD_ERROR( ("%s:%d c->buf IS null \n", __FUNCTION__, __LINE__) );
            result = WHD_WLAN_NOMEM;
            goto done;
        }
    }

    /* Retrieve last read position */
    c->last = whd_driver->internal_info.con_lastpos;

    index = dtoh32(c->log.idx);

    /* Protect against corrupt value */
    if (index > c->bufsize)
    {
        WPRINT_WHD_ERROR( ("%s:%d index > c->bufsize \n", __FUNCTION__, __LINE__) );
        result = WHD_WLAN_BUFTOOSHORT;
        goto done;
    }

    /* Skip reading the console buffer if the index pointer has not moved */
    if (index == c->last)
    {
        result = WHD_SUCCESS;
        goto done;
    }

    /* Read the console buffer */
    /* xxx this could optimize and read only the portion of the buffer needed, but
     * it would also have to handle wrap-around.
     */
    address = dtoh32(c->log.buf);

#ifndef PROTO_MSGBUF
    result = whd_bus_transfer_backplane_bytes(whd_driver, BUS_READ, address, c->bufsize, (uint8_t *)c->buf);
#else
    result = whd_bus_mem_bytes(whd_driver, BUS_READ, TRANS_ADDR(address), c->bufsize, (uint8_t *)c->buf);
#endif

    if (result != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR( ("%s:%d whd_bus_transfer_backplane_bytes failed\n", __FUNCTION__, __LINE__) );
        goto done;
    }

    while (c->last != index)
    {
        for (n = 0; n < buffer_size - 2; n++)
        {
            if (c->last == index)
            {
                /* This would output a partial line.  Instead, back up
                 * the buffer pointer and output this line next time around.
                 */
                if (c->last >= n)
                {
                    c->last -= n;
                }
                else
                {
                    c->last = c->bufsize - n;
                }
                /* Save last read position */
                whd_driver->internal_info.con_lastpos = c->last;

                result = WHD_SUCCESS;
                goto done;
            }
            ch = c->buf[c->last];
            c->last = (c->last + 1) % c->bufsize;
            if (ch == '\n')
            {
                break;
            }
            buffer[n] = ch;
        }
        if (n > 0)
        {
            if (buffer[n - 1] == '\r')
                n--;
            buffer[n] = 0;
#ifndef PROTO_MSGBUF
            WPRINT_MACRO( ("CONSOLE: %s\n", buffer) );
#else
	    /* printf is used here because WPRINT_MACRO cannot be enabled in H1-CP because of memory constraint
               This change has to be relooked again*/
            printf("CONSOLE: %s\n", buffer);
#endif
        }
    }
    /* Save last read position */
    whd_driver->internal_info.con_lastpos = c->last;
    result = WHD_SUCCESS;

done: return result;
}

void whd_wifi_peek(whd_driver_t whd_driver, uint32_t address, uint8_t register_length, uint8_t *value)
{
    uint8_t status;

    WHD_WLAN_KEEP_AWAKE(whd_driver);

    status = whd_bus_read_backplane_value(whd_driver, address, register_length, value);

    if (status != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR( ("%s: Error reading interrupt status\n", __FUNCTION__) );
    }

    WHD_WLAN_LET_SLEEP(whd_driver);
}

void whd_wifi_poke(whd_driver_t whd_driver, uint32_t address, uint8_t register_length, uint32_t value)
{
    uint8_t status;

    WHD_WLAN_KEEP_AWAKE(whd_driver);

    status = whd_bus_write_backplane_value(whd_driver, address, register_length, value);

    if (status != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR( ("%s: Error clearing the interrupt status\n", __FUNCTION__) );
    }

    WHD_WLAN_LET_SLEEP(whd_driver);
}

#ifdef WHD_IOCTL_LOG_ENABLE
whd_result_t whd_ioctl_log_add(whd_driver_t whd_driver, uint32_t cmd, whd_buffer_t buffer)
{
    uint8_t *data = NULL;
    size_t data_size = whd_buffer_get_current_piece_size(whd_driver, buffer);

    data = whd_buffer_get_current_piece_data_pointer(whd_driver, buffer);
    CHECK_IOCTL_BUFFER(data);
    CHECK_RETURN(cy_rtos_get_semaphore(&whd_driver->whd_log_mutex, CY_RTOS_NEVER_TIMEOUT, WHD_FALSE) );
    data = data + IOCTL_OFFSET;
    data_size = data_size - IOCTL_OFFSET;
    whd_driver->whd_ioctl_log[whd_driver->whd_ioctl_log_index % WHD_IOCTL_LOG_SIZE].ioct_log = cmd;
    whd_driver->whd_ioctl_log[whd_driver->whd_ioctl_log_index % WHD_IOCTL_LOG_SIZE].is_this_event = 0;
    whd_driver->whd_ioctl_log[whd_driver->whd_ioctl_log_index % WHD_IOCTL_LOG_SIZE].data_size = MIN_OF(
        WHD_MAX_DATA_SIZE, data_size);
    whd_mem_memset(whd_driver->whd_ioctl_log[whd_driver->whd_ioctl_log_index % WHD_IOCTL_LOG_SIZE].data, 0,
           WHD_MAX_DATA_SIZE);
    whd_mem_memcpy(whd_driver->whd_ioctl_log[whd_driver->whd_ioctl_log_index % WHD_IOCTL_LOG_SIZE].data, data,
           whd_driver->whd_ioctl_log[whd_driver->whd_ioctl_log_index % WHD_IOCTL_LOG_SIZE].data_size);

    whd_driver->whd_ioctl_log_index++;
    CHECK_RETURN(cy_rtos_set_semaphore(&whd_driver->whd_log_mutex, WHD_FALSE) );
    return WHD_SUCCESS;
}

whd_result_t whd_ioctl_log_add_event(whd_driver_t whd_driver, uint32_t cmd, uint16_t flag, uint32_t reason)
{
    CHECK_RETURN(cy_rtos_get_semaphore(&whd_driver->whd_log_mutex, CY_RTOS_NEVER_TIMEOUT, WHD_FALSE) );
    whd_driver->whd_ioctl_log[whd_driver->whd_ioctl_log_index % WHD_IOCTL_LOG_SIZE].is_this_event = 1;
    whd_driver->whd_ioctl_log[whd_driver->whd_ioctl_log_index % WHD_IOCTL_LOG_SIZE].ioct_log = cmd;
    whd_driver->whd_ioctl_log[whd_driver->whd_ioctl_log_index % WHD_IOCTL_LOG_SIZE].flag = flag;
    whd_driver->whd_ioctl_log[whd_driver->whd_ioctl_log_index % WHD_IOCTL_LOG_SIZE].reason = reason;

    whd_driver->whd_ioctl_log_index++;
    CHECK_RETURN(cy_rtos_set_semaphore(&whd_driver->whd_log_mutex, WHD_FALSE) );
    return WHD_SUCCESS;
}

whd_result_t whd_ioctl_print(whd_driver_t whd_driver)
{
    int i;
    uint8_t *data = NULL;
    size_t iovar_string_size = 0;

    CHECK_RETURN(cy_rtos_get_semaphore(&whd_driver->whd_log_mutex, CY_RTOS_NEVER_TIMEOUT, WHD_FALSE) );
    for (i = 0; i < WHD_IOCTL_LOG_SIZE; i++)
    {
        char iovar[WHD_IOVAR_STRING_SIZE] = {0};
        data = whd_driver->whd_ioctl_log[i].data;

        if ( (whd_driver->whd_ioctl_log[i].ioct_log == WLC_SET_VAR) ||
             (whd_driver->whd_ioctl_log[i].ioct_log == WLC_GET_VAR) )
        {
            /* refer to whd_cdc_get_iovar_buffer() */
            while (!*data)
            {
                whd_driver->whd_ioctl_log[i].data_size--;
                data++;
            }

            if (strlen( (char *)data ) <= WHD_IOVAR_STRING_SIZE)
                strncpy(iovar, (char *)data, strlen( (char *)data ) );

            iovar_string_size = strlen( (const char *)data );
            iovar[iovar_string_size] = '\0';
            data += (iovar_string_size + 1);
            whd_driver->whd_ioctl_log[i].data_size -= (iovar_string_size + 1);
        }
        if (whd_driver->whd_ioctl_log[i].is_this_event == 1)
        {
            whd_event_info_to_string(whd_driver->whd_ioctl_log[i].ioct_log, whd_driver->whd_ioctl_log[i].flag,
                                     whd_driver->whd_ioctl_log[i].reason, iovar, sizeof(iovar) - 1);
            WPRINT_MACRO( ("\n<- E:%" PRIu32 "\t\t\tS:%d\t\t\t\tR:%" PRIu32 "\n%s\n",
                           whd_driver->whd_ioctl_log[i].ioct_log,
                           whd_driver->whd_ioctl_log[i].flag, whd_driver->whd_ioctl_log[i].reason, iovar) );
        }
        else if (whd_driver->whd_ioctl_log[i].ioct_log == WLC_SET_VAR)
        {
            WPRINT_MACRO( ("\n-> %s\n", iovar) );
            whd_hexdump(data, whd_driver->whd_ioctl_log[i].data_size);
        }
        else if (whd_driver->whd_ioctl_log[i].ioct_log == WLC_GET_VAR)
        {
            WPRINT_MACRO( ("\n<- %s\n", iovar) );
            whd_hexdump(data, whd_driver->whd_ioctl_log[i].data_size);
        }
        else if (whd_driver->whd_ioctl_log[i].ioct_log != 0)
        {
            whd_ioctl_info_to_string(whd_driver->whd_ioctl_log[i].ioct_log, iovar, sizeof(iovar) - 1);
            WPRINT_MACRO( ("\n%s:%" PRIu32 "\n", iovar, whd_driver->whd_ioctl_log[i].ioct_log) );
            whd_hexdump(data, whd_driver->whd_ioctl_log[i].data_size);
        }
    }

    whd_mem_memset(whd_driver->whd_ioctl_log, 0, sizeof(whd_driver->whd_ioctl_log) );
    whd_driver->whd_ioctl_log_index = 0;
    CHECK_RETURN(cy_rtos_set_semaphore(&whd_driver->whd_log_mutex, WHD_FALSE) );
    return WHD_SUCCESS;
}
#endif

void whd_wifi_chip_info_init(whd_driver_t whd_driver)
{
    whd_driver->chip_info.save_restore_enable = WHD_FALSE;
}

whd_result_t whd_wifi_set_custom_country_code(whd_interface_t ifp, const whd_country_info_t *country_code)
{
    whd_driver_t whd_driver = ifp->whd_driver;

    /* Get chip number */
    uint16_t wlan_chip_id = whd_chip_get_chip_id(whd_driver);
    if (wlan_chip_id == 43362)
    {
        whd_buffer_t buffer;
        whd_result_t result;
        whd_country_info_t *data;
        data = (whd_country_info_t *)whd_proto_get_ioctl_buffer(whd_driver, &buffer,
                                                                (uint16_t)sizeof(whd_country_info_t) + 10);
        if (data == NULL)
        {
            whd_assert("Could not get buffer for IOCTL", 0 != 0);
            return WHD_BUFFER_ALLOC_FAIL;
        }
        whd_mem_memcpy(data, country_code, sizeof(whd_country_info_t) );
        result = whd_proto_set_ioctl(ifp, WLC_SET_CUSTOM_COUNTRY, buffer, NULL);
        return result;
    }
    else
    {
        UNUSED_PARAMETER(country_code);
        return WHD_UNSUPPORTED;
    }
}

whd_result_t whd_chip_specific_socsram_init(whd_driver_t whd_driver)
{
    /* Get chip number */
    uint16_t wlan_chip_id = whd_chip_get_chip_id(whd_driver);
    if ( (wlan_chip_id == 43430) || (wlan_chip_id == 43439) )
    {
        CHECK_RETURN(whd_bus_write_backplane_value(whd_driver, SOCSRAM_BANKX_INDEX(whd_driver), 4, 0x3) );
        CHECK_RETURN(whd_bus_write_backplane_value(whd_driver, SOCSRAM_BANKX_PDA(whd_driver), 4, 0) );
        return WHD_SUCCESS;
    }
    else
    {
        return WHD_SUCCESS;
    }
}

whd_result_t whd_chip_specific_init(whd_driver_t whd_driver)
{
    /* Get chip number */
    uint16_t wlan_chip_id = whd_chip_get_chip_id(whd_driver);
    if ( (wlan_chip_id == 4334) || (wlan_chip_id == 43362) )
    {
        return WHD_SUCCESS;
    }
    else
    {
        return whd_enable_save_restore(whd_driver);
    }
}

whd_result_t whd_allow_wlan_bus_to_sleep(whd_driver_t whd_driver)
{
    /* Get chip number */
    uint16_t wlan_chip_id = whd_chip_get_chip_id(whd_driver);
    whd_bt_dev_t btdev = whd_driver->bt_dev;
    if ( (wlan_chip_id == 4334) || (wlan_chip_id == 43362) )
    {
        /* Clear HT clock request */
        if (whd_bus_is_up(whd_driver) == WHD_TRUE)
        {
            whd_bus_set_state(whd_driver, WHD_FALSE);
            CHECK_RETURN(whd_bus_write_register_value(whd_driver, BACKPLANE_FUNCTION, (uint32_t)SDIO_CHIP_CLOCK_CSR,
                                                      (uint8_t)1, 0) );
            /* Bus specific sleep routine */
            return whd_bus_sleep(whd_driver);
        }
        else
        {
            return WHD_SUCCESS;
        }
    }
    else
    {
        /* Clear HT clock request */
        if (whd_bus_is_up(whd_driver) == WHD_TRUE)
        {
            whd_bus_set_state(whd_driver, WHD_FALSE);
            if (whd_driver->chip_info.save_restore_enable == WHD_FALSE)
            {
                return whd_bus_write_register_value(whd_driver, BACKPLANE_FUNCTION, (uint32_t)SDIO_CHIP_CLOCK_CSR,
                                                    (uint8_t)1, 0);
            }
            else
            {
                if (btdev && btdev->bt_int_cb)
                {
                    return WHD_SUCCESS;
                }
                return whd_kso_enable(whd_driver, WHD_FALSE);
            }
        }
        else
        {
            return WHD_SUCCESS;
        }
    }
}

whd_result_t whd_wifi_read_wlan_log(whd_driver_t whd_driver, char *buffer, uint32_t buffer_size)
{
    whd_result_t result;
    uint32_t wlan_shared_address;
    uint16_t wlan_chip_id = 0;

    CHECK_DRIVER_NULL(whd_driver);

    WHD_IOCTL_PRINT(whd_driver);

    wlan_chip_id = whd_chip_get_chip_id(whd_driver);
    if (wlan_chip_id == 43362)
    {
        return whd_wifi_read_wlan_log_unsafe(whd_driver, ( (GET_C_VAR(whd_driver, CHIP_RAM_SIZE) +
                                                            PLATFORM_WLAN_RAM_BASE) - 4 ), buffer, buffer_size);
    }
    else if ( (wlan_chip_id == 43909) || (wlan_chip_id == 43907) || (wlan_chip_id == 54907) )
    {
        result = whd_ensure_wlan_bus_is_up(whd_driver);
        if (result != WHD_SUCCESS)
        {
            return result;
        }
        result = whd_wifi_read_wlan_log_unsafe(whd_driver, ( (GET_C_VAR(whd_driver, CHIP_RAM_SIZE) +
                                                              GET_C_VAR(whd_driver,
                                                                        ATCM_RAM_BASE_ADDRESS) ) - 4 ), buffer,
                                               buffer_size);
        whd_thread_notify(whd_driver);
        return result;
    }
    else if ( (wlan_chip_id == 4334) || (wlan_chip_id == 4390) )
    {
        return WHD_UNSUPPORTED;
    }
    else
    {
        /* Backplane access needs HT clock. So, disabling bus sleep */
        WHD_WLAN_KEEP_AWAKE(whd_driver);
#ifndef DM_43022C1
        /* FW populates the last word of RAM with wlan_shared_t struct address */
        wlan_shared_address = PLATFORM_WLAN_RAM_BASE + GET_C_VAR(whd_driver, ATCM_RAM_BASE_ADDRESS) +
                              GET_C_VAR(whd_driver, CHIP_RAM_SIZE) - 4;
#else
        /* FW populates the last word of RAM with wlan_shared_t struct address */
        wlan_shared_address = (PLATFORM_WLAN_RAM_BASE + GET_C_VAR(whd_driver, ATCM_RAM_BASE_ADDRESS) +
                              GET_C_VAR(whd_driver, NVRAM_DNLD_ADDR) - 4);
#endif

        if (!GET_C_VAR(whd_driver, ATCM_RAM_BASE_ADDRESS) && (whd_is_fw_sr_capable(whd_driver) == WHD_TRUE) )
        {
            wlan_shared_address -= GET_C_VAR(whd_driver, SOCRAM_SRMEM_SIZE);
        }
        result = whd_wifi_read_wlan_log_unsafe(whd_driver, wlan_shared_address, buffer, buffer_size);
        WHD_WLAN_LET_SLEEP(whd_driver);
        return result;
    }

}

whd_result_t whd_wifi_print_whd_log(whd_driver_t whd_driver)
{
    whd_result_t result;
    char *buffer = NULL;

    CHECK_DRIVER_NULL(whd_driver);

    WHD_IOCTL_PRINT(whd_driver);

    if ( (buffer = whd_mem_malloc(WLAN_LOG_BUF_LEN) ) == NULL )
    {
        WPRINT_WHD_ERROR( ("Memory allocation failed for log buffer in %s \n", __FUNCTION__) );
        return WHD_MALLOC_FAILURE;
    }

    result = whd_wifi_read_wlan_log(whd_driver, buffer, WLAN_LOG_BUF_LEN);
    if (result == WHD_SUCCESS)
    {
        whd_print_logbuffer();  // This is not supported yet.
    }
    whd_mem_free(buffer);
    CHECK_RETURN(result);
    return result;
}

uint32_t whd_wifi_read_tcm_byte(whd_driver_t whd_driver, uint32_t offset)
{
    uint32_t result;
    uint32_t atcm_base_address = GET_C_VAR(whd_driver, ATCM_RAM_BASE_ADDRESS);
    uint8_t wifi_tcm_byte;

    result = whd_bus_read_backplane_value(whd_driver, (atcm_base_address + offset), 1, (uint8_t *)&wifi_tcm_byte);
    if (result != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR( ("Failed to read address @ %lx\n", (unsigned long)(atcm_base_address + offset)) );
        return result;
    }

    return wifi_tcm_byte;
}

whd_result_t whd_wifi_read_fw_capabilities(whd_interface_t ifp)
{
    whd_result_t result;
    char caps[MAX_CAPS_BUFFER_SIZE];
    whd_fwcap_id_t id;

    CHECK_IFP_NULL(ifp);
    whd_driver_t whd_driver = ifp->whd_driver;

    result = whd_wifi_get_iovar_buffer(ifp, IOVAR_STR_CAP, (uint8_t *)caps, sizeof(caps) );
    CHECK_RETURN(result);

    for (uint32_t i = 0; i < ARRAY_SIZE(whd_fwcap_map); i++)
    {
        if (strstr(caps, whd_fwcap_map[i].fwcap_name) )
        {
            id = whd_fwcap_map[i].feature;
            WPRINT_WHD_DEBUG( ("Enabling FW Capabilities: %s\n", whd_fwcap_map[i].fwcap_name) );
            whd_driver->chip_info.fwcap_flags |= (1 << id);
        }
    }
    return WHD_SUCCESS;
}

whd_result_t whd_ensure_wlan_bus_is_up(whd_driver_t whd_driver)
{
    uint8_t csr = 0;
    uint32_t attempts = ( uint32_t )WLAN_BUS_UP_ATTEMPTS;
    uint16_t wlan_chip_id = whd_chip_get_chip_id(whd_driver);

    /* Ensure HT clock is up */
    if (whd_bus_is_up(whd_driver) == WHD_TRUE)
    {
        return WHD_SUCCESS;
    }

    if ( (wlan_chip_id == 43362) || (wlan_chip_id == 4334) )
    {
        /* Bus specific wakeup routine */
        CHECK_RETURN(whd_bus_wakeup(whd_driver) );
        CHECK_RETURN(whd_bus_write_register_value(whd_driver, BACKPLANE_FUNCTION, (uint32_t)SDIO_CHIP_CLOCK_CSR,
                                                  (uint8_t)1, (uint32_t)SBSDIO_HT_AVAIL_REQ) );
        do
        {
            CHECK_RETURN(whd_bus_read_register_value(whd_driver, BACKPLANE_FUNCTION, (uint32_t)SDIO_CHIP_CLOCK_CSR,
                                                     (uint8_t)1, &csr) );
            --attempts;
        } while ( ( (csr & SBSDIO_HT_AVAIL) == 0 ) && (attempts != 0) &&
                  (cy_rtos_delay_milliseconds( (uint32_t)1 ), 1 == 1) );

        if (attempts == 0)
        {
            WPRINT_WHD_ERROR( ("SDIO bus failed to come up , %s failed at %d \n", __func__, __LINE__) );
            return WHD_BUS_UP_FAIL;
        }
        else
        {
            whd_bus_set_state(whd_driver, WHD_TRUE);
            return WHD_SUCCESS;
        }
    }
    else if ( (wlan_chip_id == 43909) || (wlan_chip_id == 43907) || (wlan_chip_id == 54907) )
    {
        //To-Do
        /* M2M power save mode */
        //M2M_POWERSAVE_COMM_TX_BEGIN
        return WHD_SUCCESS;
    }
#ifdef PROTO_MSGBUF
    else if (wlan_chip_id == 55900)
    {
        if (whd_bus_resume(whd_driver) == WHD_SUCCESS)
        {
            whd_bus_set_state(whd_driver, WHD_TRUE);
            return WHD_SUCCESS;
        }
        else
        {
            WPRINT_WHD_ERROR( ("Bus failed to come up , %s failed at %d \n", __func__, __LINE__) );
            return WHD_SDIO_BUS_UP_FAIL;
        }
    }
#endif
    else
    {
        if (whd_driver->chip_info.save_restore_enable == WHD_FALSE)
        {
            CHECK_RETURN(whd_bus_write_register_value(whd_driver, BACKPLANE_FUNCTION, (uint32_t)SDIO_CHIP_CLOCK_CSR,
                                                      (uint8_t)1, (uint32_t)SBSDIO_HT_AVAIL_REQ) );
            do
            {
                CHECK_RETURN(whd_bus_read_register_value(whd_driver, BACKPLANE_FUNCTION, (uint32_t)SDIO_CHIP_CLOCK_CSR,
                                                         (uint8_t)sizeof(csr), &csr) );
                --attempts;
            } while ( ( (csr & SBSDIO_HT_AVAIL) == 0 ) && (attempts != 0) &&
                      (cy_rtos_delay_milliseconds( ( uint32_t )HT_AVAIL_WAIT_MS ), 1 == 1) );

            if (attempts == 0)
            {
                WPRINT_WHD_ERROR( ("SDIO bus failed to come up , %s failed at %d \n", __func__, __LINE__) );
                return WHD_SDIO_BUS_UP_FAIL;
            }
            else
            {
                whd_bus_set_state(whd_driver, WHD_TRUE);
                return WHD_SUCCESS;
            }
        }
        else
        {
            if (whd_kso_enable(whd_driver, WHD_TRUE) == WHD_SUCCESS)
            {
                whd_bus_set_state(whd_driver, WHD_TRUE);
                return WHD_SUCCESS;
            }
            else
            {
                WPRINT_WHD_ERROR( ("SDIO bus failed to come up , %s failed at %d \n", __func__, __LINE__) );
                return WHD_SDIO_BUS_UP_FAIL;
            }
        }
    }
}

static whd_bool_t whd_is_fw_sr_capable(whd_driver_t whd_driver)
{
    uint32_t core_capext;
    uint32_t retention_ctl = 0;
    uint32_t srctrl = 0;
    whd_bool_t save_restore_capable = WHD_FALSE;
    whd_result_t result = WHD_SUCCESS;

    /* Get chip number */
    uint16_t wlan_chip_id = whd_chip_get_chip_id(whd_driver);

    if ( (wlan_chip_id == 43430) || (wlan_chip_id == 43439) )
    {
        /* check if fw initialized sr engine */
        if (whd_bus_read_backplane_value(whd_driver, (uint32_t)CHIPCOMMON_SR_CONTROL1, (uint8_t)4,
                                         (uint8_t *)&srctrl) != WHD_SUCCESS)
        {
            return WHD_FALSE;
        }
        if (srctrl != 0)
        {
            return WHD_TRUE;
        }
        else
        {
            return WHD_FALSE;
        }
    }
    else if ( (wlan_chip_id == 43340) || (wlan_chip_id == 43342) )
    {
        /* check if fw initialized sr engine */
        result = whd_bus_read_backplane_value(whd_driver, (uint32_t)CHIPCOMMON_CORE_CAPEXT_ADDR, (uint8_t)4,
                                              (uint8_t *)&core_capext);
        if (result != WHD_SUCCESS)
        {
            return WHD_FALSE;
        }

        if ( (core_capext & CHIPCOMMON_CORE_CAPEXT_SR_SUPPORTED) != 0 )
        {
            result = whd_bus_read_backplane_value(whd_driver, (uint32_t)CHIPCOMMON_CORE_RETENTION_CTL, (uint8_t)4,
                                                  (uint8_t *)&retention_ctl);
            if (result != WHD_SUCCESS)
            {
                return WHD_FALSE;
            }
            if ( (retention_ctl & (CHIPCOMMON_CORE_RCTL_MACPHY_DISABLE | CHIPCOMMON_CORE_RCTL_LOGIC_DISABLE) ) == 0 )
            {
                save_restore_capable = WHD_TRUE;
            }
        }
        return save_restore_capable;
    }
    else if (wlan_chip_id == 55560 || wlan_chip_id == 43022 || wlan_chip_id == 55500 || wlan_chip_id == 55530)
    {
        /*In hatchet Chips(DM) and 43022DM, the SR(SaveRestore) is always enabled. In DM mode(secure), host not
           able to access PMU register(SR). So making save_restore_capable as default for Hatchet and 43022DM Chips */
        save_restore_capable = WHD_TRUE;
        return save_restore_capable;
    }
    else
    {
        /* check if fw initialized sr engine */
        result =
            whd_bus_read_backplane_value(whd_driver, (uint32_t)RETENTION_CTL(
                                             whd_driver), (uint8_t)sizeof(retention_ctl),
                                         (uint8_t *)&retention_ctl);
        if (result != WHD_SUCCESS)
        {
            return WHD_FALSE;
        }
        if ( (retention_ctl & (RCTL_MACPHY_DISABLE | RCTL_LOGIC_DISABLE) ) == 0 )
        {
            save_restore_capable = WHD_TRUE;
        }
        return save_restore_capable;
    }
}

static whd_result_t whd_enable_save_restore(whd_driver_t whd_driver)
{
    uint8_t data;
    /* Get chip number */
    uint16_t wlan_chip_id = whd_chip_get_chip_id(whd_driver);

    if (whd_is_fw_sr_capable(whd_driver) == WHD_TRUE)
    {
        if ( (wlan_chip_id == 43012) || (wlan_chip_id == 0x4373) || (wlan_chip_id == 55560) || (wlan_chip_id == 55500) || (wlan_chip_id == 55530) || (wlan_chip_id == 43022) )
        {
            /* Configure WakeupCtrl register to set AlpAvail request bit in chipClockCSR register
             * after the sdiod core is powered on.
             */
            CHECK_RETURN(whd_bus_read_register_value(whd_driver, BACKPLANE_FUNCTION, (uint32_t)SDIO_WAKEUP_CTRL,
                                                     (uint8_t)sizeof(data), &data) );

#ifdef CP_OVER_SDIO
            data |= SBSDIO_WCTRL_BT_WAKE_TILL_ALP_AVAIL;
            CHECK_RETURN(whd_bus_write_register_value(whd_driver, BACKPLANE_FUNCTION, (uint32_t)SDIO_WAKEUP_CTRL,
                                                      (uint8_t)sizeof(data), data) );
#else
            data |= SBSDIO_WCTRL_WL_WAKE_TILL_ALP_AVAIL;
            CHECK_RETURN(whd_bus_write_register_value(whd_driver, BACKPLANE_FUNCTION, (uint32_t)SDIO_WAKEUP_CTRL,
                                                      (uint8_t)sizeof(data), data) );
#endif

        }
        else
        {
            /* Configure WakeupCtrl register to set HtAvail request bit in chipClockCSR register
             * after the sdiod core is powered on.
             */
            CHECK_RETURN(whd_bus_read_register_value(whd_driver, BACKPLANE_FUNCTION, (uint32_t)SDIO_WAKEUP_CTRL,
                                                     (uint8_t)1, &data) );
            data |= SBSDIO_WCTRL_WL_WAKE_TILL_HT_AVAIL;
            CHECK_RETURN(whd_bus_write_register_value(whd_driver, BACKPLANE_FUNCTION, (uint32_t)SDIO_WAKEUP_CTRL,
                                                      (uint8_t)1, data) );
        }

        /* Set brcmCardCapability to noCmdDecode mode.
         * It makes sdiod_aos to wakeup host for any activity of cmd line, even though
         * module won't decode cmd or respond
         */
        CHECK_RETURN(whd_bus_write_register_value(whd_driver, BUS_FUNCTION, (uint32_t)SDIOD_CCCR_BRCM_CARDCAP,
                                                  (uint8_t)1, SDIOD_CCCR_BRCM_CARDCAP_CMD_NODEC) );
        if ( (wlan_chip_id == 43012) || (wlan_chip_id == 0x4373) || (wlan_chip_id == 55560) || (wlan_chip_id == 55500) || (wlan_chip_id == 55530) || (wlan_chip_id == 43022) )
        {
            CHECK_RETURN(whd_bus_write_register_value(whd_driver, BACKPLANE_FUNCTION, (uint32_t)SDIO_CHIP_CLOCK_CSR,
                                                      (uint8_t)1, SBSDIO_HT_AVAIL_REQ) );
        }
        else
        {
            CHECK_RETURN(whd_bus_write_register_value(whd_driver, BACKPLANE_FUNCTION, (uint32_t)SDIO_CHIP_CLOCK_CSR,
                                                      (uint8_t)1, (uint32_t)SBSDIO_FORCE_HT) );
        }

        /* Enable KeepSdioOn (KSO) bit for normal operation */
        if ( (wlan_chip_id == 43012) || (wlan_chip_id == 0x4373) || (wlan_chip_id == 55560) || (wlan_chip_id == 55500) || (wlan_chip_id == 55530) || (wlan_chip_id == 43022) )
        {
            CHECK_RETURN(whd_bus_read_register_value(whd_driver, BACKPLANE_FUNCTION, (uint32_t)SDIO_SLEEP_CSR,
                                                     (uint8_t)sizeof(data), &data) );
        }
        else
        {
            CHECK_RETURN(whd_bus_read_register_value(whd_driver, BACKPLANE_FUNCTION, (uint32_t)SDIO_SLEEP_CSR,
                                                     (uint8_t)1, &data) );
        }
        if ( (data & SBSDIO_SLPCSR_KEEP_WL_KSO) == 0 )
        {
            data |= SBSDIO_SLPCSR_KEEP_WL_KSO;
            if ( (wlan_chip_id == 43012) || (wlan_chip_id == 0x4373) || (wlan_chip_id == 55560) || (wlan_chip_id == 55500) || (wlan_chip_id == 55530) || (wlan_chip_id == 43022) )
            {
                CHECK_RETURN(whd_bus_write_register_value(whd_driver, BACKPLANE_FUNCTION, (uint32_t)SDIO_SLEEP_CSR,
                                                          (uint8_t)sizeof(data), data) );
            }
            else
            {
                CHECK_RETURN(whd_bus_write_register_value(whd_driver, BACKPLANE_FUNCTION, (uint32_t)SDIO_SLEEP_CSR,
                                                          (uint8_t)1, data) );
            }
        }

        /* SPI bus can be configured for sleep by default.
         * KSO bit solely controls the wlan chip sleep
         */
        CHECK_RETURN(whd_bus_sleep(whd_driver) );

        /* Put SPI interface block to sleep */
        CHECK_RETURN(whd_bus_write_register_value(whd_driver, BACKPLANE_FUNCTION, SDIO_PULL_UP, (uint8_t)1, 0xf) );

        whd_driver->chip_info.save_restore_enable = WHD_TRUE;
    }
    else
    {
        whd_driver->chip_info.save_restore_enable = WHD_FALSE;
    }

    return WHD_SUCCESS;
}

static whd_result_t whd_kso_enable(whd_driver_t whd_driver, whd_bool_t enable)
{
    uint8_t write_value = 0;
    uint8_t read_value = 0;
    uint8_t compare_value;
    uint8_t bmask;
    uint32_t attempts = ( uint32_t )MAX_KSO_ATTEMPTS;
    whd_result_t result;
    /* Get chip number */

    uint16_t wlan_chip_id = whd_chip_get_chip_id(whd_driver);
    if (enable == WHD_TRUE)
    {
#ifdef CP_OVER_SDIO
        if (wlan_chip_id == 55500 || wlan_chip_id == 55530)
        {
            write_value |= SBSDIO_SLPCSR_KEEP_BT_KSO;
        }
#else
        write_value |= SBSDIO_SLPCSR_KEEP_WL_KSO;
#endif
    }

    /* 1st KSO write goes to AOS wake up core if device is asleep  */
    /* Possibly device might not respond to this cmd. So, don't check return value here */
    if ( (wlan_chip_id == 43430) || (wlan_chip_id == 43439) || (wlan_chip_id == 43340) || (wlan_chip_id == 43342) )
    {
        /* 2 Sequential writes to KSO bit are required for SR module to wakeup, both write can fail */
        CHECK_RETURN_IGNORE(whd_bus_write_register_value(whd_driver, BACKPLANE_FUNCTION, (uint32_t)SDIO_SLEEP_CSR,
                                                         (uint8_t)1,
                                                         write_value) );
        CHECK_RETURN_IGNORE(whd_bus_write_register_value(whd_driver, BACKPLANE_FUNCTION, (uint32_t)SDIO_SLEEP_CSR,
                                                         (uint8_t)1,
                                                         write_value) );
        if (enable == WHD_TRUE)
        {
            /* device WAKEUP through KSO:
             * write bit 0 & read back until
             * both bits 0(kso bit) & 1 (dev on status) are set
             */
            compare_value = SBSDIO_SLPCSR_KEEP_WL_KSO | SBSDIO_SLPCSR_WL_DEVON;
            bmask = compare_value;
        }
        else
        {
            /* Put device to sleep, turn off  KSO  */
            compare_value = 0;
            /* Check for bit0 only, bit1(devon status) may not get cleared right away */
            bmask = SBSDIO_SLPCSR_KEEP_WL_KSO;
        }
    }
    else
    {
        CHECK_RETURN_IGNORE(whd_bus_write_register_value(whd_driver, BACKPLANE_FUNCTION, (uint32_t)SDIO_SLEEP_CSR,
                                                         (uint8_t)1, write_value) );

        /* In case of 43012/555x0 chip, the chip could go down immediately after KSO bit is cleared.
         * So the further reads of KSO register could fail. Thereby just bailing out immediately
         * after clearing KSO bit, to avoid polling of KSO bit.
         */
        if (enable == WHD_FALSE)
        {
            return WHD_SUCCESS;
        }

        /* 2 Sequential writes to KSO bit are required for SR module to wakeup */
        CHECK_RETURN_IGNORE(whd_bus_write_register_value(whd_driver, BACKPLANE_FUNCTION, (uint32_t)SDIO_SLEEP_CSR,
                                                         (uint8_t)1, write_value) );

        /* device WAKEUP through KSO:
         * write bit 0 & read back until
         * both bits 0(kso bit) & 1 (dev on status) are set,
         * For CP , enable BT KSO bits
         */
#ifdef CP_OVER_SDIO
        if (wlan_chip_id == 55500 || wlan_chip_id == 55530 )
        {
            compare_value = SBSDIO_SLPCSR_KEEP_BT_KSO | SBSDIO_SLPCSR_BT_DEVON;
        }
#else
        compare_value = SBSDIO_SLPCSR_KEEP_WL_KSO | SBSDIO_SLPCSR_WL_DEVON;
#endif
        bmask = compare_value;
    }

    while (attempts != 0)
    {
        /* Reliable KSO bit set/clr:
         * Sdiod sleep write access appears to be in sync with PMU 32khz clk
         * just one write attempt may fail,(same is with read ?)
         * in any case, read it back until it matches written value
         */
        result = whd_bus_read_register_value(whd_driver, BACKPLANE_FUNCTION, (uint32_t)SDIO_SLEEP_CSR, (uint8_t)1,
                                             &read_value);
        if ( ( (read_value & bmask) == compare_value ) && (result == WHD_SUCCESS) && (read_value != 0xFF) )
        {
            break;
        }

        cy_rtos_delay_milliseconds( ( uint32_t )KSO_WAIT_MS );

        CHECK_RETURN_IGNORE(whd_bus_write_register_value(whd_driver, BACKPLANE_FUNCTION, (uint32_t)SDIO_SLEEP_CSR,
                                                         (uint8_t)1, write_value) );
        attempts--;
    }

    if (attempts == 0)
    {
        WPRINT_WHD_ERROR( ("SDIO bus failed to come up , %s failed at %d \n", __func__, __LINE__) );
        return WHD_SDIO_BUS_UP_FAIL;
    }
    else
    {
        return WHD_SUCCESS;
    }
}

void whd_wlan_wake_from_host(whd_driver_t whd_driver);

void whd_wlan_wake_from_host(whd_driver_t whd_driver)
{
    uint32_t val32 = 0;
    whd_result_t result;

    result = whd_bus_read_backplane_value(whd_driver, D11_MACCONTROL_REG, D11_MACCONTROL_REG_SIZE, (uint8_t *)&val32);
    if (result != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR( ("%s:%d whd_bus_read_backplane_value failed\n", __FUNCTION__, __LINE__) );
    }
    WPRINT_WHD_DEBUG( ("%s: %d:before: maccontrol: 0x%08x\n", __FUNCTION__, __LINE__, (unsigned int)val32) );

    val32 = val32 | D11_MACCONTROL_REG_WAKE;
    result = whd_bus_write_backplane_value(whd_driver, D11_MACCONTROL_REG, D11_MACCONTROL_REG_SIZE, val32);
    if (result != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR( ("%s:%d whd_bus_write_backplane_value failed\n", __FUNCTION__, __LINE__) );
    }
    result = whd_bus_read_backplane_value(whd_driver, D11_MACCONTROL_REG, D11_MACCONTROL_REG_SIZE, (uint8_t *)&val32);
    if (result != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR( ("%s:%d whd_bus_read_backplane_value failed\n", __FUNCTION__, __LINE__) )
    }
    WPRINT_WHD_DEBUG( ("%s: %d:after: maccontrol: 0x%08x\n", __FUNCTION__, __LINE__, (unsigned int)val32) );
}
#ifndef CYCFG_ULP_SUPPORT_ENABLED
whd_result_t whd_wlan_bus_complete_ds_wake(whd_driver_t whd_driver, whd_bool_t wake_from_firmware,
                                           uint32_t wake_event_indication_addr, uint32_t wake_indication_addr,
                                           uint32_t sdio_control_addr)
{
    uint32_t val = 0;
    uint32_t val1 = 0;
    uint32_t count = 0;
    int32_t timeout = 0;
    /* Get chip number */

    uint16_t wlan_chip_id = whd_chip_get_chip_id(whd_driver);

    if (wlan_chip_id == 43012)
    {
        WPRINT_WHD_DEBUG_DS( ("%s: enter: wake_fr_fw=%d\n", __func__, (int )wake_from_firmware) );

        /* Need to do for all wake causes */
        while (WHD_SUCCESS !=
               whd_bus_write_register_value(whd_driver, BUS_FUNCTION, SDIOD_CCCR_IOEN, (uint8_t)1, SDIO_FUNC_ENABLE_1) )
        {
            WPRINT_WHD_DEBUG( ("Retry IOEN write\n") );
            cy_rtos_delay_milliseconds(10);
        }

        CHECK_RETURN(whd_bus_read_backplane_value(whd_driver, D11SHM_ADDR(wake_event_indication_addr), 2,
                                                  (uint8_t *)&val) );
        CHECK_RETURN(whd_bus_read_backplane_value(whd_driver, D11SHM_ADDR(wake_indication_addr), 2, (uint8_t *)&val) );

        if (WHD_TRUE == wake_from_firmware)
        {
            WPRINT_WHD_DEBUG_DS( ("%s: CCCR written\n", __func__) );

            /* Read DS1 CTRL STATUS SHM to Check whether HOST MAIL BOX interrupt is triggered due to TX/RX
             * For RX Cases , Value will be DS1_SLEEP
             * For TX Cases, Value will be MAC ON or RADIO_PHY_ON
             */
            CHECK_RETURN(whd_bus_read_backplane_value(whd_driver, D11SHM_ADDR(M_DS1_CTRL_STATUS), (uint8_t)2,
                                                      (uint8_t *)&val) );
            if (val == DS1_SLEEP)
            {
                /* HUDI communication to inform D11 to Process and Exit DS1 not required for D11 Wake cases
                 * Simply update PMU Resource Mask and Re-download firmware for this case
                 */
                WPRINT_WHD_DEBUG_DS( ("%s: D11 wake detected\n", __func__) );

                CHECK_RETURN(whd_bus_write_backplane_value(whd_driver, PMU_MINRESMASK, 1,
                                                           DEFAULT_43012_MIN_RES_MASK) );

                return WHD_SUCCESS;
            }

            CHECK_RETURN(whd_bus_write_backplane_value(whd_driver, D11SHM_ADDR(M_DS1_CTRL_SDIO), 2,
                                                       C_DS1_CTRL_SDIO_DS1_EXIT | C_DS1_CTRL_REQ_VALID) );
            WPRINT_WHD_DEBUG_DS( ("%s: SDIO ctl written\n", __func__) );

            CHECK_RETURN(whd_bus_read_backplane_value(whd_driver, D11SHM_ADDR(M_DS1_CTRL_SDIO), (uint8_t)2,
                                                      (uint8_t *)&val) );
            WPRINT_WHD_DEBUG_DS( ("%s: M_DS1_CTRL_SDIO = [%x]\n", __func__, (int )val) );

            CHECK_RETURN(whd_bus_read_backplane_value(whd_driver, D11_MACCONTROL_REG, (uint8_t)2, (uint8_t *)&val1) );
            WPRINT_WHD_DEBUG_DS( ("%s: before setting wake = [%x]\n", __func__, (int )val1) );

            CHECK_RETURN(whd_bus_write_backplane_value(whd_driver, D11_MACCONTROL_REG, 2,
                                                       val1 | D11_MACCONTROL_REG_WAKE) );
            WPRINT_WHD_DEBUG_DS( ("%s: After setting wake= [%x]\n", __func__, (int )val1) );

            while (!(val & C_DS1_CTRL_PROC_DONE) )
            {
                CHECK_RETURN(whd_bus_read_backplane_value(whd_driver, D11SHM_ADDR(M_DS1_CTRL_SDIO), (uint8_t)2,
                                                          (uint8_t *)&val) );
                count++;
                if (count == 1000)
                {
                    WPRINT_WHD_ERROR( ("%s: sdio timed out! Aborting\n", __func__) );
                    return WHD_TIMEOUT;
                }
            }

            CHECK_RETURN(whd_bus_read_backplane_value(whd_driver, D11SHM_ADDR(M_DS1_CTRL_SDIO), (uint8_t)2,
                                                      (uint8_t *)&val) );
            WPRINT_WHD_DEBUG_DS( ("%s: M_DS1_CTRL_SDIO after poll = [%x]\n", __func__, (int )val) );

            CHECK_RETURN(whd_bus_read_backplane_value(whd_driver, D11_MACCONTROL_REG, (uint8_t)2, (uint8_t *)&val1) );
            WPRINT_WHD_DEBUG_DS( ("%s: D11_MACCONTROL_REG before Clear = [%x]\n", __func__, (int )val1) );

            CHECK_RETURN(whd_bus_write_backplane_value(whd_driver, D11_MACCONTROL_REG, 2,
                                                       val1 & D11_MACCONTROL_CLEAR_WAKE) );
            WPRINT_WHD_DEBUG_DS( ("%s: D11_MACCONTROL_REG after Clear = [%x]\n", __func__, (int )val1) );

            CHECK_RETURN(whd_bus_write_backplane_value(whd_driver, PMU_MINRESMASK, 1, DEFAULT_43012_MIN_RES_MASK) );
            CHECK_RETURN(whd_bus_read_backplane_value(whd_driver, PMU_MINRESMASK, (uint8_t)1, (uint8_t *)&val1) );

            WPRINT_WHD_DEBUG_DS( ("%s: PMU_MINRESMASK = [%x]\n", __func__, (int )val1) );
        }
        else
        {
            while (WHD_SUCCESS != whd_ensure_wlan_bus_is_up(whd_driver) )
            {
                WPRINT_WHD_DEBUG_DS( ("Retrying bus is up\n") );
            }

            WPRINT_WHD_DEBUG_DS( ("write backplane\n") );

            CHECK_RETURN(whd_bus_read_backplane_value(whd_driver, D11SHM_ADDR(sdio_control_addr), 2, (uint8_t *)&val) );

            while (WHD_SUCCESS !=
                   whd_bus_write_backplane_value(whd_driver, D11SHM_ADDR(sdio_control_addr), 2,
                                                 val | C_DS1_CTRL_SDIO_DS1_EXIT | C_DS1_CTRL_REQ_VALID) )
            {
                WPRINT_WHD_DEBUG_DS( ("Retrying backplane write; addr=%x\n",
                                      (unsigned int )D11SHM_ADDR(sdio_control_addr) ) );
                cy_rtos_delay_milliseconds(100);
            }

            WPRINT_WHD_DEBUG_DS( ("wake host\n") );
            whd_wlan_wake_from_host(whd_driver);

            WPRINT_WHD_DEBUG_DS( ("poll for timeout\n") );
            /* Poll for PROC_DONE to be set by ucode; timeout after a good amount of time */
            for (timeout = WHD_WLAN_WAKE_TIMEOUT * 10, val = 0; timeout >= 0;
                 timeout -= WHD_SHARED_MEMORY_POLLING_DELAY)
            {
                whd_result_t read_result =
                    whd_bus_read_backplane_value(whd_driver, D11SHM_ADDR(sdio_control_addr), 2, (uint8_t *)&val);

                if ( (WHD_SUCCESS != read_result) || !( (val) & C_DS1_CTRL_PROC_DONE ) )
                {
                    cy_rtos_delay_milliseconds (WHD_SHARED_MEMORY_POLLING_DELAY);
                }
                else
                {
                    /* success! */
                    WPRINT_WHD_DEBUG_DS( ("Ucode has posted DONE\n") );
                    break;
                }
            }

            WPRINT_WHD_DEBUG_DS( ("%s: %d: 0x%08x\n", __func__, __LINE__, (unsigned int)val) );

            if ( (timeout < 0) && !(val & C_DS1_CTRL_PROC_DONE) )
            {
                WPRINT_WHD_ERROR( ("%s: Failed to enter DS1 Exit state!\n", __func__) );
                return WHD_TIMEOUT;
            }
        }
        WPRINT_WHD_DEBUG_DS( ("successfully completed DS wake sequence\n") );
        return WHD_SUCCESS;
    }
    else
    {
        UNUSED_PARAMETER(wake_from_firmware);
        UNUSED_PARAMETER(wake_event_indication_addr);
        UNUSED_PARAMETER(wake_indication_addr);
        UNUSED_PARAMETER(sdio_control_addr);

        WPRINT_WHD_DEBUG( ("%s: enter\n", __FUNCTION__) );

        CHECK_RETURN(whd_bus_write_register_value(whd_driver, BUS_FUNCTION, SDIOD_CCCR_IOEN, (uint8_t)1,
                                                  SDIO_FUNC_ENABLE_1) );
        WPRINT_WHD_DEBUG( ("%s: CCCR written\n", __FUNCTION__) );

        /* Read DS1 CTRL STATUS SHM to Check whether HOST MAIL BOX interrupt is triggered due to TX/RX
         * For RX Cases , Value will be DS1_SLEEP
         * For TX Cases, Value will be MAC ON or RADIO_PHY_ON
         */
        CHECK_RETURN(whd_bus_read_backplane_value(whd_driver, D11SHM_ADDR(M_DS1_CTRL_STATUS), (uint8_t)2,
                                                  (uint8_t *)&val) );
        if (val == DS1_SLEEP)
        {
            /* HUDI communication to inform D11 to Process and Exit DS1 not required for D11 Wake cases
             * Simply update PMU Resource Mask and Re-download firmware for this case
             */
            CHECK_RETURN(whd_bus_write_backplane_value(whd_driver, PMU_MINRESMASK, 1, DEFAULT_43012_MIN_RES_MASK) );
            return WHD_SUCCESS;
        }
        CHECK_RETURN(whd_bus_write_backplane_value(whd_driver, D11SHM_ADDR(M_DS1_CTRL_SDIO), 2,
                                                   C_DS1_CTRL_SDIO_DS1_EXIT | C_DS1_CTRL_REQ_VALID) );
        WPRINT_WHD_DEBUG( ("%s: SDIO ctl written\n", __FUNCTION__) );

        CHECK_RETURN(whd_bus_read_backplane_value(whd_driver, D11SHM_ADDR(
                                                      M_DS1_CTRL_SDIO), (uint8_t)2, (uint8_t *)&val) );
        WPRINT_WHD_DEBUG( ("%s: M_DS1_CTRL_SDIO = [%x]\n", __FUNCTION__, (int)val) );

        CHECK_RETURN(whd_bus_read_backplane_value(whd_driver, D11_MACCONTROL_REG, (uint8_t)2, (uint8_t *)&val1) );
        WPRINT_WHD_DEBUG( ("%s: before setting wake = [%x]\n", __FUNCTION__, (int)val1) );
        CHECK_RETURN(whd_bus_write_backplane_value(whd_driver, D11_MACCONTROL_REG, 2,
                                                   val1 | D11_MACCONTROL_REG_WAKE) );
        WPRINT_WHD_DEBUG( ("%s: After setting wake= [%x]\n", __FUNCTION__, (int)val1) );

        while (!(val & C_DS1_CTRL_PROC_DONE) )
        {
            CHECK_RETURN(whd_bus_read_backplane_value(whd_driver, D11SHM_ADDR(M_DS1_CTRL_SDIO), (uint8_t)2,
                                                      (uint8_t *)&val) );
            count++;
            if (count == 1000)
            {
                WPRINT_WHD_ERROR( ("%s: sdio timed out! Aborting\n", __FUNCTION__) );
                return WHD_TIMEOUT;
            }
        }

        CHECK_RETURN(whd_bus_read_backplane_value(whd_driver, D11SHM_ADDR(
                                                      M_DS1_CTRL_SDIO), (uint8_t)2, (uint8_t *)&val) );
        WPRINT_WHD_DEBUG( ("%s: M_DS1_CTRL_SDIO after poll = [%x]\n", __FUNCTION__, (int)val) );

        CHECK_RETURN(whd_bus_read_backplane_value(whd_driver, D11_MACCONTROL_REG, (uint8_t)2, (uint8_t *)&val1) );
        WPRINT_WHD_DEBUG( ("%s: D11_MACCONTROL_REG before Clear = [%x]\n", __FUNCTION__, (int)val1) );

        CHECK_RETURN(whd_bus_write_backplane_value(whd_driver, D11_MACCONTROL_REG, 2,
                                                   val1 & D11_MACCONTROL_CLEAR_WAKE) );
        WPRINT_WHD_DEBUG( ("%s: D11_MACCONTROL_REG after Clear = [%x]\n", __FUNCTION__, (int)val1) );

        CHECK_RETURN(whd_bus_write_backplane_value(whd_driver, PMU_MINRESMASK, 1, DEFAULT_43012_MIN_RES_MASK) );
        CHECK_RETURN(whd_bus_read_backplane_value(whd_driver, PMU_MINRESMASK, (uint8_t)1, (uint8_t *)&val1) );
        WPRINT_WHD_DEBUG( ("%s: PMU_MINRESMASK = [%x]\n", __FUNCTION__, (int)val1) );
        return WHD_SUCCESS;
    }
}
#else
whd_result_t whd_wlan_bus_complete_ds_wake(whd_driver_t whd_driver, whd_bool_t wake_from_ucode )
{
#ifndef DM_43022C1
    uint32_t val;
    uint32_t val1;
    uint32_t count = 0;
    uint32_t wake_event_ind = 0;
    uint32_t ulp_wake_ind  = 0;
#endif
    uint8_t enb_rd = 0;
    /* Get chip number */

    uint16_t wlan_chip_id = whd_chip_get_chip_id(whd_driver);

    if ((wlan_chip_id == 43012) || (wlan_chip_id == 43022))
    {
        WPRINT_WHD_DEBUG_DS( ("%s: enter: wake_fr_fw=%d\n", __func__, (int )wake_from_ucode) );

        whd_driver->ds_exit_in_progress = WHD_TRUE;
        /* Release the transceive_semaphore, if acquired for some tx/rx operations */
        (void)cy_rtos_set_semaphore(&whd_driver->thread_info.transceive_semaphore, WHD_TRUE);

        whd_driver->internal_info.whd_wlan_status.state = WLAN_OFF;

        CHECK_RETURN(whd_bus_read_register_value(whd_driver, BUS_FUNCTION, SDIOD_CCCR_IOEN, (uint8_t)1, &enb_rd))

        if(enb_rd == SDIO_FUNC_ENABLE_1)
        {
            WPRINT_WHD_DEBUG(("GOT UCODE INTERRUPT \n"));

#ifndef DM_43022C1
            CHECK_RETURN(whd_bus_read_backplane_value(whd_driver, D11SHM_ADDR(M_WAKEEVENT_IND), 4,
                                                      (uint8_t *)&wake_event_ind) );
            WPRINT_WHD_DEBUG(("M_WAKEEVENT_IND is 0x%x \n", wake_event_ind));
            CHECK_RETURN(whd_bus_read_backplane_value(whd_driver, D11SHM_ADDR(M_ULP_WAKE_IND), 4, (uint8_t *)&ulp_wake_ind) );
            WPRINT_WHD_DEBUG(("M_ULP_WAKE_IND is 0x%x \n", ulp_wake_ind));

            if((M_WAKEEVENT_IND % 4))
            {
                 wake_event_ind = (wake_event_ind >> 16);
                 WPRINT_WHD_DEBUG(("M_WAKEEVENT_IND[MOD] is 0x%x \n", wake_event_ind));
            }
            else
            {
                wake_event_ind = (wake_event_ind & 0x0000FFFF);
                WPRINT_WHD_DEBUG(("M_WAKEEVENT_IND[MOD-1] is 0x%x \n", wake_event_ind));
            }

            if((M_ULP_WAKE_IND % 4))
            {
                 ulp_wake_ind = (ulp_wake_ind >> 16);
                 WPRINT_WHD_DEBUG(("M_ULP_WAKE_IND[MOD] is 0x%x \n", ulp_wake_ind));
            }
            else
            {
                ulp_wake_ind = (ulp_wake_ind & 0x0000FFFF);
                WPRINT_WHD_DEBUG(("M_ULP_WAKE_IND[MOD-1] is 0x%x \n", ulp_wake_ind));
            }
            CHECK_RETURN(whd_bus_write_backplane_value(whd_driver, PMU_MINRESMASK, 4,
                                                                           DEFAULT_43012_MIN_RES_MASK) );
#else
            /* For 43022DM, host should not access any core other than SDIO,
               it it access, it will end up in FW crash(intentional crash) */
            WPRINT_WHD_INFO( ("Successfully completed DS wake sequence\n") );
            return WHD_SUCCESS;
#endif /* DM_43022C1 */

#ifndef DM_43022C1
            if (wake_from_ucode == WHD_TRUE)
            {
                WPRINT_WHD_DEBUG_DS( ("%s: CCCR written\n", __func__) );

                /* Read DS1 CTRL STATUS SHM to Check whether HOST MAIL BOX interrupt is triggered due to TX/RX
                 * For RX Cases , Value will be DS1_SLEEP
                 * For TX Cases, Value will be MAC ON or RADIO_PHY_ON
                 */
                CHECK_RETURN(whd_bus_read_backplane_value(whd_driver, D11SHM_ADDR(M_DS1_CTRL_STATUS), (uint8_t)2,
                                                          (uint8_t *)&val) );

                WPRINT_WHD_DEBUG((" DS1 CTRL STATUS is 0x%x\n", val));

                if ( (wake_event_ind > 0) || (ulp_wake_ind > 0) )
                {
                    /* HUDI communication to inform D11 to Process and Exit DS1 not required for D11 Wake cases
                    * Simply update PMU Resource Mask and Re-download firmware for this case
                    */
                    WPRINT_WHD_DEBUG_DS( ("%s: D11 wake detected\n", __func__) );

                    CHECK_RETURN(whd_bus_write_backplane_value(whd_driver, PMU_MINRESMASK, 4,
                                                               DEFAULT_43012_MIN_RES_MASK) );

                }
                else
                {

                    CHECK_RETURN(whd_bus_write_backplane_value(whd_driver, D11SHM_ADDR(M_DS1_CTRL_SDIO), 2,
                                                               C_DS1_CTRL_SDIO_DS1_EXIT | C_DS1_CTRL_REQ_VALID) );
                    WPRINT_WHD_DEBUG_DS( ("%s: SDIO ctl written\n", __func__) );

                    CHECK_RETURN(whd_bus_read_backplane_value(whd_driver, D11SHM_ADDR(M_DS1_CTRL_SDIO), (uint8_t)2,
                                                              (uint8_t *)&val) );
                    WPRINT_WHD_DEBUG_DS( ("%s: M_DS1_CTRL_SDIO = [%x]\n", __func__, (int )val) );

                    CHECK_RETURN(whd_bus_read_backplane_value(whd_driver, D11_MACCONTROL_REG, (uint8_t)4, (uint8_t *)&val1) );
                    WPRINT_WHD_DEBUG_DS( ("%s: before setting wake = [%x]\n", __func__, (int )val1) );

                    CHECK_RETURN(whd_bus_write_backplane_value(whd_driver, D11_MACCONTROL_REG, 4,
                                                               val1 | D11_MACCONTROL_REG_WAKE) );
                    WPRINT_WHD_DEBUG_DS( ("%s: After setting wake= [%x]\n", __func__, (int )val1) );

                    while (!(val & C_DS1_CTRL_PROC_DONE) )
                    {
                        CHECK_RETURN(whd_bus_read_backplane_value(whd_driver, D11SHM_ADDR(M_DS1_CTRL_SDIO), (uint8_t)2,
                                                                 (uint8_t *)&val) );
                        count++;
                        WPRINT_WHD_DEBUG(("PROC READ - 0x%x \t", val));
                        if (count == 1000)
                        {
                            WPRINT_WHD_ERROR( ("%s: sdio timed out! Aborting\n", __func__) );
                            return WHD_TIMEOUT;
                        }
                    }

                    CHECK_RETURN(whd_bus_read_backplane_value(whd_driver, D11SHM_ADDR(M_DS1_CTRL_SDIO), (uint8_t)2,
                                                              (uint8_t *)&val) );
                    WPRINT_WHD_DEBUG_DS( ("%s: M_DS1_CTRL_SDIO after poll = [%x]\n", __func__, (int )val) );

                    CHECK_RETURN(whd_bus_read_backplane_value(whd_driver, D11_MACCONTROL_REG, (uint8_t)2, (uint8_t *)&val1) );
                    WPRINT_WHD_DEBUG_DS( ("%s: D11_MACCONTROL_REG before Clear = [%x]\n", __func__, (int )val1) );

                    CHECK_RETURN(whd_bus_write_backplane_value(whd_driver, D11_MACCONTROL_REG, 2,
                                                               val1 & D11_MACCONTROL_CLEAR_WAKE) );
                    WPRINT_WHD_DEBUG_DS( ("%s: D11_MACCONTROL_REG after Clear = [%x]\n", __func__, (int )val1) );

                    CHECK_RETURN(whd_bus_write_backplane_value(whd_driver, PMU_MINRESMASK, 4, DEFAULT_43012_MIN_RES_MASK) );
                    CHECK_RETURN(whd_bus_read_backplane_value(whd_driver, PMU_MINRESMASK, (uint8_t)4, (uint8_t *)&val1) );

                    WPRINT_WHD_DEBUG_DS( ("%s: PMU_MINRESMASK = [%x]\n", __func__, (int )val1) );

                }
                WPRINT_WHD_INFO( ("successfully completed DS wake sequence\n") );
                return WHD_SUCCESS;
            }
#endif /* DM_43022C1 */
        }
        else
        {
            WPRINT_WHD_ERROR( ("SDIO I/O Enable Failed\n") );
            return WHD_BADARG;
        }
    }

    WPRINT_WHD_ERROR( ("Chip Doesn't support ULP Wake \n") );
    return WHD_BADARG;
}

whd_result_t whd_ensure_wlan_bus_not_in_deep_sleep(whd_driver_t whd_driver)
{
    uint16_t wlan_chip_id = whd_chip_get_chip_id(whd_driver);
    uint8_t byte_data = 0;
    int loop_count = 0;

    if ((wlan_chip_id == 43012) || (wlan_chip_id == 43022))
    {
        /* Wait for F2 to be ready */
        while ( ( (whd_bus_read_register_value(whd_driver, BUS_FUNCTION, SDIOD_CCCR_IORDY, (uint8_t)1, &byte_data) ) != WHD_SUCCESS ) ||
                ( ( (byte_data & SDIO_FUNC_READY_2) == 0 ) && (loop_count < WAKE_FROM_UCODE_TIMEOUT_LOOPS) ) )
        {
              (void)cy_rtos_delay_milliseconds( (uint32_t)WAKE_FROM_UCODE_CHECK_PER_LOOP );   /* Ignore return - nothing can be done if it fails */
              loop_count++;
        }

        if (loop_count >= (uint32_t)WAKE_FROM_UCODE_TIMEOUT_LOOPS)
        {
            WPRINT_WHD_DEBUG( ("F2 ready timed out \n") );
            /* Reachable after hitting assert */
            return WHD_FALSE;
        }

    }
    return WHD_TRUE;
}
#endif /* CYCFG_ULP_SUPPORT_ENABLED */

#if defined(COMPONENT_CAT5) && !defined(WHD_DISABLE_PDS)
bool whd_syspm_registered_callback(cyhal_syspm_callback_state_t state, cyhal_syspm_callback_mode_t mode, void *arg)
{
    whd_driver_t whd_driver = (whd_driver_t)arg;

    switch(mode)
    {
     case CYHAL_SYSPM_CHECK_READY:
        /* Check whether wlan can go to sleep or not */
        /* Returns true, if wlan allows system to go to sleep */
        if(whd_driver->pds_sleep_allow == WHD_TRUE)
        {
           return WHD_TRUE;
        }
        else
        {
           return WHD_FALSE;
        }

     case CYHAL_SYSPM_BEFORE_TRANSITION:
        /* WHD will allow only to sleep after detecting idle and done with the D3inform/D3 ack,
            so need to do any job here */
        return WHD_TRUE;

    case CYHAL_SYSPM_AFTER_TRANSITION:
       /* WHD thread will be resumed in this case, if needed */
       return WHD_TRUE;

    default:
        return WHD_FALSE;
    }
}
#endif /* defined(COMPONENT_CAT5) && !defined(WHD_DISABLE_PDS) */
