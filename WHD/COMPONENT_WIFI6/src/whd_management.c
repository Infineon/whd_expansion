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

/** @file
 *  Implements initialisation and other management functions for WHD system
 *
 */

#include <string.h>
#include "bus_protocols/whd_bus_common.h"
#include "bus_protocols/whd_bus_protocol_interface.h"
#include "whd_debug.h"
#include "whd_events_int.h"
#include "whd_int.h"
#include "whd_chip.h"
#include "whd_utils.h"
#ifndef PROTO_MSGBUF
#include "whd_sdpcm.h"
#endif /* PROTO_MSGBUF */
#include "whd_wifi_api.h"
#include "whd_clm.h"
#include "whd_wlioctl.h"
#include "whd_types_int.h"
#include "whd_chip_constants.h"
#include "whd_proto.h"
#if defined(COMPONENT_WLANSENSE)
#include "whd_wlansense_core.h"
#endif /* defined(COMPONENT_WLANSENSE) */

#if defined(COMPONENT_CAT5) && !defined(WHD_DISABLE_PDS)
#include "cyhal_syspm.h"
#endif /* defined(COMPONENT_CAT5) && !defined(WHD_DISABLE_PDS) */

/******************************************************
*             Constants
******************************************************/

#define MAX_POST_SET_COUNTRY_RETRY  3

#define MINIMUM_WHD_STACK_SIZE        (1024 + 1200 + 2500)

#define DEFAULT_PM2_SLEEP_RET_TIME  200

#ifdef PROTO_MSGBUF
#ifndef COMPONENT_SDIO_HM
#define DMA_ALLOC_SIZE                 (15000)
#else
#define DMA_ALLOC_SIZE                 (15000 + SDIO_F2_DMA_BUFFER_SIZE)
#endif /* COMPONENT_SDIO_HM */
#endif /* PROTO_MSGBUF */
/******************************************************
*             Static Variables
******************************************************/

/******************************************************
*             Function definitions
******************************************************/
whd_result_t whd_management_wifi_platform_init(whd_driver_t whd_driver, whd_country_code_t country,
                                               whd_bool_t resume_after_deep_sleep);
void whd_wifi_bus_irq_handle(void *handler_arg, uint32_t event);

whd_interface_t whd_get_primary_interface(whd_driver_t whd_driver)
{
    if (whd_driver->iflist[0] != NULL)
    {
        return whd_driver->iflist[0];
    }
    return NULL;
}

whd_interface_t whd_get_interface(whd_driver_t whd_driver, uint8_t ifidx)
{
    if (whd_driver->iflist[ifidx] != NULL)
    {
        return whd_driver->iflist[ifidx];
    }
    return NULL;
}

whd_result_t whd_add_interface(whd_driver_t whd_driver, uint8_t bsscfgidx, uint8_t ifidx,
                               const char *name, whd_mac_t *mac_addr,  whd_interface_t *ifpp)
{
    whd_interface_t ifp;

    if (!whd_driver || !ifpp)
    {
        WPRINT_WHD_ERROR( ("Invalid param in func %s at line %d \n",
                           __func__, __LINE__) );
        return WHD_WLAN_BADARG;
    }

    if (bsscfgidx < WHD_INTERFACE_MAX)
    {
        if (whd_driver->iflist[bsscfgidx] != NULL)
        {
            *ifpp = whd_driver->iflist[bsscfgidx];
            return WHD_SUCCESS;
        }

        if ( (ifp = (whd_interface_t)whd_mem_malloc(sizeof(struct whd_interface) ) ) != NULL )
        {
            whd_mem_memset(ifp, 0, (sizeof(struct whd_interface) ) );
            *ifpp = ifp;
            ifp->whd_driver = whd_driver;

            /* Add a interface name */
            /* strncpy doesn't terminate with null if the src string is long */
            ifp->if_name[WHD_MSG_IFNAME_MAX - 1] = '\0';
            strncpy(ifp->if_name, name, sizeof(ifp->if_name) - 1);
            whd_mem_memset(ifp->event_reg_list, WHD_EVENT_NOT_REGISTERED, sizeof(ifp->event_reg_list) );
            /* Primary interface takes 0 as default */
            ifp->ifidx = ifidx;
            ifp->bsscfgidx = bsscfgidx;

            if (mac_addr != NULL)
                whd_mem_memcpy(ifp->mac_addr.octet, mac_addr->octet, sizeof(whd_mac_t) );
            else
                whd_mem_memset(ifp->mac_addr.octet, 0, sizeof(whd_mac_t) );

            whd_driver->iflist[bsscfgidx] = ifp;
            whd_driver->if2ifp[ifidx] = bsscfgidx;
        }
        else
        {
            return WHD_MALLOC_FAILURE;
        }

        return WHD_SUCCESS;
    }
    return WHD_INVALID_INTERFACE;
}

whd_result_t whd_add_primary_interface(whd_driver_t whd_driver, whd_interface_t *ifpp)
{
    return whd_add_interface(whd_driver, 0, 0, "wlan0", NULL, ifpp);
}

whd_result_t whd_add_secondary_interface(whd_driver_t whd_driver, whd_mac_t *mac_addr, whd_interface_t *ifpp)
{
#if !defined(COMPONENT_WLANSENSE)
    return whd_add_interface(whd_driver, 1, 1, "wlan1", mac_addr, ifpp);
#else
    return 0;
#endif /* !defined(COMPONENT_WLANSENSE) */
}

whd_result_t
whd_wifi_set_country_code(whd_interface_t ifp, whd_country_code_t country_code)
{
    whd_result_t ret;
    wl_country_t *country_struct;
    uint32_t *ptr;
    whd_buffer_t buffer;
    whd_driver_t whd_driver = ifp->whd_driver;

    /* Send set country command */
    /* This is the first time that the WLAN chip is required to respond
     * in it's normal run mode.
     * If you are porting a new system and it stalls here, it could
     * be one of the following problems:
     *   - Bus interrupt not triggering properly - the WLAN chip is unable to signal the host that there is data available.
     *   - Timing problems - if the timeouts on semaphores are not working correctly, then the
     *                       system might think that the IOCTL has timed out much faster than it should do.
     *
     */

    country_struct = (wl_country_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer, (uint16_t)sizeof(wl_country_t),
                                                                IOVAR_STR_COUNTRY);
    if (country_struct == NULL)
    {
        whd_assert("Could not get buffer for IOCTL", 0 != 0);
        return WHD_BUFFER_ALLOC_FAIL;
    }
    whd_mem_memset(country_struct, 0, sizeof(wl_country_t) );

    ptr = (uint32_t *)country_struct->ccode;
    *ptr = (uint32_t)country_code & 0x0000ffff;
    ptr = (uint32_t *)country_struct->country_abbrev;
    *ptr = (uint32_t)country_code & 0x0000ffff;
    country_struct->rev = (int32_t)( (country_code & 0xffff0000) >> 16 );

    /* if regrev is 0 then set regrev to -1 so the FW will use any NVRAM/OTP configured aggregate
     * to choose the regrev.  If there is no aggregate configured then the FW will try to use regrev 0.
     */
    if (country_struct->rev == 0)
    {
        country_struct->rev = (int32_t)htod32(-1);
    }
    ret = whd_proto_set_iovar(ifp, buffer, 0);

    return ret;
}
whd_result_t whd_init(whd_driver_t *whd_driver_ptr, whd_init_config_t *whd_init_config,
                  whd_resource_source_t *resource_ops, whd_buffer_funcs_t *buffer_ops,
                  whd_netif_funcs_t *network_ops)
{
    whd_driver_t whd_drv;

    if (!whd_driver_ptr || !buffer_ops || !network_ops || !resource_ops || !whd_init_config)
    {
        WPRINT_WHD_ERROR( ("Invalid param in func %s at line %d \n",
                           __func__, __LINE__) );
        return WHD_WLAN_BADARG;
    }
    if (whd_init_config->thread_stack_size <  MINIMUM_WHD_STACK_SIZE)
    {
        WPRINT_WHD_INFO( ("Stack size is less than minimum stack size required.\n") );
        return WHD_WLAN_BUFTOOSHORT;
    }

    if ( (whd_drv = (whd_driver_t)whd_mem_malloc(sizeof(struct whd_driver) ) ) != NULL )
    {
        whd_mem_memset(whd_drv, 0, sizeof(struct whd_driver) );
        *whd_driver_ptr = whd_drv;
        whd_drv->buffer_if = buffer_ops;
        whd_drv->network_if = network_ops;
        whd_drv->resource_if = resource_ops;
        whd_bus_common_info_init(whd_drv);
        whd_thread_info_init(whd_drv, whd_init_config);
        whd_internal_info_init(whd_drv);
        whd_ap_info_init(whd_drv);
        //whd_wifi_sleep_info_init(whd_drv);
        whd_wifi_chip_info_init(whd_drv);

#ifdef PROTO_MSGBUF
        /* Initialize pool for WLAN M2M DMA to access, WHD has to request pool memory
           and open the access for WLAN through APIs(Secure Call in BTFW)*/
        whd_dmapool_init(DMA_ALLOC_SIZE);
#endif

        whd_drv->bus_gspi_32bit = WHD_FALSE;

        if (whd_init_config->country == 0)
            whd_drv->country = WHD_COUNTRY_UNITED_STATES;
        else
            whd_drv->country = whd_init_config->country;
    }
    else
    {
        return WHD_MALLOC_FAILURE;
    }
    return WHD_SUCCESS;
}

whd_result_t whd_deinit(whd_interface_t ifp)
{
    uint8_t i;
    whd_driver_t whd_driver;

    CHECK_IFP_NULL(ifp);
    whd_driver = ifp->whd_driver;
    whd_internal_info_t *internal_info = &whd_driver->internal_info;
    wifi_console_t *c = internal_info->c;
    c = &internal_info->console;

    if (whd_driver->internal_info.whd_wlan_status.state != WLAN_OFF)
    {
        WPRINT_WHD_ERROR( ("Could not deinit whd because wifi power is on\n") );
        return WHD_WLAN_NOTDOWN;
    }

    if ( (whd_driver->bus_priv != NULL) || (whd_driver->bus_if != NULL) )
    {
        WPRINT_WHD_ERROR( ("Could not deinit whd because bus is attaced\n") );
        return WHD_WLAN_NOTDOWN;
    }

    for (i = 0; i < WHD_INTERFACE_MAX; i++)
    {
        if (whd_driver->iflist[i] != NULL)
        {
            whd_mem_free(whd_driver->iflist[i]);
            whd_driver->iflist[i] = NULL;
        }
    }

    /* Freeing the buffer allocated to read logs (whd_wifi_read_wlan_log_unsafe) */
    if (c->buf != NULL)
    {
        whd_mem_free(c->buf);
    }

#ifdef PROTO_MSGBUF
    whd_dmapool_reset();
#endif

    whd_internal_info_deinit(whd_driver);
    whd_bus_common_info_deinit(whd_driver);
    whd_mem_free(whd_driver);

    return WHD_SUCCESS;
}

/**
 * Initialize Wi-Fi platform
 *
 * - Initializes the required parts of the hardware platform
 *   i.e. pins for SDIO/SPI, interrupt, reset, power etc.
 *
 * - Initializes the WHD thread which arbitrates access
 *   to the SDIO/SPI bus
 *
 * @return WHD_SUCCESS if initialization is successful, error code otherwise
 */

whd_result_t whd_management_wifi_platform_init(whd_driver_t whd_driver, whd_country_code_t country,
                                               whd_bool_t resume_after_deep_sleep)
{
    whd_result_t retval;

#if defined(COMPONENT_CAT5) && !defined(WHD_DISABLE_PDS)
    whd_driver->whd_syspm_cb_data.states = (cyhal_syspm_callback_state_t)CYHAL_SYSPM_CB_CPU_DEEPSLEEP;
    whd_driver->whd_syspm_cb_data.callback = &whd_syspm_registered_callback;
    whd_driver->whd_syspm_cb_data.ignore_modes = ( CYHAL_SYSPM_CHECK_FAIL );
    whd_driver->whd_syspm_cb_data.args = whd_driver;
    whd_driver->whd_syspm_cb_data.next = NULL;
#endif /* defined(COMPONENT_CAT5) && !defined(WHD_DISABLE_PDS) */

    whd_driver->internal_info.whd_wlan_status.country_code = country;

    if (resume_after_deep_sleep == WHD_TRUE)
    {
        retval = ( whd_result_t )whd_bus_resume_after_deep_sleep(whd_driver);
    }
    else
    {
        whd_bus_init_stats(whd_driver);
        retval = ( whd_result_t )whd_bus_init(whd_driver);
    }

    if (retval != WHD_SUCCESS)
    {
        /* May have been due to user abort */
        WPRINT_WHD_INFO( ("Could not initialize bus\n") );
        return retval;
    }

    CHECK_RETURN(whd_proto_attach(whd_driver) );

    /* WLAN device is now powered up. Change state from OFF to DOWN */
    whd_driver->internal_info.whd_wlan_status.state = WLAN_DOWN;

    retval = whd_thread_init(whd_driver);
    if (retval != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR( ("Could not initialize WHD thread\n") );
        return retval;
    }

#if defined(COMPONENT_CAT5) && !defined(WHD_DISABLE_PDS)
    /* Initialize a timer for the data/ctrl activity monitoring */
    cyhal_syspm_register_callback(&whd_driver->whd_syspm_cb_data);
#endif /* defined(COMPONENT_CAT5) && !defined(WHD_DISABLE_PDS) */

#ifdef COMPONENT_SDIO_HM
    cy_rtos_init_mutex(&whd_driver->whd_hm_tx_lock);
#endif

    return WHD_SUCCESS;
}

/**
 * Turn on the Wi-Fi device
 *
 * - Initialize Wi-Fi device
 *
 * - Program various WiFi parameters and modes
 *
 * @return WHD_SUCCESS if initialization is successful, error code otherwise
 */
whd_result_t whd_wifi_on(whd_driver_t whd_driver, whd_interface_t *ifpp)
{
    whd_result_t retval;
    whd_buffer_t buffer;
    uint8_t *event_mask;
    uint32_t *data;
    uint32_t counter;
    whd_interface_t ifp;
    uint16_t wlan_chip_id = 0;

#if defined(COMPONENT_CAT5) && !defined(WHD_DISABLE_PDS)
    /* For H1CP, the BTSS sleep is enabled by default, so acquire the lock before doing the initialization process,
     * release once all the WHD initialization process is done */
    cy_rtos_init_mutex(&whd_driver->sleep_mutex);
    whd_pds_lock_sleep(whd_driver);
#endif

    if (!whd_driver || !ifpp)
    {
        WPRINT_WHD_ERROR( ("Invalid param in func %s at line %d \n",
                           __func__, __LINE__) );
        return WHD_WLAN_BADARG;
    }

    if (whd_driver->internal_info.whd_wlan_status.state == WLAN_UP)
    {
        return WHD_SUCCESS;
    }

    whd_init_stats(whd_driver);

    retval = whd_management_wifi_platform_init(whd_driver, whd_driver->country, WHD_FALSE);
    if (retval != WHD_SUCCESS)
    {
        WPRINT_WHD_INFO( ("Could not initialize wifi platform\n") );
        return retval;
    }

    whd_add_primary_interface(whd_driver, ifpp);
    ifp = *ifpp;

#ifdef WHD_ZEPHYR
    cy_rtos_delay_milliseconds(15000);
#endif

    /* Download blob file if exists */
    retval = whd_process_clm_data(ifp);
    if (retval != WHD_SUCCESS)
    {
        WPRINT_MACRO( ("****************************************************\n"
                       "** ERROR: WLAN: could not download clm_blob file\n"
                       "** FATAL ERROR: system unusable, CLM blob file not found or corrupted.\n"
                       "****************************************************\n") );
        return retval;
    }

#ifndef PROTO_MSGBUF
    retval = whd_bus_share_bt_init(whd_driver);
    if (retval != WHD_SUCCESS)
    {
        WPRINT_WHD_INFO( ("Shared bus for bt is fail\n") );
    }
#endif

    /* Get FW Capability */
    retval = whd_wifi_read_fw_capabilities(ifp);
    if (retval != WHD_SUCCESS)
    {
        WPRINT_WHD_INFO( ("Get FW Capabilities Fail\n") );
    }

#ifndef PROTO_MSGBUF    /* This is needed for cdc/bcd - sdpcm protocol */
    /* Turn off SDPCM TX Glomming */
    /* Note: This is only required for later chips.
     * The 4319 has glomming off by default however the 43362 has it on by default.
     */
    data = (uint32_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer, (uint16_t)4, IOVAR_STR_TX_GLOM);
    if (data == NULL)
    {
        whd_assert("Could not get buffer for IOVAR", 0 != 0);
        return WHD_BUFFER_ALLOC_FAIL;
    }
    *data = 0;
    retval = whd_proto_set_iovar(ifp, buffer, 0);
    if ( (retval != WHD_SUCCESS) && (retval != WHD_WLAN_UNSUPPORTED) )
    {
        /* Note: System may time out here if bus interrupts are not working properly */
        WPRINT_WHD_ERROR( ("Could not turn off TX glomming\n") );
        return retval;
    }
#endif

    /* Turn APSTA on */
    data = (uint32_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer, (uint16_t)sizeof(*data), IOVAR_STR_APSTA);
    if (data == NULL)
    {
        whd_assert("Could not get buffer for IOVAR", 0 != 0);
        return WHD_BUFFER_ALLOC_FAIL;
    }
    *data = htod32( (uint32_t)1 );
    /* This will fail on manufacturing test build since it does not have APSTA available */
    retval = whd_proto_set_iovar(ifp, buffer, 0);
    if (retval == WHD_WLAN_UNSUPPORTED)
    {
        WPRINT_WHD_DEBUG( ("Firmware does not support APSTA\n") );
    }
    else if (retval != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR( ("Could not turn on APSTA\n") );
        return retval;
    }
    retval = whd_wifi_set_country_code(ifp, whd_driver->internal_info.whd_wlan_status.country_code);

    if (retval != WHD_SUCCESS)
    {
        /* Could not set wifi country */
        WPRINT_WHD_ERROR( ("Could not set Country code\n") );
        return retval;
    }

    /* NOTE: The set country command requires time to process on the WLAN firmware and
    * the following IOCTL may fail on initial attempts therefore we try a few times */

    /* Set the event mask, indicating initially we do not want any asynchronous events */
    for (counter = 0, retval = WHD_PENDING; retval != WHD_SUCCESS && counter < (uint32_t)MAX_POST_SET_COUNTRY_RETRY;
         ++counter)
    {
        event_mask = (uint8_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer, (uint16_t)WL_EVENTING_MASK_LEN,
                                                           IOVAR_STR_EVENT_MSGS);
        if (event_mask == NULL)
        {
            whd_assert("Could not get buffer for IOVAR", 0 != 0);
            return WHD_BUFFER_ALLOC_FAIL;
        }
        whd_mem_memset(event_mask, 0, (size_t)WL_EVENTING_MASK_LEN);
        retval = whd_proto_set_iovar(ifp, buffer, 0);
    }
    if (retval != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR( ("Could not set Event mask\n") );
        return retval;
    }

    /* Send UP command */
    CHECK_RETURN(whd_wifi_set_up(ifp) );

    if (whd_driver->chip_info.fwcap_flags & (1 << WHD_FWCAP_OFFLOADS) )
    {
        retval = whd_wifi_offload_config(ifp, OFFLOAD_FEATURE, 0);
        if (retval != WHD_SUCCESS)
        {
           /* Could not initialization offload config */
           WPRINT_WHD_ERROR( ("Could not init offload config\n") );
           return retval;
        }
    }

    wlan_chip_id = whd_chip_get_chip_id(whd_driver);
    /* WAR: Disable WLAN PM/mpc for 43907 low power issue */
    if ( (wlan_chip_id == 43909) || (wlan_chip_id == 43907) || (wlan_chip_id == 54907) )
    {
        retval = whd_wifi_disable_powersave(ifp);
        if (retval != WHD_SUCCESS)
        {
            WPRINT_WHD_ERROR( ("Failed to disable PM for 43907\n") );
            return retval;
        }
        retval = whd_wifi_set_iovar_value(ifp, IOVAR_STR_MPC, 0);
        if (retval != WHD_SUCCESS)
        {
            WPRINT_WHD_ERROR( ("Failed to disable mpc for 43907\n") );
            return retval;
        }
    }
    else
    {
        CHECK_RETURN(whd_wifi_enable_powersave_with_throughput(ifp, DEFAULT_PM2_SLEEP_RET_TIME));
        if(wlan_chip_id == 55900)
        {
            CHECK_RETURN(whd_wifi_set_iovar_value(ifp, IOVAR_STR_MPC, 1));
        }
    }

#ifdef CYCFG_ULP_SUPPORT_ENABLED
    /* Configuring OOB mode in fw */
    if(wlan_chip_id == 43022)
    {
        CHECK_RETURN_IGNORE(whd_wifi_set_iovar_value(ifp, IOVAR_STR_ULP_HOST_INTR_MODE, ULP_OOB_INTR_MODE));
    }
#endif

#ifndef PROTO_MSGBUF
    /* Set the GMode */
    data = (uint32_t *)whd_proto_get_ioctl_buffer(whd_driver, &buffer, (uint16_t)4);
    if (data == NULL)
    {
        whd_assert("Could not get buffer for IOCTL", 0 != 0);
        return WHD_BUFFER_ALLOC_FAIL;
    }
    *data = htod32( (uint32_t)GMODE_AUTO );
    retval = whd_proto_set_ioctl(ifp, WLC_SET_GMODE, buffer, 0);
    if (retval != WHD_SUCCESS)
    {
        /* Note: System may time out here if bus interrupts are not working properly */
        WPRINT_WHD_ERROR( ("Error setting gmode\n") );
        return retval;
    }
#endif

    /* Disabling scanmac randomisation for H1Combo
     * Scanmac randomisation leads to probe requests with random mac address
     * This causes WPS failure with some APs
     */
    if (wlan_chip_id == 55500)
    {
        if (whd_configure_scanmac_randomisation(ifp, WHD_FALSE) != WHD_SUCCESS)
        {
            WPRINT_WHD_ERROR( ("Could not disable scanmac randomisation for 55500\n") );
        }
        else
        {
            WPRINT_WHD_INFO( ("Disabled scanmac randomisation for 55500\n") );
        }
    }

    /* Disabling ampdu hostreorder */
    if ( (wlan_chip_id == 55500) || (wlan_chip_id == 55530) || (wlan_chip_id == 55572) ||
         (wlan_chip_id == 55900) || (wlan_chip_id == 89530) )
    {
        CHECK_RETURN(whd_wifi_set_iovar_value(ifp, IOVAR_STR_AMPDU_HOST_REORDER, 0) );
    }

#if defined(COMPONENT_WLANSENSE)
    CHECK_RETURN(whd_wlansense_create_interface(whd_driver));
#endif /* defined(COMPONENT_WLANSENSE) */

#if defined(COMPONENT_CAT5) && !defined(WHD_DISABLE_PDS)
    /* Unlocking the syspm sleep lock, as WHD initialization part is done */
    whd_pds_unlock_sleep(whd_driver);
#endif /* defined(COMPONENT_CAT5) && !defined(WHD_DISABLE_PDS) */

    return WHD_SUCCESS;
}

/**
 * Turn off the Wi-Fi device
 *
 * - De-Initialises the required parts of the hardware platform
 *   i.e. pins for SDIO/SPI, interrupt, reset, power etc.
 *
 * - De-Initialises the WHD thread which arbitrates access
 *   to the SDIO/SPI bus
 *
 * @return WHD_SUCCESS if deinitialization is successful, error code otherwise
 */
whd_result_t whd_wifi_off(whd_interface_t ifp)
{
    whd_result_t retval;
    whd_driver_t whd_driver;

    CHECK_IFP_NULL(ifp);

    whd_driver = ifp->whd_driver;
    if (whd_driver->internal_info.whd_wlan_status.state == WLAN_OFF)
    {
        return WHD_SUCCESS;
    }

    /* Set wlc down before turning off the device */
    CHECK_RETURN(whd_wifi_set_ioctl_buffer(ifp, WLC_DOWN, NULL, 0) );
    whd_driver->internal_info.whd_wlan_status.state = WLAN_DOWN;

    /* Disable SDIO/SPI interrupt */
    whd_bus_irq_enable(whd_driver, WHD_FALSE);
    whd_thread_quit(whd_driver);

    whd_proto_detach(whd_driver);

    retval = whd_bus_deinit(whd_driver);
    if (retval != WHD_SUCCESS)
    {
        WPRINT_WHD_DEBUG( ("Error de-initializing bus\n") );
        return retval;
    }

#if defined(COMPONENT_CAT5) && !defined(WHD_DISABLE_PDS)
    cy_rtos_deinit_mutex(&whd_driver->sleep_mutex);
#endif

#ifdef COMPONENT_SDIO_HM
    cy_rtos_deinit_mutex(&whd_driver->whd_hm_tx_lock);
#endif

    whd_driver->internal_info.whd_wlan_status.state = WLAN_OFF;
    return WHD_SUCCESS;
}
