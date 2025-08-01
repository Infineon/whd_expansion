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
 *  Implements user functions for controlling the Wi-Fi system
 *
 *  This file provides end-user functions which allow actions such as scanning for
 *  Wi-Fi networks, joining Wi-Fi networks, getting the MAC address, etc
 *
 */

#include "whd_version.h"
#include "whd_chip_constants.h"
#ifndef PROTO_MSGBUF
#include "whd_cdc_bdc.h"
#endif /* PROTO_MSGBUF */
#include "whd_thread_internal.h"
#include "whd_debug.h"
#include "whd_utils.h"
#include "whd_wifi_api.h"
#include "whd_buffer_api.h"
#include "whd_wlioctl.h"
#include "whd_types.h"
#include "whd_types_int.h"
#include "whd_proto.h"
#ifdef CYCFG_ULP_SUPPORT_ENABLED
#include "cy_wcm.h"
#endif

#ifdef GCI_SECURE_ACCESS
#include "whd_hw.h"
#endif

/******************************************************
*                   Constants
******************************************************/
#define MAX_SUPPORTED_MCAST_ENTRIES   (10)
#define WLC_EVENT_MSG_LINK      (0x01)

#define JOIN_ASSOCIATED             (uint32_t)(1 << 0)
#define JOIN_AUTHENTICATED          (uint32_t)(1 << 1)
#define JOIN_LINK_READY             (uint32_t)(1 << 2)
#define JOIN_SECURITY_COMPLETE      (uint32_t)(1 << 3)
#define JOIN_SSID_SET               (uint32_t)(1 << 4)
#define JOIN_NO_NETWORKS            (uint32_t)(1 << 5)
#define JOIN_EAPOL_KEY_M1_TIMEOUT   (uint32_t)(1 << 6)
#define JOIN_EAPOL_KEY_M3_TIMEOUT   (uint32_t)(1 << 7)
#define JOIN_EAPOL_KEY_G1_TIMEOUT   (uint32_t)(1 << 8)
#define JOIN_EAPOL_KEY_FAILURE      (uint32_t)(1 << 9)

#define JOIN_SECURITY_FLAGS_MASK    (JOIN_SECURITY_COMPLETE | JOIN_EAPOL_KEY_M1_TIMEOUT | JOIN_EAPOL_KEY_M3_TIMEOUT | \
                                     JOIN_EAPOL_KEY_G1_TIMEOUT | JOIN_EAPOL_KEY_FAILURE)

#define DEFAULT_JOIN_ATTEMPT_TIMEOUT     (9000)   /* Overall join attempt timeout in milliseconds.(FW will do "full scan"[~2.8 seconds] + "psk-to-pmk"[2.x seconds] + "join"[5 seconds timer in FW]) */
#define DEFAULT_EAPOL_KEY_PACKET_TIMEOUT (2500)   /* Timeout when waiting for EAPOL key packet M1 or M3 in milliseconds.*/
                                                  /* Some APs may be slow to provide M1 and 1000 ms is not long enough for edge of cell. */
#ifndef DEFAULT_PM2_SLEEP_RET_TIME
#define DEFAULT_PM2_SLEEP_RET_TIME   (200)
#endif

#define PM2_SLEEP_RET_TIME_MIN       (10)     /* Minimum return-to-sleep in milliseconds */
#define PM2_SLEEP_RET_TIME_MAX       (2000)   /* Maximum return-to-sleep in milliseconds */
#define NULL_FRAMES_WITH_PM_SET_LIMIT (100)   /* NULL_FRAMES_WITH_PM_SET_LIMIT */
#define RSPEC_KBPS_MASK (0x7f)
#define RSPEC_500KBPS(rate) ( (rate) &  RSPEC_KBPS_MASK )
#define RSPEC_TO_KBPS(rate) (RSPEC_500KBPS( (rate) ) * (uint32_t)500)
#define UNSIGNED_CHAR_TO_CHAR(uch) ( (uch) & 0x7f )
#define ETHER_ISMULTI(ea) ( ( (const uint8_t *)(ea) )[0] & 1 )

#define KEY_MAX_LEN                   (64)  /* Maximum key length */
#define KEY_MIN_LEN                   (8)   /* Minimum key length */
#ifdef CYCFG_ULP_SUPPORT_ENABLED
#define MIN_DUMP_BUF_LEN              (2048)
#define MAX_DUMP_BUF_LEN              (4096)
#endif
#define BT_CTRL_REG_ADDR              (0x18000c7c)
#define HOST_CTRL_REG_ADDR            (0x18000d6c)
#define BT_BUF_REG_ADDR               (0x18000c78)

/* Default TCP Keepalive parameters.  */
#define TKO_DEFAULT_INTERVAL_SEC                 (1)
#define TKO_DEFAULT_RETRY_COUNT                  (3)
#define TKO_DEFAULT_RETRY_INTERVAL_SEC           (3)

/** Buffer length check for ulp statistics
 *
 *  @param buflen              buffer length
 *
 */
#define CHECK_BUFLEN(buflen,max,min) ((buflen) <= (max) && (buflen) >= (min))

/******************************************************
*             Local Structures
******************************************************/

#pragma pack(1)

typedef struct
{
    uint32_t entry_count;
    whd_mac_t macs[1];
} mcast_list_t;

typedef struct
{
    int32_t rssi;
    whd_mac_t macs;
} client_rssi_t;

typedef struct
{
    whd_sync_scan_result_t *aps;
    uint32_t count;
    uint32_t offset;
    cy_semaphore_t scan_semaphore;
} whd_scan_userdata_t;

#pragma pack()

/******************************************************
*             Static Variables
******************************************************/

/* LOOK: !!!When adding events below, please modify whd_event_to_string!!! */
const whd_event_num_t join_events[]  =
{
    WLC_E_SET_SSID, WLC_E_LINK, WLC_E_AUTH, WLC_E_DEAUTH_IND, WLC_E_DISASSOC_IND, WLC_E_PSK_SUP, WLC_E_CSA_COMPLETE_IND,
    WLC_E_NONE
};
static const whd_event_num_t scan_events[] = { WLC_E_ESCAN_RESULT, WLC_E_NONE };
static const whd_event_num_t auth_events[] =
{ WLC_E_EXT_AUTH_REQ, WLC_E_EXT_AUTH_FRAME_RX, WLC_E_NONE };
static const whd_event_num_t icmp_echo_req_events[] = {WLC_E_ICMP_ECHO_REQ, WLC_E_NONE};

static uint8_t icmp_echo_req_enable = 0;

static wl_icmp_echo_req_peer_config_t icmp_peer_config =
{
    .version = WL_ICMP_ECHO_REQ_VER,
    .length = sizeof(wl_icmp_echo_req_peer_config_t),
};

static wl_icmp_echo_req_peer_ip_t icmp_peer_ip =
{
    .version = WL_ICMP_ECHO_REQ_VER,
    .length = sizeof(wl_icmp_echo_req_peer_ip_t),
};

/* Values are in 100's of Kbit/sec (1 = 100Kbit/s). Arranged as:
 * [Bit index]
 *    [0] = 20Mhz only
 *       [0] = Long GI
 *       [1] = Short GI
 *    [1] = 40MHz support
 *       [0] = Long GI
 *       [1] = Short GI
 */
static const uint16_t mcs_data_rate_lookup_table[32][2][2] =
{
    [0] =
    {
        {   65, 72},
        {   135, 150}
    },
    [1] =
    {
        {   130, 144},
        {   270, 300}
    },
    [2] =
    {
        {   195, 217},
        {   405, 450}
    },
    [3] =
    {
        {   260, 289},
        {   540, 600}
    },
    [4] =
    {
        {   390, 433},
        {   810, 900}
    },
    [5] =
    {
        {   520, 578},
        {   1080, 1200}
    },
    [6] =
    {
        {   585, 650},
        {   1215, 1350}
    },
    [7] =
    {
        {   650, 722},
        {   1350, 1500}
    },
    [8] =
    {
        {   130, 144},
        {   270, 300}
    },
    [9] =
    {
        {   260, 289},
        {   540, 600}
    },
    [10] =
    {
        {   390, 433},
        {   810, 900}
    },
    [11] =
    {
        {   520, 578},
        {   1080, 1200}
    },
    [12] =
    {
        {   780, 867},
        {   1620, 1800}
    },
    [13] =
    {
        {   1040, 1156},
        {   2160, 2400}
    },
    [14] =
    {
        {   1170, 1300},
        {   2430, 2700}
    },
    [15] =
    {
        {   1300, 1444},
        {   2700, 3000}
    },
    [16] =
    {
        {   195, 217},
        {   405, 450}
    },
    [17] =
    {
        {   390, 433},
        {   810, 900}
    },
    [18] =
    {
        {   585, 650},
        {   1215, 1350}
    },
    [19] =
    {
        {   780, 867},
        {   1620, 1800}
    },
    [20] =
    {
        {   1170, 1300},
        {   2430, 2700}
    },
    [21] =
    {
        {   1560, 1733},
        {   3240, 3600}
    },
    [22] =
    {
        {   1755, 1950},
        {   3645, 4050}
    },
    [23] =
    {
        {   1950, 2167},
        {   4050, 4500}
    },
    [24] =
    {
        {   260, 288},
        {   540, 600}
    },
    [25] =
    {
        {   520, 576},
        {   1080, 1200}
    },
    [26] =
    {
        {   780, 868},
        {   1620, 1800}
    },
    [27] =
    {
        {   1040, 1156},
        {   2160, 2400}
    },
    [28] =
    {
        {   1560, 1732},
        {   3240, 3600}
    },
    [29] =
    {
        {   2080, 2312},
        {   4320, 4800}
    },
    [30] =
    {
        {   2340, 2600},
        {   4860, 5400}
    },
    [31] =
    {
        {   2600, 2888},
        {   5400, 6000}
    },
};


/******************************************************
*             Static Function prototypes
******************************************************/
static void *whd_wifi_join_events_handler(whd_interface_t ifp, const whd_event_header_t *event_header,
                                          const uint8_t *event_data, void *handler_user_data);
static void *whd_wifi_scan_events_handler(whd_interface_t ifp, const whd_event_header_t *event_header,
                                          const uint8_t *event_data,
                                          void *handler_user_data);
static whd_result_t whd_wifi_prepare_join(whd_interface_t ifp,
                                      whd_security_t security,
                                      const uint8_t *security_key,
                                      uint8_t key_length,
                                      cy_semaphore_t *semaphore);
static whd_result_t whd_wifi_check_join_status(whd_interface_t ifp);
static void     whd_wifi_active_join_deinit(whd_interface_t ifp, cy_semaphore_t *stack_semaphore,
                                            whd_result_t result);
static whd_result_t whd_wifi_active_join_init(whd_interface_t ifp, whd_security_t auth_type,
                                          const uint8_t *security_key, uint8_t key_length,
                                          cy_semaphore_t *semaphore);
static whd_result_t whd_tko_autoenab(whd_interface_t ifp, whd_bool_t enable);

/** Sets the current EAPOL key timeout for the given interface
 *
 * @param interface         : the interface for which we want to set the EAPOL key timeout
 *        eapol_key_timeout : EAPOL key timeout value
 *
 * @return  WHD_SUCCESS : if success
 *          Error code    : error code to indicate the type of error
 */
static whd_result_t whd_wifi_set_supplicant_key_timeout(whd_interface_t ifp, int32_t eapol_key_timeout);

/******************************************************
*             Function definitions
******************************************************/
whd_result_t whd_get_bt_info(whd_driver_t whd_driver, whd_bt_info_t bt_info);

inline wl_chanspec_t whd_channel_to_wl_band(whd_driver_t whd_driver, uint32_t channel)
{
    return ( ( (channel) <= CH_MAX_2G_CHANNEL ) ? (uint16_t)GET_C_VAR(whd_driver, CHANSPEC_BAND_2G) :
             (uint16_t)GET_C_VAR(whd_driver, CHANSPEC_BAND_5G) );
}

whd_result_t whd_wifi_set_up(whd_interface_t ifp)
{
    whd_mac_t mac;
    char version[200];
    whd_driver_t whd_driver;

    CHECK_IFP_NULL(ifp);

    whd_driver = ifp->whd_driver;
    if (whd_driver->internal_info.whd_wlan_status.state == WLAN_UP)
    {
        WPRINT_WHD_INFO( ("whd_wifi_set_up: already up.\n") );
        return WHD_SUCCESS;
    }

    /* Send UP command */
    CHECK_RETURN(whd_wifi_set_ioctl_buffer(ifp, WLC_UP, NULL, 0) );

    if (whd_wifi_get_mac_address(ifp, &mac) == WHD_SUCCESS)
    {
        WPRINT_MACRO( ("WLAN MAC Address : %02X:%02X:%02X:%02X:%02X:%02X\n", mac.octet[0], mac.octet[1], mac.octet[2],
                       mac.octet[3], mac.octet[4], mac.octet[5]) );
    }

    if (whd_wifi_get_wifi_version(ifp, version, sizeof(version) ) == WHD_SUCCESS)
    {
        WPRINT_MACRO( ("WLAN Firmware    : %s", version) );
    }

    /* minimize bootloader usage and start time from UART output */
    if (whd_wifi_get_clm_version(ifp, version, sizeof(version) ) == WHD_SUCCESS)
    {
        WPRINT_MACRO( ("WLAN CLM         : %s\n", version) );
    }

    WPRINT_MACRO( ("WHD VERSION      : " WHD_VERSION) );
    WPRINT_MACRO( (" : " WHD_BRANCH) );
#if defined(__ARMCC_VERSION)
    WPRINT_MACRO( (" : ARM CLANG %u", __ARMCC_VERSION) );
#elif defined(__ICCARM__)
    WPRINT_MACRO( (" : IAR %u", __VER__) );
#elif defined(__GNUC__)
    WPRINT_MACRO( (" : GCC %u.%u", __GNUC__, __GNUC_MINOR__) );
#else
    WPRINT_MACRO( (" : UNKNOWN CC") );
#endif
    WPRINT_MACRO( (" : " WHD_DATE "\n") );

    /* Update wlan status */
    whd_driver->internal_info.whd_wlan_status.state = WLAN_UP;

    return WHD_SUCCESS;
}

whd_result_t whd_wifi_set_down(whd_interface_t ifp)
{
    whd_driver_t whd_driver = ifp->whd_driver;

    if (whd_driver->internal_info.whd_wlan_status.state != WLAN_UP)
    {
        WPRINT_WHD_INFO( ("whd_wifi_set_down: already down.\n") );
        return WHD_INTERFACE_NOT_UP;
    }

    /* Send DOWN command */
    CHECK_RETURN(whd_wifi_set_ioctl_buffer(ifp, WLC_DOWN, NULL, 0) );

    /* Update wlan status */
    whd_driver->internal_info.whd_wlan_status.state = WLAN_DOWN;

    return WHD_SUCCESS;
}

whd_result_t whd_get_bt_info(whd_driver_t whd_driver, whd_bt_info_t bt_info)
{
    whd_result_t result;
    whd_interface_t ifp;
    uint32_t addr = 0;

    ifp = whd_get_primary_interface(whd_driver);

    CHECK_IFP_NULL(ifp);

    memset(bt_info, 0, sizeof(struct whd_bt_info) );
    bt_info->bt_ctrl_reg_addr = BT_CTRL_REG_ADDR;
    bt_info->host_ctrl_reg_addr = HOST_CTRL_REG_ADDR;
    bt_info->bt_buf_reg_addr = BT_BUF_REG_ADDR;
    result = whd_wifi_get_iovar_buffer(ifp, IOVAR_STR_BTADDR, (uint8_t *)&addr, sizeof(uint32_t) );
    if (result == WHD_SUCCESS)
    {
        bt_info->wlan_buf_addr = addr;
    }
    return WHD_SUCCESS;
}

whd_result_t whd_wifi_set_chanspec(whd_interface_t ifp, wl_chanspec_t chanspec)
{
    whd_buffer_t buffer;
    uint32_t *data;
    whd_driver_t whd_driver;

    CHECK_IFP_NULL(ifp);

    whd_driver = ifp->whd_driver;

    CHECK_DRIVER_NULL(whd_driver);

    /* Map P2P interface to either STA or AP interface depending if it's running as group owner or client */
    if (ifp->role == WHD_P2P_ROLE)
    {
        if (whd_driver->internal_info.whd_wifi_p2p_go_is_up == WHD_TRUE)
        {
            ifp->role = WHD_AP_ROLE;
        }
        else
        {
            ifp->role = WHD_STA_ROLE;
        }
    }
    WPRINT_WHD_INFO( ("whd_wifi_set_chanspec: ifp->role(%d) chanspec(0x%x)\n", ifp->role, chanspec) );

    switch (ifp->role)
    {
        case WHD_STA_ROLE:
        case WHD_AP_ROLE:
            data = (uint32_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer, (uint16_t)sizeof(*data),
                                                          IOVAR_STR_CHANSPEC);
            CHECK_IOCTL_BUFFER(data);
            *data = htod32( (uint32_t)chanspec );
            CHECK_RETURN(whd_proto_set_iovar(ifp, buffer, 0) );
            break;
        case WHD_P2P_ROLE:
        case WHD_INVALID_ROLE:
        default:
            whd_assert("Bad interface", 0 != 0);
            return WHD_UNKNOWN_INTERFACE;
    }
    return WHD_SUCCESS;
}

whd_result_t whd_wifi_set_channel(whd_interface_t ifp, uint32_t channel)
{
    whd_buffer_t buffer;
    uint32_t *data;
    wl_chan_switch_t *chan_switch;
    whd_driver_t whd_driver;

    CHECK_IFP_NULL(ifp);

    whd_driver = ifp->whd_driver;

    CHECK_DRIVER_NULL(whd_driver);

    /* Map P2P interface to either STA or AP interface depending if it's running as group owner or client */
    if (ifp->role == WHD_P2P_ROLE)
    {
        if (whd_driver->internal_info.whd_wifi_p2p_go_is_up == WHD_TRUE)
        {
            ifp->role = WHD_AP_ROLE;
        }
        else
        {
            ifp->role = WHD_STA_ROLE;
        }
    }

    switch (ifp->role)
    {
        case WHD_STA_ROLE:
            data = (uint32_t *)whd_proto_get_ioctl_buffer(whd_driver, &buffer, sizeof(uint32_t) );
            CHECK_IOCTL_BUFFER(data);
            *data = htod32(channel);
            CHECK_RETURN(whd_proto_get_ioctl(ifp, WLC_SET_CHANNEL, buffer, NULL) );

            break;

        case WHD_AP_ROLE:
            chan_switch = (wl_chan_switch_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer, sizeof(wl_chan_switch_t),
                                                                         IOVAR_STR_CSA);
            CHECK_IOCTL_BUFFER(chan_switch);
            chan_switch->chspec =
                ( wl_chanspec_t )(GET_C_VAR(whd_driver,
                                            CHANSPEC_BW_20) | GET_C_VAR(whd_driver, CHANSPEC_CTL_SB_NONE) | channel);
            chan_switch->chspec |= whd_channel_to_wl_band(whd_driver, channel);
            chan_switch->chspec = htod16(chan_switch->chspec);
            chan_switch->count = 1;
            chan_switch->mode = 1;
            chan_switch->reg = 0;
            CHECK_RETURN(whd_proto_set_iovar(ifp, buffer, 0) );
            break;
        case WHD_P2P_ROLE:
        case WHD_INVALID_ROLE:
        default:
            whd_assert("Bad interface", 0 != 0);
            return WHD_UNKNOWN_INTERFACE;
    }

    return WHD_SUCCESS;
}

whd_result_t whd_wifi_get_channel(whd_interface_t ifp, uint32_t *channel)
{
    whd_buffer_t buffer;
    whd_buffer_t response;
    channel_info_t *channel_info;
    whd_driver_t whd_driver;

    CHECK_IFP_NULL(ifp);

    if (channel == NULL)
        return WHD_BADARG;

    whd_driver = ifp->whd_driver;

    CHECK_DRIVER_NULL(whd_driver);

    CHECK_IOCTL_BUFFER(whd_proto_get_ioctl_buffer(whd_driver, &buffer, sizeof(channel_info_t) ) );

    CHECK_RETURN(whd_proto_get_ioctl(ifp, WLC_GET_CHANNEL, buffer, &response) );

    channel_info = (channel_info_t *)whd_buffer_get_current_piece_data_pointer(whd_driver, response);
    CHECK_PACKET_NULL(channel_info, WHD_NO_REGISTER_FUNCTION_POINTER);
    *channel = (uint32_t)channel_info->hw_channel;
    CHECK_RETURN(whd_buffer_release(whd_driver, response, WHD_NETWORK_RX) );
    return WHD_SUCCESS;
}

whd_result_t whd_wifi_enable_supplicant(whd_interface_t ifp, whd_security_t auth_type)
{
    whd_buffer_t buffer;
    uint32_t *data;
    uint32_t bss_index = 0;
    whd_driver_t whd_driver;

    CHECK_IFP_NULL(ifp);

    whd_driver = ifp->whd_driver;

    CHECK_DRIVER_NULL(whd_driver);

    /* Map the interface to a BSS index */
    bss_index = ifp->bsscfgidx;

    /* Set supplicant variable - mfg app doesn't support these iovars, so don't care if return fails */
    data = whd_proto_get_iovar_buffer(whd_driver, &buffer, (uint16_t)8, "bsscfg:" IOVAR_STR_SUP_WPA);
    CHECK_IOCTL_BUFFER(data);
    data[0] = bss_index;
    data[1] = (uint32_t)( ( ( (auth_type & WPA_SECURITY)  != 0 ) ||
                            ( (auth_type & WPA2_SECURITY) != 0 ) ||
                            (auth_type & WPA3_SECURITY) != 0 ) ? 1 : 0 );
    (void)whd_proto_set_iovar(ifp, buffer, 0);

    return WHD_SUCCESS;
}

whd_result_t whd_wifi_set_supplicant_key_timeout(whd_interface_t ifp, int32_t eapol_key_timeout)
{
    whd_buffer_t buffer;
    int32_t *data;
    uint32_t bss_index = 0;
    whd_driver_t whd_driver = (whd_driver_t)ifp->whd_driver;

    /* Map the interface to a BSS index */
    bss_index = ifp->bsscfgidx;

    data = whd_proto_get_iovar_buffer(whd_driver, &buffer, (uint16_t)8, "bsscfg:" IOVAR_STR_SUP_WPA_TMO);
    CHECK_IOCTL_BUFFER(data);
#ifndef PROTO_MSGBUF
    data[0] = (int32_t)bss_index;
    data[1] = eapol_key_timeout;
#else
    memcpy(data, &bss_index, sizeof(uint32_t));
    memcpy(data + sizeof(bss_index), &eapol_key_timeout, sizeof(uint32_t) );
#endif
    CHECK_RETURN(whd_proto_set_iovar(ifp, buffer, 0) );

    return WHD_SUCCESS;
}

whd_result_t whd_wifi_set_passphrase(whd_interface_t ifp, const uint8_t *security_key, uint8_t key_length)
{
    whd_buffer_t buffer;
    whd_driver_t whd_driver;
    wsec_pmk_t *psk;

    if (!ifp || !security_key || (key_length < KEY_MIN_LEN) || (key_length > KEY_MAX_LEN) )
    {
        WPRINT_WHD_ERROR( ("Invalid param in func %s at line %d \n",
                           __func__, __LINE__) );
        return WHD_WLAN_BADARG;
    }

    whd_driver = ifp->whd_driver;
    CHECK_DRIVER_NULL(whd_driver);

    psk = (wsec_pmk_t *)whd_proto_get_ioctl_buffer(whd_driver, &buffer, sizeof(wsec_pmk_t) );
    CHECK_IOCTL_BUFFER(psk);

    memset(psk, 0, sizeof(wsec_pmk_t) );
    memcpy(psk->key, security_key, key_length);
    psk->key_len = htod16(key_length);
    psk->flags = htod16( (uint16_t)WSEC_PASSPHRASE );

    /* Delay required to allow radio firmware to be ready to receive PMK and avoid intermittent failure */
    CHECK_RETURN(cy_rtos_delay_milliseconds(1) );

    CHECK_RETURN(whd_proto_set_ioctl(ifp, WLC_SET_WSEC_PMK, buffer, 0) );

    return WHD_SUCCESS;
}

whd_result_t whd_wifi_sae_password(whd_interface_t ifp, const uint8_t *security_key, uint8_t key_length)
{
    whd_buffer_t buffer;
    whd_driver_t whd_driver;
    wsec_sae_password_t *sae_password;

    if (!ifp || !security_key || (key_length == 0) || (key_length > WSEC_MAX_SAE_PASSWORD_LEN) )
    {
        WPRINT_WHD_ERROR( ("Invalid param in func %s at line %d \n",
                           __func__, __LINE__) );
        return WHD_WLAN_BADARG;
    }

    whd_driver = ifp->whd_driver;
    CHECK_DRIVER_NULL(whd_driver);

    sae_password = (wsec_sae_password_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer,
                                                                     sizeof(wsec_sae_password_t),
                                                                     IOVAR_STR_SAE_PASSWORD);
    CHECK_IOCTL_BUFFER(sae_password);
    memset(sae_password, 0, sizeof(wsec_sae_password_t) );
    memcpy(sae_password->password, security_key, key_length);
    sae_password->password_len = htod16(key_length);
    /* Delay required to allow radio firmware to be ready to receive PMK and avoid intermittent failure */
    cy_rtos_delay_milliseconds(1);
    CHECK_RETURN(whd_proto_set_iovar(ifp, buffer, 0) );

    return WHD_SUCCESS;
}

whd_result_t whd_wifi_offload_config(whd_interface_t ifp, uint32_t ol_feat, uint32_t reset)
{
    whd_buffer_t buffer;
    whd_driver_t whd_driver;
    wl_ol_cfg_v1_t *ol_cfg;
    uint32_t ol_feat_skip = ~ol_feat;

    whd_driver = ifp->whd_driver;
    CHECK_DRIVER_NULL(whd_driver);

    ol_cfg = (wl_ol_cfg_v1_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer,
                                                                   sizeof(wl_ol_cfg_v1_t),
                                                                   IOVAR_STR_OFFLOAD_CONFIG);
    CHECK_IOCTL_BUFFER(ol_cfg);
    memset(ol_cfg, 0, sizeof(wl_ol_cfg_v1_t) );

    ol_cfg->ver = WL_OL_CFG_VER_1;
    ol_cfg->len = sizeof(wl_ol_cfg_v1_t);
    ol_cfg->id = WL_OL_CFG_ID_PROF;
    ol_cfg->offload_skip = ol_feat_skip;

    if (reset) {
        ol_cfg->u.ol_profile.reset = WHD_TRUE;
        ol_cfg->u.ol_profile.type = -1;
    } else {
        ol_cfg->u.ol_profile.reset = WHD_FALSE;
        ol_cfg->u.ol_profile.type = WL_OL_PROF_TYPE_LOW_PWR;
    }
    CHECK_RETURN(whd_proto_set_iovar(ifp, buffer, 0) );

#ifdef CYCFG_ULP_SUPPORT_ENABLED
        /* Later this APIs can be moved to other place, if required */
        CHECK_RETURN(whd_configure_tko_offload(ifp, WHD_TRUE));
#endif

    return WHD_SUCCESS;
}

whd_result_t whd_wifi_offload_ipv4_update(whd_interface_t ifp, uint32_t ol_feat, uint32_t ipv4_addr, whd_bool_t is_add)
{
	whd_buffer_t buffer;
	whd_driver_t whd_driver;
	wl_ol_cfg_v1_t *ol_cfg;
	whd_ipv4_addr_t addr;
	uint8_t i;
	uint8_t j=0;
	uint32_t ol_feat_skip = ~ol_feat;

	whd_driver = ifp->whd_driver;
	CHECK_DRIVER_NULL(whd_driver);

	for (i=0; i<IPV4_ADDR_LEN; i++)
	{
		addr.addr[i] = (ipv4_addr >> j) & 0xff;
		j = j+8;
	}

	ol_cfg = (wl_ol_cfg_v1_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer,
                                                                   sizeof(wl_ol_cfg_v1_t),
                                                                   IOVAR_STR_OFFLOAD_CONFIG);
	CHECK_IOCTL_BUFFER(ol_cfg);
	memset(ol_cfg, 0, sizeof(wl_ol_cfg_v1_t) );

	ol_cfg->ver = WL_OL_CFG_VER_1;
	ol_cfg->len = sizeof(wl_ol_cfg_v1_t);
	ol_cfg->id = WL_OL_CFG_ID_INET_V4;
	ol_cfg->offload_skip = ol_feat_skip;

	ol_cfg->u.ol_inet_v4.del = (whd_bool_t) !is_add;
	memcpy(&ol_cfg->u.ol_inet_v4.host_ipv4, &addr, sizeof(addr));

	CHECK_RETURN(whd_proto_set_iovar(ifp, buffer, 0) );
	return WHD_SUCCESS;
}

whd_result_t whd_wifi_offload_ipv6_update(whd_interface_t ifp, uint32_t ol_feat, uint32_t *ipv6_addr, uint8_t type, whd_bool_t is_add)
{
	whd_buffer_t buffer;
	whd_driver_t whd_driver;
	wl_ol_cfg_v1_t *ol_cfg;
	whd_ipv6_addr_t addr;
	uint8_t i, j=0;
	uint8_t k=0;
	uint32_t ol_feat_skip = ~ol_feat;

	whd_driver = ifp->whd_driver;
	CHECK_DRIVER_NULL(whd_driver);

	for(i=0; i<IPV6_ADDR_LEN; i++)
	{
		addr.addr[i] = (ipv6_addr[j] >> k) & 0xff;
		k = k+8;
		if(k == 32)
		{
			j++;
			k=0;
		}
	}

	ol_cfg = (wl_ol_cfg_v1_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer,
                                                                   sizeof(wl_ol_cfg_v1_t),
                                                                   IOVAR_STR_OFFLOAD_CONFIG);
	CHECK_IOCTL_BUFFER(ol_cfg);
	memset(ol_cfg, 0, sizeof(wl_ol_cfg_v1_t) );

	ol_cfg->ver = WL_OL_CFG_VER_1;
	ol_cfg->len = sizeof(wl_ol_cfg_v1_t);
	ol_cfg->id = WL_OL_CFG_ID_INET_V6;
	ol_cfg->offload_skip = ol_feat_skip;

	ol_cfg->u.ol_inet_v6.del = (whd_bool_t) !is_add;
	ol_cfg->u.ol_inet_v6.type = type;
	memcpy(&ol_cfg->u.ol_inet_v6.host_ipv6, &addr, sizeof(addr));

	CHECK_RETURN(whd_proto_set_iovar(ifp, buffer, 0) );
	return WHD_SUCCESS;
}

whd_result_t whd_wifi_offload_enable(whd_interface_t ifp, uint32_t ol_feat, uint32_t enable)
{
	whd_buffer_t buffer;
	whd_driver_t whd_driver;
	wl_ol_cfg_v1_t *ol_cfg;
	uint32_t ol_feat_skip = ~ol_feat;

	whd_driver = ifp->whd_driver;
	CHECK_DRIVER_NULL(whd_driver);

	ol_cfg = (wl_ol_cfg_v1_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer,
                                                                   sizeof(wl_ol_cfg_v1_t),
                                                                   IOVAR_STR_OFFLOAD_CONFIG);
	CHECK_IOCTL_BUFFER(ol_cfg);
	memset(ol_cfg, 0, sizeof(wl_ol_cfg_v1_t) );

	ol_cfg->ver = WL_OL_CFG_VER_1;
	ol_cfg->len = sizeof(wl_ol_cfg_v1_t);
	ol_cfg->id = WL_OL_CFG_ID_ACTIVATE;
	ol_cfg->offload_skip = ol_feat_skip;

	if (enable) {
		ol_cfg->u.ol_activate.enable = WHD_TRUE;
	} else {
		ol_cfg->u.ol_activate.enable = WHD_FALSE;
	}

	CHECK_RETURN(whd_proto_set_iovar(ifp, buffer, 0) );
	return WHD_SUCCESS;
}

whd_result_t whd_configure_wowl(whd_interface_t ifp, uint32_t set_wowl)
{
    uint32_t get_wowl = 0;

    CHECK_RETURN(whd_wifi_get_iovar_value(ifp, IOVAR_STR_WOWL, &get_wowl));

    set_wowl = ( set_wowl | get_wowl );

    CHECK_RETURN(whd_wifi_set_iovar_value(ifp, IOVAR_STR_WOWL, set_wowl));
    CHECK_RETURN(whd_wifi_set_iovar_value(ifp, IOVAR_STR_WOWL_OS, set_wowl));

    return WHD_SUCCESS;
}

whd_result_t whd_wifi_keepalive_config(whd_interface_t ifp, whd_keep_alive_t *packet, uint8_t flag )
{
    whd_buffer_t buffer = NULL;
    whd_driver_t whd_driver;
    wl_keep_alive_pkt_t *keepalive_cfg;
    uint32_t buffer_length;
    whd_driver = ifp->whd_driver;
    CHECK_DRIVER_NULL(whd_driver);

    if (flag == WHD_KEEPALIVE_NULL )
    {
        keepalive_cfg = (wl_keep_alive_pkt_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer,sizeof(wl_keep_alive_pkt_t),IOVAR_STR_KEEPALIVE_CONFIG);

        CHECK_IOCTL_BUFFER(keepalive_cfg);
        memset(keepalive_cfg, 0, sizeof(wl_keep_alive_pkt_t));

        keepalive_cfg->period_msec = packet->period_msec;
    }
    else if (flag == WHD_KEEPALIVE_NAT )
    {
        buffer_length =(uint32_t)( (2 * (uint32_t)packet->len_bytes) + WL_KEEP_ALIVE_FIXED_LEN  );
        keepalive_cfg = (wl_keep_alive_pkt_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer,(uint16_t)buffer_length,IOVAR_STR_KEEPALIVE_CONFIG);

        CHECK_IOCTL_BUFFER(keepalive_cfg);
        keepalive_cfg->period_msec = packet->period_msec;
        keepalive_cfg->len_bytes = packet->len_bytes;
        memcpy(keepalive_cfg->data,packet->data,packet->len_bytes);
    }
    CHECK_RETURN(whd_proto_set_iovar(ifp, buffer, 0));
    return WHD_SUCCESS;
}

whd_result_t whd_configure_tko_filter(whd_interface_t ifp,whd_tko_auto_filter_t * whd_filter, uint8_t filter_flag)
{
    uint32_t result = 0;

    result =  whd_tko_toggle(ifp, WHD_FALSE);
    if (result != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR(("Set whd_tko_param returned failure\n"));
    }
    result =  whd_tko_autoenab(ifp, WHD_TRUE);
    if (result != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR(("Set whd_tko_autoenab returned failure\n"));
    }
    result = whd_tko_filter(ifp,whd_filter,filter_flag);
    if (result != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR(("Set whd_tko_filter returned failure\n"));
    }
    result =  whd_tko_toggle(ifp, WHD_TRUE);
    if (result != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR(("Set whd_tko_param returned failure\n"));
    }
    return result;
}

whd_result_t whd_configure_tko_offload(whd_interface_t ifp, whd_bool_t enable)
{
    uint32_t result = 0;
    result =  whd_tko_autoenab(ifp, enable);
    if (result != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR(("Set whd_tko_autoenab returned failure\n"));
    }
    result =  whd_tko_toggle(ifp, enable);

    if (result != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR(("Set whd_tko_param returned failure\n"));
    }
    return result;
}

whd_result_t whd_wifi_enable_sup_set_passphrase(whd_interface_t ifp, const uint8_t *security_key_psk, uint8_t psk_length,
                                            whd_security_t auth_type)
{
    whd_buffer_t buffer;
    uint32_t *data;
    uint32_t bss_index = 0;
    whd_driver_t whd_driver;

    CHECK_IFP_NULL(ifp);

    whd_driver = ifp->whd_driver;

    CHECK_DRIVER_NULL(whd_driver);

    if ( (psk_length > (uint8_t)WSEC_MAX_PSK_LEN) ||
         (psk_length < (uint8_t)WSEC_MIN_PSK_LEN) )
    {
        return WHD_INVALID_KEY;
    }

    /* Map the interface to a BSS index */
    bss_index = ifp->bsscfgidx;

    /* Set supplicant variable - mfg app doesn't support these iovars, so don't care if return fails */
    data = whd_proto_get_iovar_buffer(whd_driver, &buffer, (uint16_t)8, "bsscfg:" IOVAR_STR_SUP_WPA);
    CHECK_IOCTL_BUFFER(data);
    data[0] = bss_index;
    data[1] = (uint32_t)( ( ( (auth_type & WPA_SECURITY)  != 0 ) ||
                            ( (auth_type & WPA2_SECURITY) != 0 ) ||
                            (auth_type & WPA3_SECURITY) != 0 ) ? 1 : 0 );
    (void)whd_proto_set_iovar(ifp, buffer, 0);

    CHECK_RETURN(whd_wifi_set_passphrase(ifp, security_key_psk, psk_length) );

    return WHD_SUCCESS;
}

whd_result_t whd_wifi_set_pmk(whd_interface_t ifp, const uint8_t *security_key, uint8_t key_length)
{
    whd_buffer_t buffer;
    whd_driver_t whd_driver;
    wsec_pmk_t *pmk;
    uint32_t i;

    if (!ifp || !security_key || ( (key_length != WSEC_PMK_LEN) && (key_length != WSEC_PMK_WPA3_ENT_192_LEN) ) )
    {
        WPRINT_WHD_ERROR( ("Invalid param in func %s at line %d key_length: %u\n",
                           __func__, __LINE__, key_length) );
        return WHD_WLAN_BADARG;
    }

    whd_driver = ifp->whd_driver;
    CHECK_DRIVER_NULL(whd_driver);

    pmk = (wsec_pmk_t *)whd_proto_get_ioctl_buffer(whd_driver, &buffer, sizeof(wsec_pmk_t) );
    CHECK_IOCTL_BUFFER(pmk);

    memset(pmk, 0, sizeof(wsec_pmk_t) );

    if (key_length == WSEC_PMK_WPA3_ENT_192_LEN)
    {
        memcpy(pmk->key, security_key, key_length);
        pmk->key_len = htod16(key_length);
    }
    else
    {
        for (i = 0; i < key_length; i++)
        {
            snprintf( (char *)&pmk->key[2 * i], 3, "%02x", security_key[i] );
        }
        pmk->key_len = htod16(key_length << 1);
        pmk->flags = htod16( (uint16_t)WSEC_PASSPHRASE );
    }

    /* Delay required to allow radio firmware to be ready to receive PMK and avoid intermittent failure */
    CHECK_RETURN(cy_rtos_delay_milliseconds(1) );

    CHECK_RETURN(whd_proto_set_ioctl(ifp, WLC_SET_WSEC_PMK, buffer, 0) );

    return WHD_SUCCESS;
}

whd_result_t whd_wifi_set_pmksa(whd_interface_t ifp, const pmkid_t *pmkid)
{
    whd_buffer_t buffer;
    whd_buffer_t response;
    uint16_t cnt;
    pmkid_list_t *orig_pmkid_list;
    pmkid_list_t *new_pmkid_list;
    whd_driver_t whd_driver;

    if (!ifp || !pmkid)
    {
        WPRINT_WHD_ERROR( ("Invalid param in func %s at line %d \n",
                           __func__, __LINE__) );
        return WHD_WLAN_BADARG;
    }

    whd_driver = ifp->whd_driver;

    CHECK_DRIVER_NULL(whd_driver);

    /* Get the current pmkid_list list */
    CHECK_IOCTL_BUFFER(whd_proto_get_iovar_buffer(whd_driver, &buffer,
                                                  sizeof(uint32_t) + MAXPMKID *
                                                  sizeof(pmkid_t), IOVAR_STR_PMKID_INFO) );
    CHECK_RETURN(whd_proto_get_iovar(ifp, buffer, &response) );

    /* Verify address is not currently registered */
    orig_pmkid_list = (pmkid_list_t *)whd_buffer_get_current_piece_data_pointer(whd_driver, response);
    CHECK_PACKET_NULL(orig_pmkid_list, WHD_NO_REGISTER_FUNCTION_POINTER);
    orig_pmkid_list->npmkid = dtoh32(orig_pmkid_list->npmkid);
    for (cnt = 0; cnt < orig_pmkid_list->npmkid; ++cnt)
    {
        /* Check if any address matches */
        if (!memcmp(pmkid->BSSID.octet, orig_pmkid_list->pmkid[cnt].BSSID.octet, sizeof(whd_mac_t) ) )
        {
            break;
        }
    }

    if (cnt == MAXPMKID)
    {
        CHECK_RETURN(whd_buffer_release(whd_driver, response, WHD_NETWORK_RX) );
        WPRINT_WHD_ERROR( ("Too manay PMKSA entrie cached %" PRIu32 "\n", orig_pmkid_list->npmkid) );
        return WHD_WLAN_NORESOURCE;
    }

    /* Add Extra Space for New PMKID and write the new multicast list */
    if (cnt == orig_pmkid_list->npmkid)
    {
        new_pmkid_list = (pmkid_list_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer,
                                                                    ( uint16_t )(sizeof(uint32_t) +
                                                                                 (orig_pmkid_list->npmkid + 1) *
                                                                                 sizeof(pmkid_t) ),
                                                                    IOVAR_STR_PMKID_INFO);
        CHECK_IOCTL_BUFFER(new_pmkid_list);
        new_pmkid_list->npmkid = orig_pmkid_list->npmkid + 1;
        memcpy(new_pmkid_list->pmkid, orig_pmkid_list->pmkid, orig_pmkid_list->npmkid * sizeof(pmkid_t) );
        CHECK_RETURN(whd_buffer_release(whd_driver, response, WHD_NETWORK_RX) );
        memcpy(&new_pmkid_list->pmkid[new_pmkid_list->npmkid - 1], pmkid, sizeof(pmkid_t) );
        new_pmkid_list->npmkid = htod32(new_pmkid_list->npmkid);
    }
    else
    /* Replace Old PMKID for New PMKID under same BSSID and write the new multicast list */
    {
        new_pmkid_list = (pmkid_list_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer,
                                                                    ( uint16_t )(sizeof(uint32_t) +
                                                                                 (orig_pmkid_list->npmkid) *
                                                                                 sizeof(pmkid_t) ),
                                                                    IOVAR_STR_PMKID_INFO);
        CHECK_IOCTL_BUFFER(new_pmkid_list);
        new_pmkid_list->npmkid = orig_pmkid_list->npmkid;
        memcpy(new_pmkid_list->pmkid, orig_pmkid_list->pmkid, orig_pmkid_list->npmkid * sizeof(pmkid_t) );
        CHECK_RETURN(whd_buffer_release(whd_driver, response, WHD_NETWORK_RX) );
        memcpy(&new_pmkid_list->pmkid[cnt], pmkid, sizeof(pmkid_t) );
        new_pmkid_list->npmkid = htod32(new_pmkid_list->npmkid);
    }
    RETURN_WITH_ASSERT(whd_proto_set_iovar(ifp, buffer, NULL) );
}

whd_result_t whd_wifi_pmkid_clear(whd_interface_t ifp)
{
    whd_buffer_t buffer;
    whd_driver_t whd_driver = (whd_driver_t)ifp->whd_driver;

    CHECK_IOCTL_BUFFER(whd_proto_get_iovar_buffer(whd_driver, &buffer,
                                                  0, IOVAR_STR_PMKID_CLEAR) );
    CHECK_RETURN(whd_proto_set_iovar(ifp, buffer, 0) );

    return WHD_SUCCESS;
}

whd_result_t whd_wifi_set_roam_time_threshold(whd_interface_t ifp, uint32_t roam_time_threshold)
{
    if (!ifp || !roam_time_threshold)
    {
        WPRINT_WHD_ERROR( ("Invalid param in func %s at line %d \n",
                           __func__, __LINE__) );
        return WHD_WLAN_BADARG;
    }

    return whd_wifi_set_iovar_value(ifp, IOVAR_STR_ROAM_TIME_THRESH, roam_time_threshold);
}

whd_result_t whd_wifi_get_roam_time_threshold(whd_interface_t ifp, uint32_t *roam_time_threshold)
{
    CHECK_IFP_NULL(ifp);

    return whd_wifi_get_iovar_value(ifp, IOVAR_STR_ROAM_TIME_THRESH, roam_time_threshold);
}

whd_result_t whd_wifi_get_rssi(whd_interface_t ifp, int32_t *rssi)
{
    CHECK_IFP_NULL(ifp);

    if (rssi == NULL)
        return WHD_BADARG;
    if (ifp->role == WHD_STA_ROLE)
    {
        return whd_wifi_get_ioctl_buffer(ifp, WLC_GET_RSSI, (uint8_t *)rssi, sizeof(*rssi) );
    }
    return WHD_BADARG;
}

whd_result_t whd_wifi_get_ap_client_rssi(whd_interface_t ifp, int32_t *rssi, const whd_mac_t *client_mac)
{
    whd_buffer_t buffer;
    whd_buffer_t response;
    uint8_t *data = NULL;
    client_rssi_t *client_rssi;
    whd_driver_t whd_driver = ifp->whd_driver;

    /* WLAN expects buffer size to be 4-byte aligned */
    client_rssi =
        (client_rssi_t *)whd_proto_get_ioctl_buffer(whd_driver, &buffer, ROUND_UP(sizeof(client_rssi_t),
                                                                                  sizeof(uint32_t) ) );
    CHECK_IOCTL_BUFFER(client_rssi);

    memcpy(&client_rssi->macs, client_mac, sizeof(*client_mac) );
    client_rssi->rssi = 0;

    CHECK_RETURN_UNSUPPORTED_OK(whd_proto_get_ioctl(ifp, WLC_GET_RSSI, buffer, &response) );
    data = whd_buffer_get_current_piece_data_pointer(whd_driver, response);
    CHECK_PACKET_NULL(data, WHD_NO_REGISTER_FUNCTION_POINTER);
    memcpy(rssi, data, sizeof(int32_t) );
    CHECK_RETURN(whd_buffer_release(whd_driver, response, WHD_NETWORK_RX) );

    return WHD_SUCCESS;
}

/** Callback for join events
 *  This is called when the WLC_E_SET_SSID event is received,
 *  indicating that the system has joined successfully.
 *  Wakes the thread which was doing the join, allowing it to resume.
 */
static void *whd_wifi_join_events_handler(whd_interface_t ifp, const whd_event_header_t *event_header,
                                          const uint8_t *event_data,
                                          void *handler_user_data)
{
    cy_semaphore_t *semaphore = (cy_semaphore_t *)handler_user_data;
    whd_bool_t join_attempt_complete = WHD_FALSE;
    whd_driver_t whd_driver = ifp->whd_driver;
    whd_result_t result;

    UNUSED_PARAMETER(event_data);

    if (event_header->bsscfgidx >= WHD_INTERFACE_MAX)
    {
        WPRINT_WHD_DEBUG( ("%s: event_header: Bad interface\n", __FUNCTION__) );
        return NULL;
    }

    switch (event_header->event_type)
    {
        case WLC_E_PSK_SUP:
            /* Ignore WLC_E_PSK_SUP event if link is not up */
            if ( (whd_driver->internal_info.whd_join_status[event_header->bsscfgidx] & JOIN_LINK_READY) != 0 )
            {
                if (event_header->status == WLC_SUP_KEYED)
                {
                    /* Successful WPA key exchange */
                    whd_driver->internal_info.whd_join_status[event_header->bsscfgidx] &= ~JOIN_SECURITY_FLAGS_MASK;
                    whd_driver->internal_info.whd_join_status[event_header->bsscfgidx] |= JOIN_SECURITY_COMPLETE;
                }
                else
                {
                    /* join has completed with an error */
                    join_attempt_complete = WHD_TRUE;
                    if ( (event_header->status == WLC_SUP_KEYXCHANGE_WAIT_M1) &&
                         (event_header->reason == WLC_E_SUP_WPA_PSK_TMO) )
                    {
                        /* A timeout waiting for M1 may occur at the edge of the cell or if the AP is particularly slow. */
                        WPRINT_WHD_DEBUG( ("Supplicant M1 timeout event\n") );
                        whd_driver->internal_info.whd_join_status[event_header->bsscfgidx] |= JOIN_EAPOL_KEY_M1_TIMEOUT;
                    }
                    else if ( (event_header->status == WLC_SUP_KEYXCHANGE_WAIT_M3) &&
                              (event_header->reason == WLC_E_SUP_WPA_PSK_TMO) )
                    {
                        /* A timeout waiting for M3 is an indicator that the passphrase may be incorrect. */
                        WPRINT_WHD_DEBUG( ("Supplicant M3 timeout event\n") );
                        whd_driver->internal_info.whd_join_status[event_header->bsscfgidx] |= JOIN_EAPOL_KEY_M3_TIMEOUT;
                    }
                    else if ( (event_header->status == WLC_SUP_KEYXCHANGE_WAIT_G1) &&
                              (event_header->reason == WLC_E_SUP_WPA_PSK_TMO) )
                    {
                        /* A timeout waiting for G1 (group key) may occur at the edge of the cell. */
                        WPRINT_WHD_DEBUG( ("Supplicant G1 timeout event\n") );
                        whd_driver->internal_info.whd_join_status[event_header->bsscfgidx] |= JOIN_EAPOL_KEY_G1_TIMEOUT;
                    }
                    else
                    {
                        WPRINT_WHD_DEBUG( ("Unsuccessful supplicant event; status=0x%" PRIu32 "\n",
                                           event_header->status) );
                        /* Unknown failure during EAPOL key handshake */
                        whd_driver->internal_info.whd_join_status[event_header->bsscfgidx] |= JOIN_EAPOL_KEY_FAILURE;
                    }
                }
            }
            break;

        case WLC_E_SET_SSID:
            if (event_header->status == WLC_E_STATUS_SUCCESS)
            {
                /* SSID has been successfully set. */
                whd_driver->internal_info.whd_join_status[event_header->bsscfgidx] |= JOIN_SSID_SET;
            }
            /* We don't bail out on this event or things like WPS won't work if the AP is rebooting after configuration */
            else if (event_header->status == WLC_E_STATUS_NO_NETWORKS)
            {
                whd_driver->internal_info.whd_join_status[event_header->bsscfgidx] |= JOIN_NO_NETWORKS;
            }
            else
            {
                join_attempt_complete = WHD_TRUE;
            }
            break;

        case WLC_E_LINK:
            if ( (event_header->flags & WLC_EVENT_MSG_LINK) != 0 )
            {
                whd_driver->internal_info.whd_join_status[event_header->bsscfgidx] |= JOIN_LINK_READY;
            }
            else
            {
                whd_driver->internal_info.whd_join_status[event_header->bsscfgidx] &= ~JOIN_LINK_READY;
            }
            break;

        case WLC_E_DEAUTH_IND:
        case WLC_E_DISASSOC_IND:
            whd_driver->internal_info.whd_join_status[event_header->bsscfgidx] &=
                ~(JOIN_AUTHENTICATED | JOIN_LINK_READY);
            break;

        case WLC_E_AUTH:
            if (event_header->status == WLC_E_STATUS_SUCCESS)
            {
                whd_driver->internal_info.whd_join_status[event_header->bsscfgidx] |= JOIN_AUTHENTICATED;
            }
            else if (event_header->status == WLC_E_STATUS_UNSOLICITED)
            {
                WPRINT_WHD_DEBUG( ("Ignore UNSOLICITED pkt event\n") );
            }
            else
            {
                /* We cannot authenticate. Perhaps we're blocked or at the edge of a cell. */
                join_attempt_complete = WHD_TRUE;
            }
            break;

        case WLC_E_CSA_COMPLETE_IND:
            if (event_header->datalen >= sizeof(wl_chan_switch_t) )
            {
                wl_chan_switch_t *wl_csa = (wl_chan_switch_t *)event_data;
                UNUSED_PARAMETER(wl_csa);
                WPRINT_WHD_INFO( ("CSA event => chan %d\n", (dtoh16(wl_csa->chspec) & 0xff) ) );
            }
            break;

        /* Note - These are listed to keep gcc pedantic checking happy */
        case WLC_E_RRM:
        case WLC_E_NONE:
        case WLC_E_ROAM:
        case WLC_E_JOIN:
        case WLC_E_START:
        case WLC_E_AUTH_IND:
        case WLC_E_DEAUTH:
        case WLC_E_ASSOC:
        case WLC_E_ASSOC_IND:
        case WLC_E_REASSOC:
        case WLC_E_REASSOC_IND:
        case WLC_E_DISASSOC:
        case WLC_E_QUIET_START:
        case WLC_E_QUIET_END:
        case WLC_E_BEACON_RX:
        case WLC_E_MIC_ERROR:
        case WLC_E_NDIS_LINK:
        case WLC_E_TXFAIL:
        case WLC_E_PMKID_CACHE:
        case WLC_E_RETROGRADE_TSF:
        case WLC_E_PRUNE:
        case WLC_E_AUTOAUTH:
        case WLC_E_EAPOL_MSG:
        case WLC_E_SCAN_COMPLETE:
        case WLC_E_ADDTS_IND:
        case WLC_E_DELTS_IND:
        case WLC_E_BCNSENT_IND:
        case WLC_E_BCNRX_MSG:
        case WLC_E_BCNLOST_MSG:
        case WLC_E_ROAM_PREP:
        case WLC_E_PFN_NET_FOUND:
        case WLC_E_PFN_NET_LOST:
        case WLC_E_RESET_COMPLETE:
        case WLC_E_JOIN_START:
        case WLC_E_ROAM_START:
        case WLC_E_ASSOC_START:
        case WLC_E_IBSS_ASSOC:
        case WLC_E_RADIO:
        case WLC_E_PSM_WATCHDOG:
        case WLC_E_CCX_ASSOC_START:
        case WLC_E_CCX_ASSOC_ABORT:
        case WLC_E_PROBREQ_MSG:
        case WLC_E_SCAN_CONFIRM_IND:
        case WLC_E_COUNTRY_CODE_CHANGED:
        case WLC_E_EXCEEDED_MEDIUM_TIME:
        case WLC_E_ICV_ERROR:
        case WLC_E_UNICAST_DECODE_ERROR:
        case WLC_E_MULTICAST_DECODE_ERROR:
        case WLC_E_TRACE:
        case WLC_E_BTA_HCI_EVENT:
        case WLC_E_IF:
        case WLC_E_PFN_BEST_BATCHING:
        case WLC_E_RSSI:
        case WLC_E_EXTLOG_MSG:
        case WLC_E_ACTION_FRAME:
        case WLC_E_ACTION_FRAME_COMPLETE:
        case WLC_E_PRE_ASSOC_IND:
        case WLC_E_PRE_REASSOC_IND:
        case WLC_E_CHANNEL_ADOPTED:
        case WLC_E_AP_STARTED:
        case WLC_E_DFS_AP_STOP:
        case WLC_E_DFS_AP_RESUME:
        case WLC_E_WAI_STA_EVENT:
        case WLC_E_WAI_MSG:
        case WLC_E_ESCAN_RESULT:
        case WLC_E_ACTION_FRAME_OFF_CHAN_COMPLETE:
        case WLC_E_PROBRESP_MSG:
        case WLC_E_P2P_PROBREQ_MSG:
        case WLC_E_DCS_REQUEST:
        case WLC_E_FIFO_CREDIT_MAP:
        case WLC_E_ACTION_FRAME_RX:
        case WLC_E_WAKE_EVENT:
        case WLC_E_RM_COMPLETE:
        case WLC_E_HTSFSYNC:
        case WLC_E_OVERLAY_REQ:
        case WLC_E_EXCESS_PM_WAKE_EVENT:
        case WLC_E_PFN_SCAN_NONE:
        case WLC_E_PFN_SCAN_ALLGONE:
        case WLC_E_GTK_PLUMBED:
        case WLC_E_ASSOC_IND_NDIS:
        case WLC_E_REASSOC_IND_NDIS:
        case WLC_E_ASSOC_REQ_IE:
        case WLC_E_ASSOC_RESP_IE:
        case WLC_E_ASSOC_RECREATED:
        case WLC_E_ACTION_FRAME_RX_NDIS:
        case WLC_E_AUTH_REQ:
        case WLC_E_TDLS_PEER_EVENT:
        case WLC_E_SPEEDY_RECREATE_FAIL:
        case WLC_E_NATIVE:
        case WLC_E_PKTDELAY_IND:
        case WLC_E_AWDL_AW:
        case WLC_E_AWDL_ROLE:
        case WLC_E_AWDL_EVENT:
        case WLC_E_NIC_AF_TXS:
        case WLC_E_NAN:
        case WLC_E_BEACON_FRAME_RX:
        case WLC_E_SERVICE_FOUND:
        case WLC_E_GAS_FRAGMENT_RX:
        case WLC_E_GAS_COMPLETE:
        case WLC_E_P2PO_ADD_DEVICE:
        case WLC_E_P2PO_DEL_DEVICE:
        case WLC_E_WNM_STA_SLEEP:
        case WLC_E_TXFAIL_THRESH:
        case WLC_E_PROXD:
        case WLC_E_IBSS_COALESCE:
        case WLC_E_AWDL_RX_PRB_RESP:
        case WLC_E_AWDL_RX_ACT_FRAME:
        case WLC_E_AWDL_WOWL_NULLPKT:
        case WLC_E_AWDL_PHYCAL_STATUS:
        case WLC_E_AWDL_OOB_AF_STATUS:
        case WLC_E_AWDL_SCAN_STATUS:
        case WLC_E_AWDL_AW_START:
        case WLC_E_AWDL_AW_END:
        case WLC_E_AWDL_AW_EXT:
        case WLC_E_AWDL_PEER_CACHE_CONTROL:
        case WLC_E_CSA_START_IND:
        case WLC_E_CSA_DONE_IND:
        case WLC_E_CSA_FAILURE_IND:
        case WLC_E_CCA_CHAN_QUAL:
        case WLC_E_BSSID:
        case WLC_E_TX_STAT_ERROR:
        case WLC_E_BCMC_CREDIT_SUPPORT:
        case WLC_E_PSTA_PRIMARY_INTF_IND:
        case WLC_E_P2P_DISC_LISTEN_COMPLETE:
        case WLC_E_BT_WIFI_HANDOVER_REQ:
        case WLC_E_SPW_TXINHIBIT:
        case WLC_E_FBT_AUTH_REQ_IND:
        case WLC_E_RSSI_LQM:
        case WLC_E_PFN_GSCAN_FULL_RESULT:
        case WLC_E_PFN_SWC:
        case WLC_E_AUTHORIZED:
        case WLC_E_PROBREQ_MSG_RX:
        case WLC_E_PFN_SCAN_COMPLETE:
        case WLC_E_RMC_EVENT:
        case WLC_E_DPSTA_INTF_IND:
        case WLC_E_ULP:
        case WLC_E_LAST:
        default:
            whd_assert("Received event which was not registered\n", 0 != 0);
            break;
    }

    if (whd_wifi_is_ready_to_transceive(ifp) == WHD_SUCCESS)
    {
        join_attempt_complete = WHD_TRUE;
    }

    if (join_attempt_complete == WHD_TRUE)
    {
        if (semaphore != NULL)
        {
            result = cy_rtos_get_semaphore(&whd_driver->internal_info.active_join_mutex, CY_RTOS_NEVER_TIMEOUT,
                                           WHD_FALSE);
            if (result != WHD_SUCCESS)
            {
                WPRINT_WHD_ERROR( ("Get semaphore failed in %s at %d \n", __func__, __LINE__) );
            }
            if (whd_driver->internal_info.active_join_semaphore != NULL)
            {
                whd_assert("Unexpected semaphore\n", whd_driver->internal_info.active_join_semaphore == semaphore);
                result = cy_rtos_set_semaphore(whd_driver->internal_info.active_join_semaphore, WHD_FALSE);
                if (result != WHD_SUCCESS)
                {
                    WPRINT_WHD_ERROR( ("Set semaphore failed in %s at %d \n", __func__, __LINE__) );
                }
            }
            result = cy_rtos_set_semaphore(&whd_driver->internal_info.active_join_mutex, WHD_FALSE);
            if (result != WHD_SUCCESS)
            {
                WPRINT_WHD_ERROR( ("Set semaphore failed in %s at %d \n", __func__, __LINE__) );
            }
        }
        return NULL;
    }
    else
    {
        return handler_user_data;
    }
}

/* Do any needed preparation prior to launching a join */
static whd_result_t whd_wifi_active_join_init(whd_interface_t ifp, whd_security_t auth_type, const uint8_t *security_key,
                                          uint8_t key_length, cy_semaphore_t *semaphore)
{
    whd_driver_t whd_driver = ifp->whd_driver;

    if (whd_driver->internal_info.active_join_mutex_initted == WHD_FALSE)
    {
        CHECK_RETURN(cy_rtos_init_semaphore(&whd_driver->internal_info.active_join_mutex, 1, 0) );
        whd_driver->internal_info.active_join_mutex_initted = WHD_TRUE;
        CHECK_RETURN(cy_rtos_set_semaphore(&whd_driver->internal_info.active_join_mutex, WHD_FALSE) );
    }

    CHECK_RETURN(cy_rtos_get_semaphore(&whd_driver->internal_info.active_join_mutex, CY_RTOS_NEVER_TIMEOUT,
                                       WHD_FALSE) );
    whd_driver->internal_info.active_join_semaphore = semaphore;
    CHECK_RETURN(cy_rtos_set_semaphore(&whd_driver->internal_info.active_join_mutex, WHD_FALSE) );

    CHECK_RETURN(whd_wifi_prepare_join(ifp, auth_type, security_key, key_length, semaphore) );
    return WHD_SUCCESS;
}

whd_result_t whd_set_wsec_info_algos(whd_interface_t ifp, uint32_t algos, uint32_t mask)
{
        whd_driver_t whd_driver;
        wl_wsec_info_t *wsec_info;
        whd_xtlv_t *wsec_info_tlv;
        uint16_t tlv_data_len;
        uint8_t tlv_data[8];
        uint32_t param_len;
        uint8_t  *buf;
        whd_buffer_t buffer;

        whd_driver = ifp->whd_driver;
        CHECK_DRIVER_NULL(whd_driver);
        tlv_data_len = sizeof(tlv_data);
        param_len = offsetof(wl_wsec_info_t, tlvs) +
                    offsetof(wl_wsec_info_tlv_t, data) + tlv_data_len;

        buf = whd_proto_get_iovar_buffer(whd_driver, &buffer, param_len, IOVAR_STR_WSEC_INFO);
        CHECK_IOCTL_BUFFER(buf);

        wsec_info = (wl_wsec_info_t *)buf;
        wsec_info->version = WL_WSEC_INFO_VERSION;
        wsec_info_tlv = (whd_xtlv_t *)(buf + offsetof(struct wl_wsec_info, tlvs));

        wsec_info->num_tlvs++;
        memcpy(tlv_data, &algos, sizeof(algos));
        memcpy(tlv_data + sizeof(algos), &mask, sizeof(mask));

        wsec_info_tlv->id = htod16(WL_WSEC_INFO_BSS_ALGOS);
        wsec_info_tlv->len = htod16(tlv_data_len);
        memcpy(wsec_info_tlv->data, tlv_data, tlv_data_len);

        CHECK_RETURN(whd_proto_set_iovar(ifp, buffer, 0) );
        return WHD_SUCCESS;
}

static whd_result_t whd_wifi_prepare_join(whd_interface_t ifp, whd_security_t auth_type,
                                      const uint8_t *security_key, uint8_t key_length,
                                      cy_semaphore_t *semaphore)
{
    whd_buffer_t buffer;
    uint32_t auth_mfp = WL_MFP_NONE;
    whd_result_t retval = WHD_SUCCESS;
    whd_result_t check_result = WHD_SUCCESS;
    uint32_t *data;
    uint32_t *wpa_auth;
    uint32_t bss_index = 0;
    uint32_t auth;
    whd_driver_t whd_driver = ifp->whd_driver;
    uint16_t event_entry = 0xFF;
    (void)bss_index;
    uint16_t chip_id = whd_chip_get_chip_id(whd_driver);

    if ( chip_id == 43022 )
    {
        if ( (auth_type == WHD_SECURITY_WPA_TKIP_PSK)  || (auth_type == WHD_SECURITY_WPA_AES_PSK)   ||
             (auth_type == WHD_SECURITY_WPA_MIXED_PSK) || (auth_type == WHD_SECURITY_WPA2_TKIP_PSK) ||
             (auth_type == WHD_SECURITY_WPA_TKIP_ENT)  || (auth_type == WHD_SECURITY_WPA_AES_ENT)   ||
             (auth_type == WHD_SECURITY_WPA_MIXED_ENT) || (auth_type == WHD_SECURITY_WPA2_TKIP_ENT) )
        {
            WPRINT_WHD_ERROR( ("WPA and TKIP are not supported, %s failed at line %d \n", __func__, __LINE__) );
            return WHD_UNSUPPORTED;
        }
    }
    if ( (auth_type == WHD_SECURITY_WPA2_FBT_ENT) || (auth_type == WHD_SECURITY_IBSS_OPEN) ||
         (auth_type == WHD_SECURITY_WPA2_FBT_PSK) )
    {
        return WHD_UNKNOWN_SECURITY_TYPE;
    }
    else if ( (auth_type & WEP_ENABLED) != 0 )
    {
        return WHD_WEP_NOT_ALLOWED;
    }
    if ( ( ( (key_length > (uint8_t)WSEC_MAX_PSK_LEN) || (key_length < (uint8_t)WSEC_MIN_PSK_LEN) ) &&
           ( (auth_type == WHD_SECURITY_WPA_TKIP_PSK) || (auth_type == WHD_SECURITY_WPA_AES_PSK) ||
             (auth_type == WHD_SECURITY_WPA2_AES_PSK) || (auth_type == WHD_SECURITY_WPA2_AES_PSK_SHA256) ||
             (auth_type == WHD_SECURITY_WPA2_TKIP_PSK) || (auth_type == WHD_SECURITY_WPA2_MIXED_PSK) ) ) ||
         ( (key_length > (uint8_t)WSEC_MAX_SAE_PASSWORD_LEN) &&
           ( (auth_type == WHD_SECURITY_WPA3_SAE) || (auth_type == WHD_SECURITY_WPA3_WPA2_PSK) ) ) )
    {
        return WHD_INVALID_KEY;
    }

    (void)auth_type, (void)security_key, (void)key_length, (void)semaphore;

    /* Clear the current join status */
    whd_driver->internal_info.whd_join_status[ifp->bsscfgidx] = 0;

    /* Get MFP iovar is not necessary for open security */
    if (auth_type != WHD_SECURITY_OPEN)
    {
        /* Setting wsec will overwrite mfp setting in older branches, store value before setting wsec */
        CHECK_RETURN(whd_wifi_get_iovar_value(ifp, IOVAR_STR_MFP, &auth_mfp) );
    }

    /* Set Wireless Security Type */
    CHECK_RETURN(whd_wifi_set_ioctl_value(ifp, WLC_SET_WSEC, (uint32_t)(auth_type & 0xFF) ) );

    /* Enable Roaming in FW by default */
    CHECK_RETURN(whd_wifi_set_iovar_value(ifp, IOVAR_STR_ROAM_OFF, 0) );

    /* Map the interface to a BSS index */
    bss_index = ifp->bsscfgidx;

    /* Set necessary cfg param for GTKOE to work on 43022 */
    if(whd_driver->chip_info.chip_id == 43022)
    {
        /* Set the wpa auth */
        data =
            (uint32_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer, (uint16_t)8, "bsscfg:" IOVAR_STR_WPA_AUTH);
            CHECK_IOCTL_BUFFER(data);

        data[0] = (int32_t)bss_index;
        data[1] = ((auth_type == WHD_SECURITY_WPA_TKIP_PSK) ?
                               (WPA_AUTH_PSK) : (WPA2_AUTH_PSK) );
        CHECK_RETURN(whd_proto_set_iovar(ifp, buffer, 0) );

        /* Set the wsec */
        data =
            (uint32_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer, (uint16_t)8, "bsscfg:" IOVAR_STR_WSEC);
            CHECK_IOCTL_BUFFER(data);

        data[0] = (int32_t)bss_index;
        data[1] = (auth_type & 0xFF);
        CHECK_RETURN(whd_proto_set_iovar(ifp, buffer, 0) );

        /* Set wowl bit for broadcast key rotation */
        CHECK_RETURN(whd_configure_wowl(ifp, WL_WOWL_KEYROT));
    }

    /* Set supplicant variable - mfg app doesn't support these iovars, so don't care if return fails */
    data = whd_proto_get_iovar_buffer(whd_driver, &buffer, (uint16_t)8, "bsscfg:" IOVAR_STR_SUP_WPA);
    CHECK_IOCTL_BUFFER(data);
    data[0] = htod32(bss_index);
    data[1] =
        htod32( ( uint32_t )( ( ( (auth_type & WPA_SECURITY) != 0 ) || ( (auth_type & WPA2_SECURITY) != 0 ) ||
                                (auth_type & WPA3_SECURITY) != 0 ) ? 1 : 0 ) );
    (void)whd_proto_set_iovar(ifp, buffer, 0);

    /* Set the EAPOL version to whatever the AP is using (-1) */
    data = whd_proto_get_iovar_buffer(whd_driver, &buffer, (uint16_t)8, "bsscfg:" IOVAR_STR_SUP_WPA2_EAPVER);
    CHECK_IOCTL_BUFFER(data);
    data[0] = htod32(bss_index);
    data[1] = htod32( ( uint32_t )-1 );
    (void)whd_proto_set_iovar(ifp, buffer, 0);

    /* Send WPA Key */
    switch (auth_type)
    {
        case WHD_SECURITY_OPEN:
        case WHD_SECURITY_WPS_SECURE:
            break;

        case WHD_SECURITY_WPA_TKIP_PSK:
        case WHD_SECURITY_WPA_AES_PSK:
        case WHD_SECURITY_WPA_MIXED_PSK:
        case WHD_SECURITY_WPA2_AES_PSK:
        case WHD_SECURITY_WPA2_AES_PSK_SHA256:
        case WHD_SECURITY_WPA2_TKIP_PSK:
        case WHD_SECURITY_WPA2_MIXED_PSK:
        case WHD_SECURITY_WPA2_WPA_AES_PSK:
        case WHD_SECURITY_WPA2_WPA_MIXED_PSK:
            /* Set the EAPOL key packet timeout value, otherwise unsuccessful supplicant events aren't reported. If the IOVAR is unsupported then continue. */
            CHECK_RETURN_UNSUPPORTED_CONTINUE(whd_wifi_set_supplicant_key_timeout(ifp,
                                                                                  DEFAULT_EAPOL_KEY_PACKET_TIMEOUT) );
            CHECK_RETURN(whd_wifi_set_passphrase(ifp, security_key, key_length) );
            break;

        case WHD_SECURITY_WPA3_SAE:
        case WHD_SECURITY_WPA3_WPA2_PSK:
            if (auth_type == WHD_SECURITY_WPA3_WPA2_PSK)
            {
                CHECK_RETURN(whd_wifi_enable_sup_set_passphrase(ifp, security_key, key_length, auth_type) );
            }
            /* Set the EAPOL key packet timeout value, otherwise unsuccessful supplicant events aren't reported. If the IOVAR is unsupported then continue. */
            CHECK_RETURN_UNSUPPORTED_CONTINUE(whd_wifi_set_supplicant_key_timeout(ifp,
                                                                                  DEFAULT_EAPOL_KEY_PACKET_TIMEOUT) );
            if (whd_driver->chip_info.fwcap_flags & (1 << WHD_FWCAP_SAE) )
            {
                CHECK_RETURN(whd_wifi_sae_password(ifp, security_key, key_length) );
            }
            else
            {
                /* Disable Roaming in FW, becuase our wpa3_external_supplicant's limitation.
                   If FW report WLC_E_EXT_AUTH_REQ during roaming, Host already called whd_wifi_stop_external_auth_request. */
                CHECK_RETURN(whd_wifi_set_iovar_value(ifp, IOVAR_STR_ROAM_OFF, 1) );
            }
            break;

        case WHD_SECURITY_WPA_TKIP_ENT:
        case WHD_SECURITY_WPA_AES_ENT:
        case WHD_SECURITY_WPA_MIXED_ENT:
        case WHD_SECURITY_WPA2_TKIP_ENT:
        case WHD_SECURITY_WPA2_AES_ENT:
        case WHD_SECURITY_WPA2_MIXED_ENT:
            /* Disable eapol timer by setting to value 0 */
            CHECK_RETURN_UNSUPPORTED_CONTINUE(whd_wifi_set_supplicant_key_timeout(ifp, 0) );
            break;
#if 0
        case WHD_SECURITY_WPA2_FBT_ENT:
            /* Disable eapol timer by setting to value 0 */
            CHECK_RETURN_UNSUPPORTED_CONTINUE(whd_wifi_set_supplicant_key_timeout(ifp, 0) );
            break;
#endif
        case WHD_SECURITY_FORCE_32_BIT:
        case WHD_SECURITY_UNKNOWN:
        default:
            whd_assert("whd_wifi_prepare_join: Unsupported security type\n", 0 != 0);
            break;
    }
    /* Set infrastructure mode */
    CHECK_RETURN(whd_wifi_set_ioctl_value(ifp, WLC_SET_INFRA, ( (auth_type & IBSS_ENABLED) == 0 ) ? 1 : 0) );

    if ( (auth_type == WHD_SECURITY_WPA3_SAE) || (auth_type == WHD_SECURITY_WPA3_WPA2_PSK) )
    {
        auth = WL_AUTH_SAE;
    }
    else
    {
        auth = WL_AUTH_OPEN_SYSTEM;
    }
    CHECK_RETURN(whd_wifi_set_ioctl_value(ifp, WLC_SET_AUTH, auth) );

    /* From PMF cert test plan,
     * 2.2 Out of Box Requirements
     * When WPA2 security is enabled on the DUT, then by defaults the DUT shall:
     * Enable Robust Management Frame Protection Capable (MFPC) functionality
     */
    if (auth_type == WHD_SECURITY_WPA3_SAE)
    {
        auth_mfp = WL_MFP_REQUIRED;
    }
    else if ( (auth_type == WHD_SECURITY_WPA3_WPA2_PSK) || (auth_type & WPA2_SECURITY) )
    {
        auth_mfp = WL_MFP_CAPABLE;
    }

    check_result = whd_wifi_set_iovar_value(ifp, IOVAR_STR_MFP, auth_mfp);
    if (check_result != WHD_SUCCESS)
    {
        WPRINT_WHD_DEBUG( ("Older chipsets might not support MFP..Ignore result\n") );
    }

    /* Set WPA authentication mode */
    wpa_auth = (uint32_t *)whd_proto_get_ioctl_buffer(whd_driver, &buffer, (uint16_t)4);
    CHECK_IOCTL_BUFFER(wpa_auth);

    switch (auth_type)
    {
#if 0
        case WHD_SECURITY_IBSS_OPEN:
            /* IBSS does not get authenticated onto an AP */
            whd_driver->internal_info.whd_join_status[ifp->bsscfgidx] |= JOIN_AUTHENTICATED;
#endif
        /* intentional fall-thru */
        /* Disables Eclipse static analysis warning */
        /* no break */
        /* Fall-Through */
        case WHD_SECURITY_OPEN:
        case WHD_SECURITY_WPS_SECURE:
            *wpa_auth = WPA_AUTH_DISABLED;
            /* Open Networks do not have to complete security */
            whd_driver->internal_info.whd_join_status[ifp->bsscfgidx] |= JOIN_SECURITY_COMPLETE;
            break;

        case WHD_SECURITY_WPA_TKIP_PSK:
        case WHD_SECURITY_WPA_AES_PSK:
        case WHD_SECURITY_WPA_MIXED_PSK:
            *wpa_auth = (uint32_t)WPA_AUTH_PSK;
            break;

        case WHD_SECURITY_WPA2_AES_PSK:
        case WHD_SECURITY_WPA2_TKIP_PSK:
        case WHD_SECURITY_WPA2_MIXED_PSK:
        case WHD_SECURITY_WPA2_WPA_AES_PSK:
        case WHD_SECURITY_WPA2_WPA_MIXED_PSK:
            *wpa_auth = (uint32_t)WPA2_AUTH_PSK;
            break;
        case WHD_SECURITY_WPA2_AES_PSK_SHA256:
            *wpa_auth = (uint32_t)WPA2_AUTH_PSK_SHA256;
            break;

        case WHD_SECURITY_WPA3_SAE:
        case WHD_SECURITY_WPA3_WPA2_PSK:
            *wpa_auth = (uint32_t)WPA3_AUTH_SAE_PSK;
            break;

        case WHD_SECURITY_WPA_TKIP_ENT:
        case WHD_SECURITY_WPA_AES_ENT:
        case WHD_SECURITY_WPA_MIXED_ENT:
            *wpa_auth = (uint32_t)WPA_AUTH_UNSPECIFIED;
            break;

        case WHD_SECURITY_WPA2_TKIP_ENT:
        case WHD_SECURITY_WPA2_AES_ENT:
        case WHD_SECURITY_WPA2_MIXED_ENT:
            *wpa_auth = (uint32_t)WPA2_AUTH_UNSPECIFIED;
            break;
#if 0
        case WHD_SECURITY_WPA2_FBT_ENT:
            *wpa_auth = ( uint32_t )(WPA2_AUTH_UNSPECIFIED | WPA2_AUTH_FT);
            break;
#endif
        case WHD_SECURITY_UNKNOWN:
        case WHD_SECURITY_FORCE_32_BIT:
        default:
            WPRINT_WHD_DEBUG( ("Unsupported Security type\n") );
            *wpa_auth = WPA_AUTH_DISABLED;
            break;
    }
    *wpa_auth = htod32(*wpa_auth);
    CHECK_RETURN(whd_proto_set_ioctl(ifp, WLC_SET_WPA_AUTH, buffer, 0) );

    if (ifp->event_reg_list[WHD_JOIN_EVENT_ENTRY] != WHD_EVENT_NOT_REGISTERED)
    {
        whd_wifi_deregister_event_handler(ifp, ifp->event_reg_list[WHD_JOIN_EVENT_ENTRY]);
        ifp->event_reg_list[WHD_JOIN_EVENT_ENTRY] = WHD_EVENT_NOT_REGISTERED;
    }

    CHECK_RETURN(whd_management_set_event_handler(ifp, join_events, whd_wifi_join_events_handler, (void *)semaphore,
                                                  &event_entry) );
    if (event_entry >= WHD_EVENT_ENTRY_MAX)
    {
        WPRINT_WHD_ERROR( ("Join events registration failed in function %s and line %d", __func__, __LINE__) );
        return WHD_UNFINISHED;
    }
    ifp->event_reg_list[WHD_JOIN_EVENT_ENTRY] = event_entry;
    whd_assert("Set join Event handler failed\n", retval == WHD_SUCCESS);

    return WHD_SUCCESS;
}

/* do any needed tear down after join
 * @param stack_semaphore - semaphore used to control execution if client_semaphore is NULL
 * @param client_semaphore - semaphore used to control execution if client passes this in
 */
static void whd_wifi_active_join_deinit(whd_interface_t ifp, cy_semaphore_t *stack_semaphore, whd_result_t result)
{
    whd_driver_t whd_driver = ifp->whd_driver;
    whd_result_t val;
    /* deinit join specific variables, with protection from mutex */

    val = cy_rtos_get_semaphore(&whd_driver->internal_info.active_join_mutex, CY_RTOS_NEVER_TIMEOUT, WHD_FALSE);
    if (val != WHD_SUCCESS)
        WPRINT_WHD_ERROR( ("Get semaphore failed in %s at %d \n", __func__, __LINE__) );

    whd_driver->internal_info.active_join_semaphore = NULL;

    cy_rtos_deinit_semaphore(stack_semaphore);

    if (WHD_SUCCESS != result)
    {
        WPRINT_WHD_INFO( ("Failed join (err %" PRIu32 ")\n", result) );
        ifp->role = WHD_INVALID_ROLE;
    }

    val = cy_rtos_set_semaphore(&whd_driver->internal_info.active_join_mutex, WHD_FALSE);
    if (val != WHD_SUCCESS)
        WPRINT_WHD_ERROR( ("Get semaphore failed in %s at %d \n", __func__, __LINE__) );

    /* we forced the chip to be up during join, now let it sleep */
    WHD_WLAN_LET_SLEEP(whd_driver);
}

static uint32_t whd_wifi_join_wait_for_complete(whd_interface_t ifp, cy_semaphore_t *semaphore)
{
    whd_result_t result;
    uint32_t start_time;
    uint32_t current_time;
    whd_bool_t done = WHD_FALSE;

    cy_rtos_get_time(&start_time);

    while (!done)
    {
        result = cy_rtos_get_semaphore(semaphore, DEFAULT_JOIN_ATTEMPT_TIMEOUT / 10, WHD_FALSE);
        whd_assert("Get semaphore failed", (result == CY_RSLT_SUCCESS) || (result == CY_RTOS_TIMEOUT) );
        REFERENCE_DEBUG_ONLY_VARIABLE(result);

        result = whd_wifi_is_ready_to_transceive(ifp);
        if (result == WHD_SUCCESS)
        {
            break;
        }

        cy_rtos_get_time(&current_time);
        done = (whd_bool_t)( (current_time - start_time) >= DEFAULT_JOIN_ATTEMPT_TIMEOUT );
    }

    if (result != WHD_SUCCESS)
    {
        CHECK_RETURN(whd_wifi_leave(ifp) );
        WPRINT_WHD_INFO( ("%s: not ready to transceive (err %" PRIu32 "); left network\n", __func__, result) );
    }

    return result;
}

static whd_result_t whd_wifi_check_join_status(whd_interface_t ifp)
{
    whd_driver_t whd_driver = ifp->whd_driver;

    if ( (uint32_t)ifp->bsscfgidx >= WHD_INTERFACE_MAX )
    {
        WPRINT_WHD_ERROR( ("%s: Bad interface %d\n", __FUNCTION__, ifp->bsscfgidx) );
        return WHD_INVALID_JOIN_STATUS;
    }
    switch (whd_driver->internal_info.whd_join_status[ifp->bsscfgidx])
    {
        case JOIN_NO_NETWORKS:
            return WHD_NETWORK_NOT_FOUND;

        case JOIN_AUTHENTICATED | JOIN_LINK_READY | JOIN_EAPOL_KEY_M1_TIMEOUT:
            return WHD_EAPOL_KEY_PACKET_M1_TIMEOUT;

        case JOIN_AUTHENTICATED | JOIN_LINK_READY | JOIN_EAPOL_KEY_M3_TIMEOUT:
        case JOIN_AUTHENTICATED | JOIN_LINK_READY | JOIN_SSID_SET | JOIN_EAPOL_KEY_M3_TIMEOUT:
            return WHD_EAPOL_KEY_PACKET_M3_TIMEOUT;

        case JOIN_AUTHENTICATED | JOIN_LINK_READY | JOIN_EAPOL_KEY_G1_TIMEOUT:
        case JOIN_AUTHENTICATED | JOIN_LINK_READY | JOIN_SSID_SET | JOIN_EAPOL_KEY_G1_TIMEOUT:
            return WHD_EAPOL_KEY_PACKET_G1_TIMEOUT;

        case JOIN_AUTHENTICATED | JOIN_LINK_READY | JOIN_EAPOL_KEY_FAILURE:
        case JOIN_AUTHENTICATED | JOIN_LINK_READY | JOIN_SSID_SET | JOIN_EAPOL_KEY_FAILURE:
            return WHD_EAPOL_KEY_FAILURE;

        case JOIN_AUTHENTICATED | JOIN_LINK_READY | JOIN_SSID_SET | JOIN_SECURITY_COMPLETE:
            return WHD_SUCCESS;

        case 0:
        case JOIN_SECURITY_COMPLETE: /* For open/WEP */
            return WHD_NOT_AUTHENTICATED;

        case JOIN_AUTHENTICATED | JOIN_LINK_READY | JOIN_SECURITY_COMPLETE:
            return WHD_JOIN_IN_PROGRESS;

        case JOIN_AUTHENTICATED | JOIN_LINK_READY:
        case JOIN_AUTHENTICATED | JOIN_LINK_READY | JOIN_SSID_SET:
            return WHD_NOT_KEYED;

        default:
            return WHD_INVALID_JOIN_STATUS;
    }
}

whd_result_t whd_wifi_join_specific(whd_interface_t ifp, const whd_scan_result_t *ap, const uint8_t *security_key,
                                uint8_t key_length)
{
    whd_buffer_t buffer;
    cy_semaphore_t join_semaphore;
    whd_result_t result;
    wl_extjoin_params_t *ext_join_params;
    wl_join_params_t *join_params;
    whd_security_t security = ap->security;
    whd_driver_t whd_driver;
    wl_chanspec_t chanspec = 0;

    CHECK_IFP_NULL(ifp);

    whd_driver = ifp->whd_driver;

    CHECK_DRIVER_NULL(whd_driver);

    /* Keep WLAN awake while joining */
    WHD_WLAN_KEEP_AWAKE(whd_driver);
    ifp->role = WHD_STA_ROLE;

    if (ap->bss_type == WHD_BSS_TYPE_MESH)
    {
        return WHD_UNSUPPORTED;
    }

    if (ap->bss_type == WHD_BSS_TYPE_ADHOC)
    {
        security |= IBSS_ENABLED;
    }

    if (ap->channel == 0)
    {
        WPRINT_WHD_INFO( ("FW will do assoc-scan full channels\n") );
    }
    else if (ap->band == WHD_802_11_BAND_2_4GHZ)
    {
        chanspec = (wl_chanspec_t)(ap->channel | GET_C_VAR(whd_driver, CHANSPEC_BAND_2G) |
                                   GET_C_VAR(whd_driver,
                                             CHANSPEC_BW_20) | GET_C_VAR(whd_driver, CHANSPEC_CTL_SB_NONE) );
    }
    else if (ap->band == WHD_802_11_BAND_5GHZ)
    {
        chanspec = (wl_chanspec_t)(ap->channel | GET_C_VAR(whd_driver, CHANSPEC_BAND_5G) |
                                   GET_C_VAR(whd_driver,
                                             CHANSPEC_BW_20) | GET_C_VAR(whd_driver, CHANSPEC_CTL_SB_NONE) );
    }
    else if (ap->band == WHD_802_11_BAND_6GHZ)
    {
        chanspec = (wl_chanspec_t)(ap->channel | GET_C_VAR(whd_driver, CHANSPEC_BAND_6G) |
                                   GET_C_VAR(whd_driver,
                                             CHANSPEC_BW_20) | GET_C_VAR(whd_driver, CHANSPEC_CTL_SB_NONE) );
    }
    else
    {
        WPRINT_WHD_ERROR( ("AP Band is not allowed/valid\n") );
        return WHD_BADARG;
    }

    if (NULL_MAC(ap->BSSID.octet) )
    {
        WPRINT_WHD_ERROR( ("NULL address is not allowed/valid\n") );
        return WHD_BADARG;
    }

    if (BROADCAST_ID(ap->BSSID.octet) )
    {
        WPRINT_WHD_ERROR( ("Broadcast address is not allowed/valid in join with specific BSSID of AP\n") );
        return WHD_BADARG;
    }

    if ( (ap->SSID.length == 0) || (ap->SSID.length > (size_t)SSID_NAME_SIZE) )
    {
        WPRINT_WHD_ERROR( ("%s: failure: SSID length error\n", __func__) );
        return WHD_WLAN_BADSSIDLEN;
    }

    CHECK_RETURN(cy_rtos_init_semaphore(&join_semaphore, 1, 0) );
    result = whd_wifi_active_join_init(ifp, security, security_key, key_length, &join_semaphore);

    if (result == WHD_SUCCESS)
    {
        if (ap->bss_type == WHD_BSS_TYPE_ADHOC)
        {
           CHECK_RETURN(whd_wifi_set_chanspec(ifp, chanspec) );
        }

        /* Join network */
        ext_join_params =
            (wl_extjoin_params_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer, sizeof(wl_extjoin_params_t), "join");
        CHECK_IOCTL_BUFFER(ext_join_params);
        memset(ext_join_params, 0, sizeof(wl_extjoin_params_t) );

        ext_join_params->ssid.SSID_len = ap->SSID.length;
        DISABLE_COMPILER_WARNING(diag_suppress = Pa039)
        memcpy(ext_join_params->ssid.SSID, ap->SSID.value, ext_join_params->ssid.SSID_len);
        ENABLE_COMPILER_WARNING(diag_suppress = Pa039)
        memcpy(&ext_join_params->assoc_params.bssid, &ap->BSSID, sizeof(whd_mac_t) );
        ext_join_params->scan_params.scan_type = 0;
        ext_join_params->scan_params.active_time = -1;
        ext_join_params->scan_params.home_time = -1;
        ext_join_params->scan_params.nprobes = -1;
        ext_join_params->scan_params.passive_time = -1;
        ext_join_params->assoc_params.bssid_cnt = 0;
        if (ap->channel)
        {
            ext_join_params->assoc_params.chanspec_num = (uint32_t)1;
            ext_join_params->assoc_params.chanspec_list[0] = htod16(chanspec);
        }
        result = whd_proto_set_iovar(ifp, buffer, 0);

        WPRINT_WHD_INFO( ("%s: set_ssid result (err %" PRIu32 "); left network\n", __func__, result) );

        /* Some firmware, e.g. for 4390, does not support the join IOVAR, so use the older IOCTL call instead */
        if (result == WHD_WLAN_UNSUPPORTED)
        {
            join_params =
                (wl_join_params_t *)whd_proto_get_ioctl_buffer(whd_driver, &buffer, sizeof(wl_join_params_t) );
            CHECK_IOCTL_BUFFER(join_params);
            memset(join_params, 0, sizeof(wl_join_params_t) );
            DISABLE_COMPILER_WARNING(diag_suppress = Pa039)
            memcpy(&join_params->ssid, &ext_join_params->ssid, sizeof(wlc_ssid_t) );
            ENABLE_COMPILER_WARNING(diag_suppress = Pa039)
            memcpy(&join_params->params.bssid, &ap->BSSID, sizeof(whd_mac_t) );
            join_params->params.bssid_cnt = 0;
            if (ap->channel)
            {
                join_params->params.chanspec_num = (uint32_t)1;
                join_params->params.chanspec_list[0] = htod16(chanspec);
            }
            result = whd_proto_set_ioctl(ifp, WLC_SET_SSID, buffer, 0);
        }

        if (result == WHD_SUCCESS)
        {

            uint16_t chip_id = whd_chip_get_chip_id(whd_driver);

            CHECK_RETURN(whd_wifi_join_wait_for_complete(ifp, &join_semaphore) );

            if ( (chip_id == 0x4373) || (chip_id == 55560) )
            {
                /* For 11 AC MAX throughput set the frame burst and MPDU per AMPDU */
                CHECK_RETURN(whd_wifi_set_iovar_value(ifp, IOVAR_STR_MPDU_PER_AMPDU, 16) );
            }

        }
        else
        {
            WPRINT_WHD_INFO( ("%s:3 not ready to transceive (err %" PRIu32 "); left network\n", __func__, result) );
        }
    }
    else
    {
        WPRINT_WHD_INFO( ("%s: active join init failed: (%" PRIu32 ")\n", __FUNCTION__, result) );
    }
    /* clean up from the join attempt */
    whd_wifi_active_join_deinit(ifp, &join_semaphore, result);

    CHECK_RETURN(result);

    return WHD_SUCCESS;
}

whd_result_t whd_wifi_join(whd_interface_t ifp, const whd_ssid_t *ssid, whd_security_t auth_type,
                       const uint8_t *security_key, uint8_t key_length)
{
    cy_semaphore_t join_sema;
    whd_result_t result;
    whd_buffer_t buffer;
    wlc_ssid_t *ssid_params;
    whd_driver_t whd_driver;

    CHECK_IFP_NULL(ifp);

    whd_driver = ifp->whd_driver;

    CHECK_DRIVER_NULL(whd_driver);

    if (ssid == NULL)
    {
        WPRINT_WHD_ERROR( ("%s: failure: ssid is null\n", __func__) );
        return WHD_BADARG;
    }

    if ( (ssid->length == 0) || (ssid->length > (size_t)SSID_NAME_SIZE) )
    {
        WPRINT_WHD_ERROR( ("%s: failure: SSID length error\n", __func__) );
        return WHD_WLAN_BADSSIDLEN;
    }

    /* Keep WLAN awake while joining */
    WHD_WLAN_KEEP_AWAKE(whd_driver);
    ifp->role = WHD_STA_ROLE;

    CHECK_RETURN(cy_rtos_init_semaphore(&join_sema, 1, 0) );
    result = whd_wifi_active_join_init(ifp, auth_type, security_key, key_length, &join_sema);

    if (result == WHD_SUCCESS)
    {
        /* Join network */
        ssid_params = (struct wlc_ssid *)whd_proto_get_ioctl_buffer(whd_driver, &buffer, sizeof(wlc_ssid_t) );
        CHECK_IOCTL_BUFFER(ssid_params);
        memset(ssid_params, 0, sizeof(wlc_ssid_t) );
        ssid_params->SSID_len = htod32(ssid->length);
        memcpy(ssid_params->SSID, ssid->value, ssid_params->SSID_len);
        result = whd_proto_set_ioctl(ifp, WLC_SET_SSID, buffer, 0);

        if (result == WHD_SUCCESS)
        {
            CHECK_RETURN(whd_wifi_join_wait_for_complete(ifp, &join_sema) );
        }
    }

    /* clean up from the join attempt */
    whd_wifi_active_join_deinit(ifp, &join_sema, result);

    return result;
}

whd_result_t whd_wifi_leave(whd_interface_t ifp)
{
    whd_result_t result = WHD_SUCCESS;
    whd_driver_t whd_driver;
    uint32_t bss_index = 0;

    CHECK_IFP_NULL(ifp);

    whd_driver = ifp->whd_driver;

    CHECK_DRIVER_NULL(whd_driver);

    /* Map the interface to a BSS index */
    bss_index = ifp->bsscfgidx;

    /* If interface is greater than max return error */
    if (bss_index >= WHD_INTERFACE_MAX)
    {
        WPRINT_WHD_ERROR( ("%s: Bad interface 2\n", __FUNCTION__) );
        return WHD_BADARG;
    }
    if (ifp->event_reg_list[WHD_JOIN_EVENT_ENTRY] != WHD_EVENT_NOT_REGISTERED)
    {
        CHECK_RETURN(whd_wifi_deregister_event_handler(ifp, ifp->event_reg_list[WHD_JOIN_EVENT_ENTRY]) );
        ifp->event_reg_list[WHD_JOIN_EVENT_ENTRY] = WHD_EVENT_NOT_REGISTERED;
    }

    /* Disassociate from AP */
    result = whd_wifi_set_ioctl_buffer(ifp, WLC_DISASSOC, NULL, 0);

    if (result != WHD_SUCCESS)
    {
        WPRINT_WHD_DEBUG( ("send_ioctl(WLC_DISASSOC) failed:%" PRIu32 "\r\n", result) );
    }

    if((whd_driver->chip_info.chip_id == 43022) || (whd_driver->chip_info.chip_id == 43907) || (whd_driver->chip_info.chip_id == 43909) || (whd_driver->chip_info.chip_id == 54907) || (whd_driver->chip_info.chip_id == 43012))
    {
        whd_buffer_t buffer = NULL;
        uint32_t *data = NULL;
        /* De-initialize the supplicant, as sup init happens on every join */
        data = whd_proto_get_iovar_buffer(whd_driver, &buffer, (uint16_t)8, "bsscfg:" IOVAR_STR_SUP_WPA);
        CHECK_IOCTL_BUFFER(data);
        data[0] = htod32(bss_index);
        data[1] = htod32(0);
        (void)whd_proto_set_iovar(ifp, buffer, 0);
    }

    whd_driver->internal_info.whd_join_status[bss_index] = 0;
    ifp->role = WHD_INVALID_ROLE;

    if (whd_driver->internal_info.active_join_mutex_initted == WHD_TRUE)
    {
        cy_rtos_deinit_semaphore(&whd_driver->internal_info.active_join_mutex);
        whd_driver->internal_info.active_join_mutex_initted = WHD_FALSE;
    }
    if (whd_driver->internal_info.active_join_semaphore)
    {
        cy_rtos_deinit_semaphore(whd_driver->internal_info.active_join_semaphore);
        whd_driver->internal_info.active_join_semaphore = NULL;
    }


    return WHD_SUCCESS;
}

/** Handles scan result events
 *
 *  This function receives scan record events, and parses them into a better format, then passes the results
 *  to the user application.
 *
 * @param event_header     : The event details
 * @param event_data       : The data for the event which contains the scan result structure
 * @param handler_user_data: data which will be passed to user application
 *
 * @returns : handler_user_data parameter
 *
 */
static void *whd_wifi_scan_events_handler(whd_interface_t ifp, const whd_event_header_t *event_header,
                                          const uint8_t *event_data,
                                          void *handler_user_data)

{
    whd_scan_result_t *record;
    wl_escan_result_t *eresult;
    wl_bss_info_t *bss_info;
    uint16_t chanspec;
    uint32_t version;
    whd_tlv8_header_t *cp;
    uint32_t len;
    uint16_t ie_offset;
    uint32_t bss_info_length;
    country_info_ie_fixed_portion_t *country_info_ie;
    rsn_ie_fixed_portion_t *rsnie;
    wpa_ie_fixed_portion_t *wpaie = NULL;
    rsnx_ie_t *rsnxie = NULL;
    uint8_t rate_num;
    ht_capabilities_ie_t *ht_capabilities_ie = NULL;
    uint32_t count_tmp = 0;
    uint16_t temp16;
    uint16_t bss_count;
    whd_driver_t whd_driver = ifp->whd_driver;

    if (whd_driver->internal_info.scan_result_callback == NULL)
    {
        return handler_user_data;
    }

    if (event_header->status == WLC_E_STATUS_SUCCESS)
    {
        whd_driver->internal_info.scan_result_callback(NULL, handler_user_data, WHD_SCAN_COMPLETED_SUCCESSFULLY);
        whd_driver->internal_info.scan_result_callback = NULL;
        whd_wifi_deregister_event_handler(ifp, ifp->event_reg_list[WHD_SCAN_EVENT_ENTRY]);
        ifp->event_reg_list[WHD_SCAN_EVENT_ENTRY] = WHD_EVENT_NOT_REGISTERED;
        return handler_user_data;
    }
    if ( (event_header->status == WLC_E_STATUS_NEWSCAN) || (event_header->status == WLC_E_STATUS_NEWASSOC) ||
         (event_header->status == WLC_E_STATUS_ABORT) )
    {
        whd_driver->internal_info.scan_result_callback(NULL, handler_user_data, WHD_SCAN_ABORTED);
        whd_driver->internal_info.scan_result_callback = NULL;
        whd_wifi_deregister_event_handler(ifp, ifp->event_reg_list[WHD_SCAN_EVENT_ENTRY]);
        ifp->event_reg_list[WHD_SCAN_EVENT_ENTRY] = WHD_EVENT_NOT_REGISTERED;
        return handler_user_data;
    }

    if (event_header->status != WLC_E_STATUS_PARTIAL)
    {
        return handler_user_data;
    }

    eresult = (wl_escan_result_t *)event_data;
    bss_info = &eresult->bss_info[0];
    bss_count = dtoh16(eresult->bss_count);

    version = dtoh32(WHD_READ_32(&bss_info->version) );
    whd_minor_assert("wl_bss_info_t has wrong version", version == WL_BSS_INFO_VERSION);

    /* PNO bss info doesn't contain the correct bss info version */
    if (version != WL_BSS_INFO_VERSION)
    {
        whd_minor_assert("Invalid bss_info version returned by firmware\n", version != WL_BSS_INFO_VERSION);

        return handler_user_data;
    }

    whd_minor_assert("More than one result returned by firmware", bss_count == 1);
    if (bss_count != 1)
    {
        return handler_user_data;
    }

    /*
     * check the SSID length and bssinfo ie offset for buffer overflow
     */
    bss_info->ie_offset = dtoh16(bss_info->ie_offset);
    bss_info->ie_length = dtoh32(bss_info->ie_length);
    if ( (bss_info->SSID_len > sizeof(bss_info->SSID) ) || (bss_info->ie_offset < sizeof(wl_bss_info_t) ) ||
         (bss_info->ie_offset > (sizeof(wl_bss_info_t) + bss_info->ie_length) ) )
    {
        WPRINT_WHD_ERROR( ("Invalid bss length check %s: SSID_len:%d,ie_len:%" PRIu32 ",ie_off:%d\n", __FUNCTION__,
                           bss_info->SSID_len, bss_info->ie_length, bss_info->ie_offset) );
        whd_minor_assert(" bss length check failed\n", bss_info->SSID_len != sizeof(bss_info->SSID) );
        return handler_user_data;
    }

    /* Safe to access *whd_scan_result_ptr, as whd_scan_result_ptr == NULL case is handled above */
    record = (whd_scan_result_t *)(whd_driver->internal_info.whd_scan_result_ptr);

    /* Clear the last scan result data */
    memset(record, 0, sizeof(whd_scan_result_t) );

    /*
     * Totally ignore off channel results.  This can only happen with DSSS (1 and 2 Mb).  It is better to
     * totally ignore it when it happens.  It is hard to argue it is "significant" given that it can't
     * happen in 5G with OFDM (or other 2G modulations).  This is left here so that it could be
     * passed as a scan result for debugging only.
     */
    if (!(bss_info->flags & WL_BSS_FLAGS_RSSI_ONCHANNEL) )
    {
        record->flags |= WHD_SCAN_RESULT_FLAG_RSSI_OFF_CHANNEL;
        /* Comment out this return statement to pass along an off channel result for debugging */
        return handler_user_data;
    }

    /* Copy the SSID into the output record structure */
    record->SSID.length = (uint8_t)MIN_OF(sizeof(record->SSID.value), bss_info->SSID_len);
    memset(record->SSID.value, 0, sizeof(record->SSID.value) );
    memcpy(record->SSID.value, bss_info->SSID, record->SSID.length);

    /* Copy the BSSID into the output record structure */
    memcpy( (void *)record->BSSID.octet, (const void *)bss_info->BSSID.octet, sizeof(bss_info->BSSID.octet) );

    /* Copy the RSSI into the output record structure */
    record->signal_strength = ( int16_t )dtoh16( (WHD_READ_16(&bss_info->RSSI) ) );

    /* Find maximum data rate and put it in the output record structure */
    record->max_data_rate = 0;
    count_tmp = WHD_READ_32(&bss_info->rateset.count);
    if (count_tmp > 16)
    {
        count_tmp = 16;
    }

#ifdef WPRINT_ENABLE_WHD_DEBUG
    /* print out scan results info */
    {
        char ea_buf[WHD_ETHER_ADDR_STR_LEN];
        char ssid_buf[SSID_NAME_SIZE + 1];

        WPRINT_WHD_DEBUG( ("Scan result: channel=%d signal=%d ssid=%s bssid=%s\n", record->channel,
                           record->signal_strength,
                           whd_ssid_to_string(record->SSID.value, record->SSID.length, ssid_buf,
                                              (uint8_t)sizeof(ssid_buf) ),
                           whd_ether_ntoa( (const uint8_t *)bss_info->BSSID.octet, ea_buf, sizeof(ea_buf) ) ) );
    }
#endif /* WPRINT_ENABLE_WHD_DEBUG */

    for (rate_num = 0; rate_num < count_tmp; rate_num++)
    {
        uint32_t rate = RSPEC_TO_KBPS(bss_info->rateset.rates[rate_num]);
        if (record->max_data_rate < rate)
        {
            record->max_data_rate = rate;
        }
    }

    bss_info->capability = dtoh16(bss_info->capability);

    /* Write the BSS type into the output record structure */
    record->bss_type =
        ( (bss_info->capability & DOT11_CAP_ESS) !=
          0 ) ? WHD_BSS_TYPE_INFRASTRUCTURE : ( (bss_info->capability & DOT11_CAP_IBSS) !=
                                                0 ) ? WHD_BSS_TYPE_ADHOC : WHD_BSS_TYPE_UNKNOWN;

    /* Determine the network security.
     * Some of this section has been copied from the standard broadcom host driver file wl/exe/wlu.c function wl_dump_wpa_rsn_ies
     */

    ie_offset = WHD_READ_16(&bss_info->ie_offset);
    cp = (whd_tlv8_header_t *)( ( (uint8_t *)bss_info ) + ie_offset );
    len = WHD_READ_32(&bss_info->ie_length);
    bss_info_length = WHD_READ_32(&bss_info->length);

    record->ie_ptr = (uint8_t *)cp;
    record->ie_len = len;

    /* Validate the length of the IE section */
    if ( (ie_offset > bss_info_length) || (len > bss_info_length - ie_offset) )
    {
        whd_minor_assert("Invalid ie length", 1 == 0);
        return handler_user_data;
    }

    /* Find an RSN IE (Robust-Security-Network Information-Element) */
    rsnie = (rsn_ie_fixed_portion_t *)whd_parse_dot11_tlvs(cp, len, DOT11_IE_ID_RSN);

    /* Find a WPA IE */
    if (rsnie == NULL)
    {
        whd_tlv8_header_t *parse = cp;
        uint32_t parse_len = len;
        while ( (wpaie =
                     (wpa_ie_fixed_portion_t *)whd_parse_tlvs(parse, parse_len, DOT11_IE_ID_VENDOR_SPECIFIC) ) != 0 )
        {
            if (whd_is_wpa_ie( (vendor_specific_ie_header_t *)wpaie, &parse, &parse_len ) != WHD_FALSE)
            {
                break;
            }
        }
    }

    temp16 = WHD_READ_16(&bss_info->capability);

    /* Check if AP is configured for RSN */
    if ( (rsnie != NULL) &&
         (rsnie->tlv_header.length >= RSN_IE_MINIMUM_LENGTH + rsnie->pairwise_suite_count * sizeof(uint32_t) ) )
    {
        uint16_t a;
        uint32_t group_key_suite;
        akm_suite_portion_t *akm_suites;
        DISABLE_COMPILER_WARNING(diag_suppress = Pa039)
        akm_suites = (akm_suite_portion_t *)&(rsnie->pairwise_suite_list[rsnie->pairwise_suite_count]);
        ENABLE_COMPILER_WARNING(diag_suppress = Pa039)
        for (a = 0; a < akm_suites->akm_suite_count; ++a)
        {
            uint32_t akm_suite_list_item = ntoh32(akm_suites->akm_suite_list[a]) & 0xFF;
            if (akm_suite_list_item == (uint32_t)WHD_AKM_PSK)
            {
                record->security |= WPA2_SECURITY;
            }
            if (akm_suite_list_item == (uint32_t)WHD_AKM_PSK_SHA256)
            {
                record->security |= WPA2_SECURITY;
                record->security |= WPA2_SHA256_SECURITY;
            }
            if (akm_suite_list_item == (uint32_t)WHD_AKM_SAE_SHA256)
            {
                record->security |= WPA3_SECURITY;
            }
            if (akm_suite_list_item == (uint32_t)WHD_AKM_8021X)
            {
                record->security |= WPA2_SECURITY;
                record->security |= ENTERPRISE_ENABLED;
            }
            if (akm_suite_list_item == (uint32_t)WHD_AKM_FT_8021X)
            {
                record->security |= WPA2_SECURITY;
                record->security |= FBT_ENABLED;
                record->security |= ENTERPRISE_ENABLED;
            }
            if (akm_suite_list_item == (uint32_t)WHD_AKM_FT_PSK)
            {
                record->security |= WPA2_SECURITY;
                record->security |= FBT_ENABLED;
            }
        }

        group_key_suite = ntoh32(rsnie->group_key_suite) & 0xFF;
        /* Check the RSN contents to see if there are any references to TKIP cipher (2) in the group key or pairwise keys, */
        /* If so it must be mixed mode. */
        if (group_key_suite == (uint32_t)WHD_CIPHER_TKIP)
        {
            record->security |= TKIP_ENABLED;
        }
        if (group_key_suite == (uint32_t)WHD_CIPHER_CCMP_128)
        {
            record->security |= AES_ENABLED;
        }

        for (a = 0; a < rsnie->pairwise_suite_count; ++a)
        {
            uint32_t pairwise_suite_list_item = ntoh32(rsnie->pairwise_suite_list[a]) & 0xFF;
            if (pairwise_suite_list_item == (uint32_t)WHD_CIPHER_TKIP)
            {
                record->security |= TKIP_ENABLED;
            }

            if (pairwise_suite_list_item == (uint32_t)WHD_CIPHER_CCMP_128)
            {
                record->security |= AES_ENABLED;
            }
        }
    }
    /* Check if AP is configured for WPA */
    else if ( (wpaie != NULL) &&
              (wpaie->vendor_specific_header.tlv_header.length >=
               WPA_IE_MINIMUM_LENGTH + wpaie->unicast_suite_count * sizeof(uint32_t) ) )
    {
        uint16_t a;
        uint32_t group_key_suite;
        akm_suite_portion_t *akm_suites;

        record->security = (whd_security_t)WPA_SECURITY;
        group_key_suite = ntoh32(wpaie->multicast_suite) & 0xFF;
        if (group_key_suite == (uint32_t)WHD_CIPHER_TKIP)
        {
            record->security |= TKIP_ENABLED;
        }
        if (group_key_suite == (uint32_t)WHD_CIPHER_CCMP_128)
        {
            record->security |= AES_ENABLED;
        }

        akm_suites = (akm_suite_portion_t *)&(wpaie->unicast_suite_list[wpaie->unicast_suite_count]);
        for (a = 0; a < akm_suites->akm_suite_count; ++a)
        {
            uint32_t akm_suite_list_item = ntoh32(akm_suites->akm_suite_list[a]) & 0xFF;
            if (akm_suite_list_item == (uint32_t)WHD_AKM_8021X)
            {
                record->security |= ENTERPRISE_ENABLED;
            }
        }

        for (a = 0; a < wpaie->unicast_suite_count; ++a)
        {
            if (wpaie->unicast_suite_list[a][3] == (uint32_t)WHD_CIPHER_CCMP_128)
            {
                record->security |= AES_ENABLED;
            }
        }
    }
    /* Check if AP is configured for WEP, that is, if the capabilities field indicates privacy, then security supports WEP */
    else if ( (temp16 & DOT11_CAP_PRIVACY) != 0 )
    {
        record->security = WHD_SECURITY_WEP_PSK;
    }
    else
    {
        /* Otherwise no security */
        record->security = WHD_SECURITY_OPEN;
    }

    /* Find a RSNX IE */
    rsnxie = (rsnx_ie_t *)whd_parse_tlvs(cp, len, DOT11_IE_ID_RSNX);
    if ( (rsnxie != NULL) && (rsnxie->tlv_header.length == DOT11_RSNX_CAP_LEN) &&
         (rsnxie->data[0] & (1 << DOT11_RSNX_SAE_H2E) ) )
    {
        record->flags |= WHD_SCAN_RESULT_FLAG_SAE_H2E;
    }

    /* Update the maximum data rate with 11n rates from the HT Capabilities IE */
    ht_capabilities_ie = (ht_capabilities_ie_t *)whd_parse_tlvs(cp, len, DOT11_IE_ID_HT_CAPABILITIES);
    if ( (ht_capabilities_ie != NULL) && (ht_capabilities_ie->tlv_header.length == HT_CAPABILITIES_IE_LENGTH) )
    {
        uint8_t a;
        uint8_t supports_40mhz =
            (ht_capabilities_ie->ht_capabilities_info & HT_CAPABILITIES_INFO_SUPPORTED_CHANNEL_WIDTH_SET) != 0 ? 1 : 0;
        uint8_t short_gi[2] =
        { (ht_capabilities_ie->ht_capabilities_info & HT_CAPABILITIES_INFO_SHORT_GI_FOR_20MHZ) != 0 ? 1 : 0,
          (ht_capabilities_ie->ht_capabilities_info & HT_CAPABILITIES_INFO_SHORT_GI_FOR_40MHZ) != 0 ? 1 : 0 };

        /* Find highest bit from MCS info */
        for (a = 31; a != 0xFF; --a)
        {
            if ( (ht_capabilities_ie->rx_mcs[a / 8] & (1 << (a % 8) ) ) != 0 )
            {
                break;
            }
        }
        if (a != 0xFF)
        {
            record->max_data_rate =
                ( uint32_t )(100UL * mcs_data_rate_lookup_table[a][supports_40mhz][short_gi[supports_40mhz]]);
        }
    }

    if (bss_info->flags & WL_BSS_FLAGS_FROM_BEACON)
    {
        record->flags |= WHD_SCAN_RESULT_FLAG_BEACON;
    }

    /* Get the channel for pre-N and control channel for n/HT or later */
    chanspec = dtoh16(WHD_READ_16(&bss_info->chanspec) );
    if (CHSPEC_IS6G(chanspec) )
    {
        uint16_t ctrl_ch_num;

        whd_chip_get_chanspec_ctl_channel_num(whd_driver, chanspec, &ctrl_ch_num);
        record->channel = ctrl_ch_num;
    }
    else if (bss_info->n_cap)
    {
        /* Check control channel first.The channel that chanspec reports is the center frequency which might not be the same as
         * the 20 MHz channel that the beacons is on (primary or control channel) if it's an 802.11n/AC 40MHz or wider channel.
         */
        record->channel = bss_info->ctl_ch;
    }
    else
    {
        /* 11 a/b/g and 20MHz bandwidth only */
        record->channel = ( ( uint8_t )(chanspec & WL_CHANSPEC_CHAN_MASK) );
    }

    /* Find country info IE (Country-Information Information-Element) */
    country_info_ie = (country_info_ie_fixed_portion_t *)whd_parse_dot11_tlvs(cp, len, DOT11_IE_ID_COUNTRY);
    if ( (country_info_ie != NULL) && (country_info_ie->tlv_header.length >= COUNTRY_INFO_IE_MINIMUM_LENGTH) )
    {
        record->ccode[0] = UNSIGNED_CHAR_TO_CHAR(country_info_ie->ccode[0]);
        record->ccode[1] = UNSIGNED_CHAR_TO_CHAR(country_info_ie->ccode[1]);
    }
    record->band = (CHSPEC_IS2G(chanspec) ? WHD_802_11_BAND_2_4GHZ :
                    (CHSPEC_IS5G(chanspec) ? WHD_802_11_BAND_5GHZ : WHD_802_11_BAND_6GHZ) );

    whd_driver->internal_info.scan_result_callback(&whd_driver->internal_info.whd_scan_result_ptr, handler_user_data,
                                                   WHD_SCAN_INCOMPLETE);

    /* whd_driver->internal_info.scan_result_callback() can make whd_scan_result_ptr to NULL */
    if (whd_driver->internal_info.whd_scan_result_ptr == NULL)
    {
        whd_driver->internal_info.scan_result_callback(NULL, handler_user_data, WHD_SCAN_ABORTED);
        whd_driver->internal_info.scan_result_callback = NULL;
        whd_wifi_deregister_event_handler(ifp, ifp->event_reg_list[WHD_SCAN_EVENT_ENTRY]);
        ifp->event_reg_list[WHD_SCAN_EVENT_ENTRY] = WHD_EVENT_NOT_REGISTERED;
    }

    return handler_user_data;
}

/** Handles auth result events
 *
 *  This function receives scan record events, and parses them into a better format, then passes the results
 *  to the user application.
 *
 * @param event_header     : The event details
 * @param event_data       : The data for the event which contains the auth result structure
 * @param handler_user_data: data which will be passed to user application
 *
 * @returns : handler_user_data parameter
 *
 */
static void *whd_wifi_auth_events_handler(whd_interface_t ifp, const whd_event_header_t *event_header,
                                          const uint8_t *event_data,
                                          void *handler_user_data)
{
    whd_driver_t whd_driver = ifp->whd_driver;
    whd_scan_result_t *record;

    if (whd_driver->internal_info.auth_result_callback == NULL)
    {
        WPRINT_WHD_ERROR( ("No set callback function in %s at %d \n", __func__, __LINE__) );
        return handler_user_data;
    }
    if (event_header->event_type == WLC_E_EXT_AUTH_REQ)
    {
        uint8_t flag = 0;
        if (whd_driver->internal_info.whd_scan_result_ptr)
        {
            record = (whd_scan_result_t *)(whd_driver->internal_info.whd_scan_result_ptr);
            if (record->flags & WHD_SCAN_RESULT_FLAG_SAE_H2E)
                flag = 1;
            else
                flag = 0;

        }
        whd_driver->internal_info.auth_result_callback( (void *)event_data, sizeof(whd_auth_req_status_t),
                                                        WHD_AUTH_EXT_REQ, &flag, handler_user_data );
        return handler_user_data;
    }
    else if (event_header->event_type == WLC_E_EXT_AUTH_FRAME_RX)
    {
        uint32_t mgmt_frame_len;
        wl_rx_mgmt_data_t *rxframe;
        uint8_t *frame;

        mgmt_frame_len = event_header->datalen - sizeof(wl_rx_mgmt_data_t);
        rxframe = (wl_rx_mgmt_data_t *)event_data;
        frame =  (uint8_t *)(rxframe + 1);
        whd_driver->internal_info.auth_result_callback( (void *)frame, mgmt_frame_len, WHD_AUTH_EXT_FRAME_RX, NULL,
                                                        handler_user_data );
        return handler_user_data;

    }

    return handler_user_data;
}

static void whd_scan_count_handler(whd_scan_result_t **result_ptr, void *user_data, whd_scan_status_t status)
{
    uint32_t result;
    whd_scan_userdata_t *scan_userdata = (whd_scan_userdata_t *)user_data;

    /* finished scan, either successfully or through an abort */
    if (status != WHD_SCAN_INCOMPLETE)
    {
        DISABLE_COMPILER_WARNING(diag_suppress = Pa039)
        result = cy_rtos_set_semaphore(&scan_userdata->scan_semaphore, WHD_FALSE);
        ENABLE_COMPILER_WARNING(diag_suppress = Pa039)
        if (result != WHD_SUCCESS)
            WPRINT_WHD_ERROR( ("Set semaphore failed in %s at %d \n", __func__, __LINE__) );
        return;
    }

    /* just count the available networks */
    scan_userdata->offset += 1;

    memset(*result_ptr, 0, sizeof(whd_scan_result_t) );
    return;
}

static void whd_scan_result_handler(whd_scan_result_t **result_ptr, void *user_data, whd_scan_status_t status)
{
    uint32_t result;
    whd_sync_scan_result_t *record;
    whd_scan_userdata_t *scan_userdata = (whd_scan_userdata_t *)user_data;
    whd_scan_result_t *current_result;

    /* Safe to access *scan_userdata. This static function registered only from whd_wifi_scan_synch and
     * not exposed for general use. The user_data is valid when passed in from whd_wifi_scan_synch.
     */

    /* finished scan, either successfully or through an abort */
    if (status != WHD_SCAN_INCOMPLETE)
    {
        DISABLE_COMPILER_WARNING(diag_suppress = Pa039)
        result =  cy_rtos_set_semaphore(&scan_userdata->scan_semaphore, WHD_FALSE);
        ENABLE_COMPILER_WARNING(diag_suppress = Pa039)
        if (result != WHD_SUCCESS)
            WPRINT_WHD_ERROR( ("Set semaphore failed in %s at %d \n", __func__, __LINE__) );
        return;
    }

    /* can't really keep anymore scan results */
    if (scan_userdata->offset == scan_userdata->count)
    {
        /*Offset and the count requested is reached. return with out saving the record details */
        memset(*result_ptr, 0, sizeof(whd_scan_result_t) );
        return;
    }

    /* Safe to access *result_ptr as result_ptr should only be NULL if the scan has completed or
     * been aborted, which is handled above
     */
    current_result = (whd_scan_result_t *)(*result_ptr);

    DISABLE_COMPILER_WARNING(diag_suppress = Pa039)
    /* Safe to access *scan_userdata, as noted above */
    record = (whd_sync_scan_result_t *)(&scan_userdata->aps[scan_userdata->offset]);
    ENABLE_COMPILER_WARNING(diag_suppress = Pa039)

    /* Copy the SSID into the output record structure */
    record->SSID.length = current_result->SSID.length;
    memset(record->SSID.value, 0, sizeof(record->SSID.value) );
    memcpy(record->SSID.value, current_result->SSID.value, record->SSID.length);

    /* Copy the BSSID into the output record structure */
    memcpy( (void *)record->BSSID.octet, (const void *)current_result->BSSID.octet,
            sizeof(current_result->BSSID.octet) );

    record->security = current_result->security;
    record->signal_strength = current_result->signal_strength;
    record->channel = current_result->channel;

    scan_userdata->offset += 1;
    memset(*result_ptr, 0, sizeof(whd_scan_result_t) );
    return;
}

whd_result_t whd_wifi_scan_synch(whd_interface_t ifp,
                             whd_sync_scan_result_t *scan_result,
                             uint32_t *count
                             )
{
    uint32_t result;
    whd_scan_result_t *scan_result_ptr;
    whd_scan_userdata_t scan_userdata;
    scan_userdata.count = *count;
    scan_userdata.aps = scan_result;
    scan_userdata.offset = 0;

    if (!ifp || !scan_result)
    {
        WPRINT_WHD_ERROR( ("Invalid param in func %s at line %d \n",
                           __func__, __LINE__) );
        return WHD_WLAN_BADARG;
    }

    DISABLE_COMPILER_WARNING(diag_suppress = Pa039)
    CHECK_RETURN(cy_rtos_init_semaphore(&scan_userdata.scan_semaphore, 1, 0) );
    ENABLE_COMPILER_WARNING(diag_suppress = Pa039)

    whd_scan_result_callback_t handler = (*count == 0)
                                         ? whd_scan_count_handler : whd_scan_result_handler;

    scan_result_ptr = (whd_scan_result_t *)whd_mem_malloc(sizeof(whd_scan_result_t) );
    if (scan_result_ptr == NULL)
    {
        goto error;
    }
    memset(scan_result_ptr, 0, sizeof(whd_scan_result_t) );

    if (whd_wifi_scan(ifp, WHD_SCAN_TYPE_ACTIVE, WHD_BSS_TYPE_ANY, NULL, NULL, NULL, NULL,
                      handler, (whd_scan_result_t *)scan_result_ptr, &scan_userdata) != WHD_SUCCESS)
    {
        WPRINT_WHD_INFO( ("Failed scan \n") );
        goto error;
    }

    DISABLE_COMPILER_WARNING(diag_suppress = Pa039)
    result = cy_rtos_get_semaphore(&scan_userdata.scan_semaphore, CY_RTOS_NEVER_TIMEOUT, WHD_FALSE);
    ENABLE_COMPILER_WARNING(diag_suppress = Pa039)
    whd_assert("Get semaphore failed", (result == CY_RSLT_SUCCESS) || (result == CY_RTOS_TIMEOUT) );

    DISABLE_COMPILER_WARNING(diag_suppress = Pa039)
    result = cy_rtos_deinit_semaphore(&scan_userdata.scan_semaphore);
    ENABLE_COMPILER_WARNING(diag_suppress = Pa039)
    if (WHD_SUCCESS != result)
    {
        WPRINT_WHD_INFO( ("Failed join (err %" PRIu32 ")\n", result) );
    }
    if (scan_result_ptr != NULL)
    {
        whd_mem_free(scan_result_ptr);
        scan_result_ptr = NULL;
    }
    *count = scan_userdata.offset;

    return WHD_SUCCESS;

error:
    if (scan_result_ptr != NULL)
    {
        whd_mem_free(scan_result_ptr);
        scan_result_ptr = NULL;
    }

    return WHD_MALLOC_FAILURE;
}

/*
 * NOTE: search references of function wlu_get in wl/exe/wlu.c to find what format the returned IOCTL data is.
 */
whd_result_t whd_wifi_scan(whd_interface_t ifp,
                       whd_scan_type_t scan_type,
                       whd_bss_type_t bss_type,
                       const whd_ssid_t *optional_ssid,
                       const whd_mac_t *optional_mac,
                       const uint16_t *optional_channel_list,
                       const whd_scan_extended_params_t *optional_extended_params,
                       whd_scan_result_callback_t callback,
                       whd_scan_result_t *result_ptr,
                       void *user_data
                       )
{
    whd_buffer_t buffer;
    wl_escan_params_t *scan_params;
    uint16_t param_size = offsetof(wl_escan_params_t, params) + WL_SCAN_PARAMS_FIXED_SIZE;
    uint16_t channel_list_size = 0;
    whd_driver_t whd_driver = ifp->whd_driver;
    uint16_t event_entry = 0xFF;

    whd_assert("Bad args", callback != NULL);

    if ( (result_ptr == NULL) || (callback == NULL) )
    {
        return WHD_BADARG;
    }

    if (!( (scan_type == WHD_SCAN_TYPE_ACTIVE) || (scan_type == WHD_SCAN_TYPE_PASSIVE) ||
           (scan_type == WHD_SCAN_TYPE_PROHIBITED_CHANNELS) || (scan_type == WHD_SCAN_TYPE_NO_BSSID_FILTER) ) )
        return WHD_BADARG;

    if (!( (bss_type == WHD_BSS_TYPE_INFRASTRUCTURE) || (bss_type == WHD_BSS_TYPE_ADHOC) ||
           (bss_type == WHD_BSS_TYPE_ANY) ) )
        return WHD_BADARG;

    /* Determine size of channel_list, and add it to the parameter size so correct sized buffer can be allocated */
    if (optional_channel_list != NULL)
    {
        /* Look for entry with channel number 0, which suggests the end of channel_list */
        for (channel_list_size = 0; optional_channel_list[channel_list_size] != 0; channel_list_size++)
        {
        }
        param_size = ( uint16_t )(param_size + channel_list_size * sizeof(uint16_t) );
    }

    if (ifp->event_reg_list[WHD_SCAN_EVENT_ENTRY] != WHD_EVENT_NOT_REGISTERED)
    {
        whd_wifi_deregister_event_handler(ifp, ifp->event_reg_list[WHD_SCAN_EVENT_ENTRY]);
        ifp->event_reg_list[WHD_SCAN_EVENT_ENTRY] = WHD_EVENT_NOT_REGISTERED;
    }
    CHECK_RETURN(whd_management_set_event_handler(ifp, scan_events, whd_wifi_scan_events_handler, user_data,
                                                  &event_entry) );
    if (event_entry >= WHD_MAX_EVENT_SUBSCRIPTION)
    {
        WPRINT_WHD_ERROR( ("scan_events registration failed in function %s and line %d", __func__, __LINE__) );
        return WHD_UNFINISHED;
    }
    ifp->event_reg_list[WHD_SCAN_EVENT_ENTRY] = event_entry;
    /* Allocate a buffer for the IOCTL message */
    scan_params = (wl_escan_params_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer, param_size, IOVAR_STR_ESCAN);
    CHECK_IOCTL_BUFFER(scan_params);

    /* Clear the scan parameters structure */
    memset(scan_params, 0, param_size);

    /* Fill in the appropriate details of the scan parameters structure */
    scan_params->version = htod32(ESCAN_REQ_VERSION);
    scan_params->action = htod16(WL_SCAN_ACTION_START);
    scan_params->params.scan_type = (int8_t)scan_type;
    scan_params->params.bss_type = (int8_t)bss_type;

    /* Fill out the SSID parameter if provided */
    if (optional_ssid != NULL)
    {
        scan_params->params.ssid.SSID_len = htod32(optional_ssid->length);
        memcpy(scan_params->params.ssid.SSID, optional_ssid->value, scan_params->params.ssid.SSID_len);
    }

    /* Fill out the BSSID parameter if provided */
    if (optional_mac != NULL)
    {
        memcpy(scan_params->params.bssid.octet, optional_mac, sizeof(whd_mac_t) );
    }
    else
    {
        memset(scan_params->params.bssid.octet, 0xff, sizeof(whd_mac_t) );
    }

    /* Fill out the extended parameters if provided */
    if (optional_extended_params != NULL)
    {
        scan_params->params.nprobes = (int32_t)htod32(optional_extended_params->number_of_probes_per_channel);
        scan_params->params.active_time =
            (int32_t)htod32(optional_extended_params->scan_active_dwell_time_per_channel_ms);
        scan_params->params.passive_time = (int32_t)htod32(
            optional_extended_params->scan_passive_dwell_time_per_channel_ms);
        scan_params->params.home_time = (int32_t)htod32(
            optional_extended_params->scan_home_channel_dwell_time_between_channels_ms);
    }
    else
    {
#ifndef PROTO_MSGBUF
        scan_params->params.nprobes = (int32_t)htod32(-1);
        scan_params->params.active_time = (int32_t)htod32(-1);
        scan_params->params.passive_time = (int32_t)htod32(-1);
        scan_params->params.home_time = (int32_t)htod32(-1);
#else
        memset(&(scan_params->params.nprobes), -1, 16);
#endif
    }

    /* Copy the channel list parameter if provided */
    if ( (channel_list_size > 0) && (optional_channel_list != NULL) )
    {
        int i;

        for (i = 0; i < channel_list_size; i++)
        {
            scan_params->params.channel_list[i] = htod16(CH20MHZ_CHSPEC(optional_channel_list[i]) );
        }
        scan_params->params.channel_num = (int32_t)htod32(channel_list_size);
    }

    whd_driver->internal_info.scan_result_callback = callback;
    whd_driver->internal_info.whd_scan_result_ptr = result_ptr;

    /* Send the Incremental Scan IOVAR message - blocks until the response is received */

    CHECK_RETURN(whd_proto_set_iovar(ifp, buffer, 0) );

    return WHD_SUCCESS;
}

whd_result_t whd_wifi_stop_scan(whd_interface_t ifp)
{
    whd_buffer_t buffer;
    wl_escan_params_t *scan_params;
    whd_driver_t whd_driver;

    CHECK_IFP_NULL(ifp);
    whd_driver = ifp->whd_driver;
    CHECK_DRIVER_NULL(whd_driver)

    /* Allocate a buffer for the IOCTL message */
    scan_params = (wl_escan_params_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer, sizeof(wl_escan_params_t),
                                                                  IOVAR_STR_ESCAN);
    CHECK_IOCTL_BUFFER(scan_params);
    /* Clear the scan parameters structure */
    memset(scan_params, 0, sizeof(wl_escan_params_t) );

    /* Fill in the appropriate details of the scan parameters structure */
    scan_params->version = htod32(ESCAN_REQ_VERSION);
    scan_params->action = htod16(WL_SCAN_ACTION_ABORT);

    /* Send the Scan IOVAR message to abort scan*/
    CHECK_RETURN(whd_proto_set_iovar(ifp, buffer, 0) );

    return WHD_SUCCESS;
}

whd_result_t whd_wifi_external_auth_request(whd_interface_t ifp,
                                        whd_auth_result_callback_t callback,
                                        void *result_ptr,
                                        void *user_data
                                        )
{
    CHECK_IFP_NULL(ifp);
    whd_driver_t whd_driver = ifp->whd_driver;
    uint16_t event_entry = 0xFF;

    whd_assert("Bad args", callback != NULL);

    if (ifp->event_reg_list[WHD_AUTH_EVENT_ENTRY] != WHD_EVENT_NOT_REGISTERED)
    {
        whd_wifi_deregister_event_handler(ifp, ifp->event_reg_list[WHD_AUTH_EVENT_ENTRY]);
        ifp->event_reg_list[WHD_AUTH_EVENT_ENTRY] = WHD_EVENT_NOT_REGISTERED;
    }
    CHECK_RETURN(whd_management_set_event_handler(ifp, auth_events, whd_wifi_auth_events_handler, user_data,
                                                  &event_entry) );
    if (event_entry >= WHD_MAX_EVENT_SUBSCRIPTION)
    {
        WPRINT_WHD_ERROR( ("auth_events registration failed in function %s and line %d", __func__, __LINE__) );
        return WHD_UNFINISHED;
    }
    ifp->event_reg_list[WHD_AUTH_EVENT_ENTRY] = event_entry;


    whd_driver->internal_info.auth_result_callback = callback;

    return WHD_SUCCESS;
}

whd_result_t whd_wifi_stop_external_auth_request(whd_interface_t ifp)
{
    whd_driver_t whd_driver;

    CHECK_IFP_NULL(ifp);
    whd_driver = ifp->whd_driver;
    CHECK_DRIVER_NULL(whd_driver)

    if (ifp->event_reg_list[WHD_AUTH_EVENT_ENTRY] != WHD_EVENT_NOT_REGISTERED)
    {
        whd_wifi_deregister_event_handler(ifp, ifp->event_reg_list[WHD_AUTH_EVENT_ENTRY]);
        ifp->event_reg_list[WHD_AUTH_EVENT_ENTRY] = WHD_EVENT_NOT_REGISTERED;
    }
    whd_driver->internal_info.auth_result_callback = NULL;
    return WHD_SUCCESS;
}

whd_result_t  whd_wifi_deauth_sta(whd_interface_t ifp, whd_mac_t *mac, whd_dot11_reason_code_t reason)
{
    whd_result_t result;
    scb_val_t *scb_val;
    whd_buffer_t buffer1;
    whd_driver_t whd_driver;

    CHECK_IFP_NULL(ifp);

    whd_driver = ifp->whd_driver;

    if (mac == NULL)
    {
        uint8_t *buffer = NULL;
        whd_maclist_t *clients = NULL;
        const whd_mac_t *current;
        wl_bss_info_t ap_info;
        whd_security_t sec;
        uint32_t max_clients = 0;
        size_t size = 0;

        result = whd_wifi_ap_get_max_assoc(ifp, &max_clients);
        if (result != WHD_SUCCESS)
        {
            WPRINT_WHD_ERROR( ("Failed to get max number of associated clients\n") );
            max_clients = 5;
        }

        size = (sizeof(uint32_t) + (max_clients * sizeof(whd_mac_t) ) );
        buffer = whd_mem_calloc(1, size);

        if (buffer == NULL)
        {
            WPRINT_WHD_ERROR( ("Unable to allocate memory for associated clients list, %s failed at line %d \n",
                               __func__, __LINE__) );
            return WHD_MALLOC_FAILURE;
        }

        clients = (whd_maclist_t *)buffer;
        clients->count = max_clients;
        memset(&ap_info, 0, sizeof(wl_bss_info_t) );

        result = whd_wifi_get_associated_client_list(ifp, clients, (uint16_t)size);
        if (result != WHD_SUCCESS)
        {
            WPRINT_WHD_ERROR( ("Failed to get client list, %s failed at line %d \n", __func__, __LINE__) );
            whd_mem_free(buffer);
            return result;
        }

        current = &clients->mac_list[0];
        result = whd_wifi_get_ap_info(ifp, &ap_info, &sec);
        if (result != WHD_SUCCESS)
        {
            WPRINT_WHD_ERROR( ("Function %s failed at line %d \n", __func__, __LINE__) );
            whd_mem_free(buffer);
            return result;
        }


        while ( (clients->count > 0) && (!(NULL_MAC(current->octet) ) ) )
        {
            if (memcmp(current->octet, &(ap_info.BSSID), sizeof(whd_mac_t) ) != 0)
            {
                WPRINT_WHD_INFO( ("Deauthenticating STA MAC: %.2X:%.2X:%.2X:%.2X:%.2X:%.2X\n", current->octet[0],
                                  current->octet[1], current->octet[2], current->octet[3], current->octet[4],
                                  current->octet[5]) );

                scb_val = (scb_val_t *)whd_proto_get_ioctl_buffer(whd_driver, &buffer1, sizeof(scb_val_t) );
                if (scb_val == NULL)
                {
                    WPRINT_WHD_ERROR( ("Buffer alloc failed in function %s at line %d \n", __func__, __LINE__) );
                    whd_mem_free(buffer);
                    return WHD_BUFFER_ALLOC_FAIL;
                }
                memset( (char *)scb_val, 0, sizeof(scb_val_t) );
                memcpy( (char *)&scb_val->ea, (char *)current, sizeof(whd_mac_t) );
                scb_val->val = (uint32_t)reason;
                result = whd_proto_set_ioctl(ifp, WLC_SCB_DEAUTHENTICATE_FOR_REASON, buffer1, 0);

                if (result != WHD_SUCCESS)
                {
                    WPRINT_WHD_ERROR( ("Failed to deauth client\n") );
                }
            }

            --clients->count;
            ++current;
        }

        whd_mem_free(buffer);

        return WHD_SUCCESS;
    }

    scb_val = (scb_val_t *)whd_proto_get_ioctl_buffer(whd_driver, &buffer1, sizeof(scb_val_t) );
    CHECK_IOCTL_BUFFER(scb_val);
    memset( (char *)scb_val, 0, sizeof(scb_val_t) );
    memcpy( (char *)&scb_val->ea, (char *)mac, sizeof(whd_mac_t) );
    scb_val->val = (uint32_t)reason;
    CHECK_RETURN(whd_proto_set_ioctl(ifp, WLC_SCB_DEAUTHENTICATE_FOR_REASON, buffer1, 0) );

    return WHD_SUCCESS;
}

whd_result_t whd_wifi_get_mac_address(whd_interface_t ifp, whd_mac_t *mac)
{
    whd_buffer_t buffer;
    whd_buffer_t response;
    whd_driver_t whd_driver;

    CHECK_IFP_NULL(ifp);

    if (mac == NULL)
        return WHD_BADARG;

    whd_driver = ifp->whd_driver;

    CHECK_DRIVER_NULL(whd_driver);

    CHECK_IOCTL_BUFFER(whd_proto_get_iovar_buffer(whd_driver, &buffer, sizeof(whd_mac_t), IOVAR_STR_CUR_ETHERADDR) );

    CHECK_RETURN(whd_proto_get_iovar(ifp, buffer, &response) );

    memcpy(mac, whd_buffer_get_current_piece_data_pointer(whd_driver, response), sizeof(whd_mac_t) );
    CHECK_RETURN(whd_buffer_release(whd_driver, response, WHD_NETWORK_RX) );

    return WHD_SUCCESS;
}

whd_result_t whd_wifi_get_bssid(whd_interface_t ifp, whd_mac_t *bssid)
{
    whd_buffer_t buffer;
    whd_buffer_t response;
    whd_result_t result;
    whd_driver_t whd_driver;
    uint8_t  *data = NULL;
    CHECK_IFP_NULL(ifp);

    if (bssid == NULL)
        return WHD_BADARG;

    whd_driver = ifp->whd_driver;

    CHECK_DRIVER_NULL(whd_driver);

    if ( (ifp->role == WHD_STA_ROLE) || (ifp->role == WHD_AP_ROLE) )
    {
        memset(bssid, 0, sizeof(whd_mac_t) );
        CHECK_IOCTL_BUFFER(whd_proto_get_ioctl_buffer(whd_driver, &buffer, sizeof(whd_mac_t) ) );
        if ( (result =
                  whd_proto_get_ioctl(ifp, WLC_GET_BSSID, buffer, &response) ) == WHD_SUCCESS )
        {
            data = whd_buffer_get_current_piece_data_pointer(whd_driver, response);
            CHECK_PACKET_NULL(data, WHD_NO_REGISTER_FUNCTION_POINTER);
            memcpy(bssid->octet, data, sizeof(whd_mac_t) );
            CHECK_RETURN(whd_buffer_release(whd_driver, response, WHD_NETWORK_RX) );
        }
        return result;
    }
    else if (ifp->role == WHD_INVALID_ROLE)
    {
        WPRINT_WHD_ERROR( ("STA not associated with AP\n") );
        return WHD_WLAN_NOTASSOCIATED;
    }
    else
    {
        return WHD_UNSUPPORTED;
    }
}

whd_result_t whd_wifi_ap_get_max_assoc(whd_interface_t ifp, uint32_t *max_assoc)
{
    if (!ifp || !max_assoc)
    {
        WPRINT_WHD_ERROR( ("Invalid param in func %s at line %d \n",
                           __func__, __LINE__) );
        return WHD_WLAN_BADARG;
    }

    return whd_wifi_get_iovar_value(ifp, IOVAR_STR_MAX_ASSOC, max_assoc);
}

whd_result_t whd_wifi_get_associated_client_list(whd_interface_t ifp, void *client_list_buffer, uint16_t buffer_length)
{
    whd_buffer_t buffer;
    whd_buffer_t response;
    whd_result_t result;
    whd_maclist_t *data = NULL;
    uint8_t *pdata = NULL;
    whd_driver_t whd_driver;

    CHECK_IFP_NULL(ifp);

    whd_driver = ifp->whd_driver;

    CHECK_DRIVER_NULL(whd_driver);

    /* Check if soft AP interface is up, if not, return a count of 0 as a result */
    result = whd_wifi_is_ready_to_transceive(ifp);
    if ( (result == WHD_SUCCESS) && (ifp->role == WHD_AP_ROLE) )
    {
        data = (whd_maclist_t *)whd_proto_get_ioctl_buffer(whd_driver, &buffer, buffer_length);
        CHECK_IOCTL_BUFFER(data);
        memset(data, 0, buffer_length);
        data->count = htod32( ( (whd_maclist_t *)client_list_buffer )->count );

        CHECK_RETURN(whd_proto_get_ioctl(ifp, WLC_GET_ASSOCLIST, buffer, &response) );
        pdata = whd_buffer_get_current_piece_data_pointer(whd_driver,  response);
        CHECK_PACKET_NULL(pdata, WHD_NO_REGISTER_FUNCTION_POINTER);
        memcpy(client_list_buffer, (void *)pdata,
               (size_t)MIN_OF(whd_buffer_get_current_piece_size(whd_driver, response), buffer_length) );

        CHECK_RETURN(whd_buffer_release(whd_driver, response, WHD_NETWORK_RX) );
    }
    else if (result == WHD_INTERFACE_NOT_UP)
    {
        /* not up, so can't have associated clients */
        ( (whd_maclist_t *)client_list_buffer )->count = 0;
    }
    else
    {
        WPRINT_WHD_ERROR( ("Invalid Interface\n") );
        return WHD_INVALID_INTERFACE;
    }
    return result;
}

whd_result_t whd_wifi_get_ap_info(whd_interface_t ifp, wl_bss_info_t *ap_info, whd_security_t *security)
{
    whd_buffer_t buffer;
    whd_buffer_t response;
    uint32_t *data;
    uint8_t  *pdata = NULL;
    uint32_t security_value; /* hold misc security values */
    whd_driver_t whd_driver;
    CHECK_IFP_NULL(ifp);

    if ( (ap_info == NULL) || (security == NULL) )
        return WHD_BADARG;

    whd_driver = ifp->whd_driver;

    CHECK_DRIVER_NULL(whd_driver);
    /* Read the BSS info */
    data = (uint32_t *)whd_proto_get_ioctl_buffer(whd_driver, &buffer, WLC_IOCTL_SMLEN);
    CHECK_IOCTL_BUFFER(data);
    *data = WLC_IOCTL_SMLEN;
    CHECK_RETURN(whd_proto_get_ioctl(ifp, WLC_GET_BSS_INFO, buffer, &response) );
    pdata = whd_buffer_get_current_piece_data_pointer(whd_driver, response);
    CHECK_PACKET_NULL(pdata, WHD_NO_REGISTER_FUNCTION_POINTER);
    memcpy(ap_info, (void *)(pdata + 4), sizeof(wl_bss_info_t) );
    CHECK_RETURN(whd_buffer_release(whd_driver, response, WHD_NETWORK_RX) );

    /* Read the WSEC setting */
    CHECK_RETURN(whd_wifi_get_ioctl_value(ifp, WLC_GET_WSEC, &security_value) );
    security_value = security_value & SECURITY_MASK;
    *security = ( whd_security_t )(security_value);

    if (*security == WHD_SECURITY_WEP_PSK)
    {
        /* Read the WEP auth setting */
        CHECK_RETURN(whd_wifi_get_ioctl_value(ifp, WLC_GET_AUTH, &security_value) );

        if (security_value == SHARED_AUTH)
        {
            *security |= SHARED_ENABLED;
        }
    }
    else if ( (*security & (TKIP_ENABLED | AES_ENABLED) ) != 0 )
    {
        /* Read the WPA auth setting */
        CHECK_RETURN(whd_wifi_get_ioctl_value(ifp, WLC_GET_WPA_AUTH, &security_value) );

        if (security_value == WPA2_AUTH_PSK)
        {
            *security |= WPA2_SECURITY;
        }
        else if (security_value == WPA_AUTH_PSK)
        {
            *security |= WPA_SECURITY;
        }
    }
    else if (*security != WHD_SECURITY_OPEN)
    {
        *security = WHD_SECURITY_UNKNOWN;
        WPRINT_WHD_ERROR( ("Unknown security type, %s failed at line %d \n", __func__, __LINE__) );
        return WHD_UNKNOWN_SECURITY_TYPE;
    }

    return WHD_SUCCESS;
}

whd_result_t whd_wifi_enable_powersave(whd_interface_t ifp)
{
    whd_buffer_t buffer;
    uint32_t *data;
    whd_driver_t whd_driver;
    CHECK_IFP_NULL(ifp);

    whd_driver = ifp->whd_driver;

    CHECK_DRIVER_NULL(whd_driver);

    /* Set legacy powersave mode - PM1 */
    data = (uint32_t *)whd_proto_get_ioctl_buffer(whd_driver, &buffer, (uint16_t)4);
    CHECK_IOCTL_BUFFER(data);
    *data = htod32( (uint32_t)PM1_POWERSAVE_MODE );

    RETURN_WITH_ASSERT(whd_proto_set_ioctl(ifp, WLC_SET_PM, buffer, NULL) );
}

whd_result_t whd_wifi_get_powersave_mode(whd_interface_t ifp, uint32_t *value)
{
    whd_driver_t whd_driver;

    CHECK_IFP_NULL(ifp);

    if (value == NULL)
        return WHD_BADARG;

    whd_driver = ifp->whd_driver;

    CHECK_DRIVER_NULL(whd_driver);

    return whd_wifi_get_ioctl_value(ifp, WLC_GET_PM, value);
}

#ifdef CYCFG_ULP_SUPPORT_ENABLED
whd_result_t whd_wifi_config_ulp_mode(whd_interface_t ifp, uint32_t *mode, uint32_t *wait_time)
{
    whd_driver_t whd_driver;
    uint32_t get_ulp_mode = 0;
    uint16_t wlan_chip_id = 0;

    CHECK_IFP_NULL(ifp);

    whd_driver = ifp->whd_driver;
    CHECK_DRIVER_NULL(whd_driver);

    wlan_chip_id = whd_chip_get_chip_id(ifp->whd_driver);

    if (wlan_chip_id == 43022)
    {
        WPRINT_WHD_DEBUG(("Connected Chip supports ULP \n"));

        if((*mode == ULP_DS1_SUPPORT) || (*mode == ULP_DS2_SUPPORT))
        {

            CHECK_RETURN(whd_wifi_set_iovar_value(ifp, IOVAR_STR_ULP_WAIT, *wait_time));

            whd_wifi_get_iovar_value(ifp, IOVAR_STR_ULP, &get_ulp_mode);
            if(get_ulp_mode == 0)
            {
                /* Enable wowl magic pattern wake bit */
                CHECK_RETURN(whd_configure_wowl(ifp, ( WL_WOWL_MAGIC | WL_WOWL_ARPOFFLOAD | WL_WOWL_DEAUTH )));
                /* Set ulp mode */
                CHECK_RETURN(whd_wifi_set_iovar_value(ifp, IOVAR_STR_ULP, *mode));
            }
            else
            {
                WPRINT_WHD_DEBUG(("ULP mode already set to %d \n", (int)get_ulp_mode));
            }
        }
        else if(*mode == ULP_NO_SUPPORT)
        {
            CHECK_RETURN(whd_wifi_set_iovar_value(ifp, IOVAR_STR_ULP, *mode));
        }
        else
        {
            WPRINT_WHD_ERROR(("Given ULP Configuration mode is not supported\n"));
            return WHD_BADARG;
        }
    }
    else
    {
        WPRINT_WHD_ERROR(("Connected Chip doesn't support ULP \n"));
        return WHD_BADARG;
    }

    return WHD_SUCCESS;
}
#endif

whd_result_t whd_wifi_enable_powersave_with_throughput(whd_interface_t ifp, uint16_t return_to_sleep_delay_ms)
{
    whd_buffer_t buffer;
    uint32_t *data;
    whd_driver_t whd_driver;
    uint16_t chip_id;
    CHECK_IFP_NULL(ifp);

    whd_driver = ifp->whd_driver;

    CHECK_DRIVER_NULL(whd_driver);

    if (return_to_sleep_delay_ms < PM2_SLEEP_RET_TIME_MIN)
    {
        WPRINT_WHD_ERROR( ("Delay too short, %s failed at line %d \n", __func__, __LINE__) );
        return WHD_DELAY_TOO_SHORT;
    }
    else if (return_to_sleep_delay_ms > PM2_SLEEP_RET_TIME_MAX)
    {
        WPRINT_WHD_ERROR( ("Delay too long, %s failed at line %d \n", __func__, __LINE__) );
        return WHD_DELAY_TOO_LONG;
    }

    /* Set the maximum time to wait before going back to sleep */
    CHECK_RETURN(whd_wifi_set_iovar_value(ifp, IOVAR_STR_PM2_SLEEP_RET,
                                          (uint32_t)(return_to_sleep_delay_ms / 10) * 10) );
    chip_id = whd_chip_get_chip_id(whd_driver);

    if (chip_id == 43362)
    {
        CHECK_RETURN(whd_wifi_set_iovar_value(ifp, IOVAR_STR_PM_LIMIT, NULL_FRAMES_WITH_PM_SET_LIMIT) );
    }

    /* set PM2 fast return to sleep powersave mode */
    data = (uint32_t *)whd_proto_get_ioctl_buffer(whd_driver, &buffer, (uint16_t)4);
    CHECK_IOCTL_BUFFER(data);
    *data = htod32( (uint32_t)PM2_POWERSAVE_MODE );

    RETURN_WITH_ASSERT(whd_proto_set_ioctl(ifp, WLC_SET_PM, buffer, NULL) );
}

whd_result_t whd_wifi_disable_powersave(whd_interface_t ifp)
{
    whd_buffer_t buffer;
    whd_driver_t whd_driver;

    CHECK_IFP_NULL(ifp);

    whd_driver = ifp->whd_driver;

    CHECK_DRIVER_NULL(whd_driver);

    uint32_t *data = (uint32_t *)whd_proto_get_ioctl_buffer(whd_driver, &buffer, (uint16_t)4);

    CHECK_IOCTL_BUFFER(data);
    *data = htod32( (uint32_t)NO_POWERSAVE_MODE );
    CHECK_RETURN(whd_proto_set_ioctl(ifp, WLC_SET_PM, buffer, NULL) );
    return WHD_SUCCESS;
}

whd_result_t whd_wifi_register_multicast_address(whd_interface_t ifp, const whd_mac_t *mac)
{
    whd_buffer_t buffer;
    whd_buffer_t response;
    uint16_t a;
    mcast_list_t *orig_mcast_list;
    mcast_list_t *new_mcast_list;
    whd_driver_t whd_driver;

    if (!ifp || !mac || !ETHER_ISMULTI(mac) )
    {
        WPRINT_WHD_ERROR( ("Invalid param in func %s at line %d \n",
                           __func__, __LINE__) );
        return WHD_WLAN_BADARG;
    }

    whd_driver = ifp->whd_driver;

    CHECK_DRIVER_NULL(whd_driver);

    /* Get the current multicast list */
    CHECK_IOCTL_BUFFER(whd_proto_get_iovar_buffer(whd_driver, &buffer,
                                                  sizeof(uint32_t) + MAX_SUPPORTED_MCAST_ENTRIES *
                                                  sizeof(whd_mac_t), IOVAR_STR_MCAST_LIST) );
    CHECK_RETURN(whd_proto_get_iovar(ifp, buffer, &response) );

    /* Verify address is not currently registered */
    orig_mcast_list = (mcast_list_t *)whd_buffer_get_current_piece_data_pointer(whd_driver, response);
    CHECK_PACKET_NULL(orig_mcast_list, WHD_NO_REGISTER_FUNCTION_POINTER);
    orig_mcast_list->entry_count = dtoh32(orig_mcast_list->entry_count);
    for (a = 0; a < orig_mcast_list->entry_count; ++a)
    {
        /* Check if any address matches */
        if (0 == memcmp(mac, &orig_mcast_list->macs[a], sizeof(whd_mac_t) ) )
        {
            /* A matching address has been found so we can stop now. */
            CHECK_RETURN(whd_buffer_release(whd_driver, response, WHD_NETWORK_RX) );
            return WHD_SUCCESS;
        }
    }

    /* Add the provided address to the list and write the new multicast list */
    new_mcast_list = (mcast_list_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer,
                                                                ( uint16_t )(sizeof(uint32_t) +
                                                                             (orig_mcast_list->entry_count + 1) *
                                                                             sizeof(whd_mac_t) ),
                                                                IOVAR_STR_MCAST_LIST);
    CHECK_IOCTL_BUFFER(new_mcast_list);
    new_mcast_list->entry_count = orig_mcast_list->entry_count;
    memcpy(new_mcast_list->macs, orig_mcast_list->macs, orig_mcast_list->entry_count * sizeof(whd_mac_t) );
    CHECK_RETURN(whd_buffer_release(whd_driver, response, WHD_NETWORK_RX) );
    memcpy(&new_mcast_list->macs[new_mcast_list->entry_count], mac, sizeof(whd_mac_t) );
    ++new_mcast_list->entry_count;
    new_mcast_list->entry_count = htod32(new_mcast_list->entry_count);
    RETURN_WITH_ASSERT(whd_proto_set_iovar(ifp, buffer, NULL) );

}

whd_result_t whd_wifi_unregister_multicast_address(whd_interface_t ifp, const whd_mac_t *mac)
{
    whd_buffer_t buffer;
    whd_buffer_t response;
    uint16_t a;
    mcast_list_t *orig_mcast_list;
    whd_driver_t whd_driver;

    if (!ifp || !mac || !ETHER_ISMULTI(mac) )
    {
        WPRINT_WHD_ERROR( ("Invalid param in func %s at line %d \n",
                           __func__, __LINE__) );
        return WHD_WLAN_BADARG;
    }

    whd_driver = ifp->whd_driver;

    CHECK_DRIVER_NULL(whd_driver);

    /* Get the current multicast list */
    CHECK_IOCTL_BUFFER(whd_proto_get_iovar_buffer(whd_driver, &buffer,
                                                  sizeof(uint32_t) + MAX_SUPPORTED_MCAST_ENTRIES *
                                                  sizeof(whd_mac_t), IOVAR_STR_MCAST_LIST) );
    CHECK_RETURN(whd_proto_get_iovar(ifp, buffer, &response) );

    /* Find the address, assuming it is part of the list */
    orig_mcast_list = (mcast_list_t *)whd_buffer_get_current_piece_data_pointer(whd_driver, response);
    orig_mcast_list->entry_count = dtoh32(orig_mcast_list->entry_count);
    if (orig_mcast_list->entry_count != 0)
    {
        mcast_list_t *new_mcast_list = (mcast_list_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer,
                                                                                  ( uint16_t )(sizeof(uint32_t) +
                                                                                               (orig_mcast_list->
                                                                                                entry_count - 1) *
                                                                                               sizeof(whd_mac_t) ),
                                                                                  IOVAR_STR_MCAST_LIST);
        CHECK_IOCTL_BUFFER(new_mcast_list);

        for (a = 0; a < orig_mcast_list->entry_count; ++a)
        {
            WPRINT_WHD_INFO( ("MAC: %.2X:%.2X:%.2X:%.2X:%.2X:%.2X\n", orig_mcast_list->macs[a].octet[0],
                              orig_mcast_list->macs[a].octet[1], orig_mcast_list->macs[a].octet[2],
                              orig_mcast_list->macs[a].octet[3], orig_mcast_list->macs[a].octet[4],
                              orig_mcast_list->macs[a].octet[5]) );
            if (0 == memcmp(mac, &orig_mcast_list->macs[a], sizeof(whd_mac_t) ) )
            {
                /* Copy the existing list up to the matching address */
                memcpy(new_mcast_list->macs, orig_mcast_list->macs, a * sizeof(whd_mac_t) );

                /* Skip the current address and copy the remaining entries */
                memcpy(&new_mcast_list->macs[a], &orig_mcast_list->macs[a + 1],
                       ( size_t )(orig_mcast_list->entry_count - a - 1) * sizeof(whd_mac_t) );

                new_mcast_list->entry_count = orig_mcast_list->entry_count - 1;
                CHECK_RETURN(whd_buffer_release(whd_driver, response, WHD_NETWORK_RX) );
                new_mcast_list->entry_count = htod32(new_mcast_list->entry_count);
                RETURN_WITH_ASSERT(whd_proto_set_iovar(ifp, buffer, NULL) );
            }
        }
        /* There was something in the list, but the request MAC wasn't there */
        CHECK_RETURN(whd_buffer_release(whd_driver, buffer, WHD_NETWORK_TX) );
    }
    /* If we get here than the address wasn't in the list or the list was empty */
    CHECK_RETURN(whd_buffer_release(whd_driver, response, WHD_NETWORK_RX) );
    WPRINT_WHD_ERROR( ("whd_wifi_unregister_multicast_address address not registered yet \n") );
    return WHD_DOES_NOT_EXIST;
}

whd_result_t whd_wifi_set_listen_interval(whd_interface_t ifp, uint8_t listen_interval,
                                      whd_listen_interval_time_unit_t time_unit)
{
    uint8_t listen_interval_dtim;

    CHECK_IFP_NULL(ifp);

    switch (time_unit)
    {
        case WHD_LISTEN_INTERVAL_TIME_UNIT_DTIM:
        {
            listen_interval_dtim = listen_interval;
            break;
        }
        case WHD_LISTEN_INTERVAL_TIME_UNIT_BEACON:
        {
            /* If the wake interval measured in DTIMs is set to 0, the wake interval is measured in beacon periods */
            listen_interval_dtim = 0;

            /* The wake period is measured in beacon periods, set the value as required */
            CHECK_RETURN(whd_wifi_set_iovar_value(ifp, IOVAR_STR_LISTEN_INTERVAL_BEACON, listen_interval) );
            break;
        }
        default:
            WPRINT_WHD_ERROR( ("whd_wifi_set_listen_interval: Invalid Time unit specified \n") );
            return WHD_BADARG;
    }

    CHECK_RETURN(whd_wifi_set_iovar_value(ifp, IOVAR_STR_LISTEN_INTERVAL_DTIM, listen_interval_dtim) );

    CHECK_RETURN(whd_wifi_set_iovar_value(ifp, IOVAR_STR_LISTEN_INTERVAL_ASSOC, listen_interval) );

    return WHD_SUCCESS;

}

whd_result_t whd_wifi_get_listen_interval(whd_interface_t ifp, whd_listen_interval_t *li)
{
    whd_buffer_t buffer;
    whd_buffer_t response;
    int *data;
    uint8_t *pdata = NULL;
    whd_driver_t whd_driver;

    CHECK_IFP_NULL(ifp);

    if (li == NULL)
        return WHD_BADARG;

    whd_driver = ifp->whd_driver;

    CHECK_DRIVER_NULL(whd_driver);

    data = (int *)whd_proto_get_iovar_buffer(whd_driver, &buffer, 4, IOVAR_STR_LISTEN_INTERVAL_BEACON);
    CHECK_IOCTL_BUFFER(data);
    memset(data, 0, 1);
    CHECK_RETURN(whd_proto_get_iovar(ifp, buffer, &response) );
    pdata = whd_buffer_get_current_piece_data_pointer(whd_driver, response);
    CHECK_PACKET_NULL(pdata, WHD_NO_REGISTER_FUNCTION_POINTER);
    memcpy( (uint8_t *)&(li->beacon), (char *)pdata, 1 );
    CHECK_RETURN(whd_buffer_release(whd_driver, response, WHD_NETWORK_RX) );

    data = (int *)whd_proto_get_iovar_buffer(whd_driver, &buffer, 4, IOVAR_STR_LISTEN_INTERVAL_DTIM);
    CHECK_IOCTL_BUFFER(data);
    memset(data, 0, 1);
    CHECK_RETURN(whd_proto_get_iovar(ifp, buffer, &response) );
    pdata = whd_buffer_get_current_piece_data_pointer(whd_driver, response);
    CHECK_PACKET_NULL(pdata, WHD_NO_REGISTER_FUNCTION_POINTER);
    memcpy( (uint8_t *)&(li->dtim), (char *)pdata, 1 );
    CHECK_RETURN(whd_buffer_release(whd_driver, response, WHD_NETWORK_RX) );

    data = (int *)whd_proto_get_iovar_buffer(whd_driver, &buffer, 4, IOVAR_STR_LISTEN_INTERVAL_ASSOC);
    CHECK_IOCTL_BUFFER(data);
    memset(data, 0, 4);
    CHECK_RETURN(whd_proto_get_iovar(ifp, buffer, &response) );
    pdata = whd_buffer_get_current_piece_data_pointer(whd_driver, response);
    CHECK_PACKET_NULL(pdata, WHD_NO_REGISTER_FUNCTION_POINTER);
    memcpy( (uint16_t *)&(li->assoc), (char *)pdata, 2 );
    CHECK_RETURN(whd_buffer_release(whd_driver, response, WHD_NETWORK_RX) );

    return WHD_SUCCESS;
}

whd_result_t whd_wifi_is_ready_to_transceive(whd_interface_t ifp)
{
    whd_driver_t whd_driver;

    CHECK_IFP_NULL(ifp);

    whd_driver = ifp->whd_driver;

    CHECK_DRIVER_NULL(whd_driver);

    switch (ifp->role)
    {
        case WHD_AP_ROLE:
            return (whd_wifi_get_ap_is_up(whd_driver) == WHD_TRUE) ? WHD_SUCCESS : WHD_INTERFACE_NOT_UP;

        case WHD_STA_ROLE:
            return whd_wifi_check_join_status(ifp);

        /* Disables Eclipse static analysis warning */
        /* No break needed due to returns in all case paths */
        /* no break */
        /* Fall Through */
        case WHD_P2P_ROLE:
        case WHD_INVALID_ROLE:

        default:
            return WHD_UNKNOWN_INTERFACE;
    }
}

whd_result_t whd_wifi_get_acparams(whd_interface_t ifp, edcf_acparam_t *acp)
{
    whd_buffer_t buffer;
    whd_buffer_t response;
    whd_driver_t whd_driver;
    uint8_t *pdata = NULL;

    if (!ifp || !acp)
    {
        WPRINT_WHD_ERROR( ("Invalid param in func %s at line %d \n",
                           __func__, __LINE__) );
        return WHD_WLAN_BADARG;
    }
    whd_driver = ifp->whd_driver;

    CHECK_DRIVER_NULL(whd_driver);

    int *data = (int *)whd_proto_get_iovar_buffer(whd_driver, &buffer, 64, IOVAR_STR_AC_PARAMS_STA);

    CHECK_IOCTL_BUFFER(data);
    memset(data, 0, 64);
    CHECK_RETURN(whd_proto_get_iovar(ifp, buffer, &response) );
    pdata = whd_buffer_get_current_piece_data_pointer(whd_driver, response);
    CHECK_PACKET_NULL(pdata, WHD_NO_REGISTER_FUNCTION_POINTER);
    memcpy( (char *)acp, (char *)pdata, (sizeof(edcf_acparam_t) * 4) );
    CHECK_RETURN(whd_buffer_release(whd_driver, response, WHD_NETWORK_RX) );

    return WHD_SUCCESS;
}

whd_result_t whd_wifi_get_channels(whd_interface_t ifp, whd_list_t *channel_list)
{
    whd_buffer_t buffer;
    whd_buffer_t response;
    whd_list_t *list;
    whd_driver_t whd_driver;
    uint16_t buffer_length;

    if (!ifp || !channel_list)
    {
        WPRINT_WHD_ERROR( ("Invalid param in func %s at line %d \n",
                           __func__, __LINE__) );
        return WHD_WLAN_BADARG;
    }
    if (!channel_list->count)
    {
        WPRINT_WHD_ERROR( ("channel_list->count is zero and max channel is %d in func %s at line %d \n",
                           MAXCHANNEL, __func__, __LINE__) );
        return WHD_WLAN_BADARG;
    }

    whd_driver = ifp->whd_driver;

    CHECK_DRIVER_NULL(whd_driver);

    buffer_length = sizeof(uint32_t) * (MAXCHANNEL + 1);

    list = (whd_list_t *)whd_proto_get_ioctl_buffer(whd_driver, &buffer, buffer_length);
    CHECK_IOCTL_BUFFER(list);

    memset(list, 0, buffer_length);
    list->count = htod32(MAXCHANNEL);
    CHECK_RETURN(whd_proto_get_ioctl(ifp, WLC_GET_VALID_CHANNELS, buffer, &response) );

    list = (whd_list_t *)whd_buffer_get_current_piece_data_pointer(whd_driver, response);
    memcpy(channel_list, list,
           (size_t)MIN_OF(whd_buffer_get_current_piece_size(whd_driver, response),
                          (sizeof(uint32_t) * (channel_list->count + 1) ) ) );

    CHECK_RETURN(whd_buffer_release(whd_driver, response, WHD_NETWORK_RX) );

    return WHD_SUCCESS;
}

whd_result_t whd_wifi_manage_custom_ie(whd_interface_t ifp, whd_custom_ie_action_t action, const uint8_t *oui,
                                   uint8_t subtype, const void *data, uint16_t length, uint16_t which_packets)
{
    whd_buffer_t buffer;
    vndr_ie_setbuf_t *ie_setbuf;
    uint32_t *iovar_data;
    whd_driver_t whd_driver;

    if (!ifp || !oui || !data)
    {
        WPRINT_WHD_ERROR( ("Invalid param in func %s at line %d \n",
                           __func__, __LINE__) );
        return WHD_WLAN_BADARG;
    }

    /* VNDR_IE = OUI + subtype + data_length */
    if (VNDR_IE_MAX_LEN < WIFI_IE_OUI_LENGTH + 1 + length)
    {
        WPRINT_WHD_ERROR( ("Invalid length :%u in func %s\n", length, __func__) );
        return WHD_WLAN_BADARG;
    }

    if (which_packets & VENDOR_IE_UNKNOWN)
    {
        WPRINT_WHD_ERROR( ("Unsupported packet ID(%x) in func %s\n", which_packets, __func__) );
        return WHD_WLAN_BADARG;
    }

    whd_driver = ifp->whd_driver;

    CHECK_DRIVER_NULL(whd_driver);

    whd_assert("Bad Args", oui != NULL);

    iovar_data =
        (uint32_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer, (uint16_t)(sizeof(vndr_ie_setbuf_t) + length + 4),
                                               "bsscfg:" IOVAR_STR_VENDOR_IE);
    CHECK_IOCTL_BUFFER(iovar_data);
    *iovar_data = ifp->bsscfgidx;
    ie_setbuf = (vndr_ie_setbuf_t *)(iovar_data + 1);

    /* Copy the vndr_ie SET command ("add"/"del") to the buffer */
    if (action == WHD_ADD_CUSTOM_IE)
    {
        memcpy( (char *)ie_setbuf->cmd, "add", 3 );
    }
    else
    {
        memcpy( (char *)ie_setbuf->cmd, "del", 3 );
    }
    ie_setbuf->cmd[3] = 0;

    /* Set the values */
	ie_setbuf->vndr_ie_buffer.vndr_ie_list[0].pktflag = htod32(which_packets);
	ie_setbuf->vndr_ie_buffer.vndr_ie_list[0].vndr_ie_data.id = 0xdd;
	ie_setbuf->vndr_ie_buffer.vndr_ie_list[0].vndr_ie_data.len =
	    ( uint8_t )(length + sizeof(ie_setbuf->vndr_ie_buffer.vndr_ie_list[0].vndr_ie_data.oui) + 1);	/* +1: one byte for sub type */
	ie_setbuf->vndr_ie_buffer.iecount = (int32_t)htod32(1);

    /* Stop lint warning about vndr_ie_list array element not yet being defined */
    memcpy(ie_setbuf->vndr_ie_buffer.vndr_ie_list[0].vndr_ie_data.oui, oui, (size_t)WIFI_IE_OUI_LENGTH);

    ie_setbuf->vndr_ie_buffer.vndr_ie_list[0].vndr_ie_data.data[0] = subtype;

    memcpy(&ie_setbuf->vndr_ie_buffer.vndr_ie_list[0].vndr_ie_data.data[1], data, length);

    RETURN_WITH_ASSERT(whd_proto_set_iovar(ifp, buffer, NULL) );
}

whd_result_t whd_wifi_send_action_frame(whd_interface_t ifp, whd_af_params_t *af_params)
{
    whd_buffer_t buffer;
    whd_af_params_t *af_frame;
    whd_driver_t whd_driver;
    CHECK_IFP_NULL(ifp);

    whd_driver = ifp->whd_driver;

    CHECK_DRIVER_NULL(whd_driver);

    if ( (af_params == NULL) || (af_params->action_frame.len > ACTION_FRAME_SIZE) )
    {
        WPRINT_WHD_ERROR( ("Invalid param in func %s at line %d \n", __func__, __LINE__) );
        return WHD_WLAN_BADARG;
    }

    af_frame = (whd_af_params_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer, WL_WIFI_AF_PARAMS_SIZE,
                                                             IOVAR_STR_ACTION_FRAME);
    CHECK_IOCTL_BUFFER (af_frame);
    memcpy(af_frame, af_params, WL_WIFI_AF_PARAMS_SIZE);
    RETURN_WITH_ASSERT(whd_proto_set_iovar(ifp, buffer, NULL) );
}

whd_result_t whd_wifi_send_auth_frame(whd_interface_t ifp, whd_auth_params_t *auth_params)
{
    whd_buffer_t buffer;
    whd_auth_params_t *auth_frame;
    whd_driver_t whd_driver;
    uint16_t auth_frame_len;
    CHECK_IFP_NULL(ifp);

    whd_driver = ifp->whd_driver;

    CHECK_DRIVER_NULL(whd_driver);

    if (auth_params == NULL)
    {
        WPRINT_WHD_ERROR( ("Invalid param in func %s at line %d \n", __func__, __LINE__) );
        return WHD_WLAN_BADARG;
    }
    /* FW doesn't need MAC Header Length  */
    auth_params->len -= DOT11_MGMT_HDR_LEN;
    auth_frame_len = OFFSET(whd_auth_params_t, data) + auth_params->len;
    auth_frame = (whd_auth_params_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer, auth_frame_len,
                                                                 IOVAR_STR_MGMT_FRAME);
    CHECK_IOCTL_BUFFER (auth_frame);
    memcpy(auth_frame, auth_params, OFFSET(whd_auth_params_t, data) );
    memcpy(auth_frame->data, &auth_params->data[DOT11_MGMT_HDR_LEN], auth_params->len);
    auth_frame->dwell_time = MGMT_AUTH_FRAME_DWELL_TIME;
    RETURN_WITH_ASSERT(whd_proto_set_iovar(ifp, buffer, NULL) );
}

whd_result_t whd_wifi_he_omi(whd_interface_t ifp, whd_he_omi_params_t *he_omi_params)
{
    whd_buffer_t buffer;
    whd_xtlv_t *he_omi_iovar;
    whd_driver_t whd_driver;
    wl_he_omi_t he_omi;

    CHECK_IFP_NULL(ifp);
    whd_driver = ifp->whd_driver;
    CHECK_DRIVER_NULL(whd_driver);

    memset( (uint8_t *)&he_omi_iovar, 0x00, sizeof(he_omi_iovar) );
    he_omi.version = WL_HE_OMI_VER;
    he_omi.length = sizeof(wl_he_omi_t) - 2;
    he_omi.rx_nss = he_omi_params->rx_nss;
    he_omi.chnl_wdth = he_omi_params->chnl_wdth;
    he_omi.ul_mu_dis = he_omi_params->ul_mu_dis;
    he_omi.tx_nsts = he_omi_params->tx_nsts;
    he_omi.er_su_dis = he_omi_params->er_su_dis;
    he_omi.dl_mu_resound = he_omi_params->dl_mu_resound;
    he_omi.ul_mu_data_dis = he_omi_params->ul_mu_data_dis;

    he_omi_iovar = (whd_xtlv_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer, sizeof(wl_he_omi_t) + 4,
                                                            IOVAR_STR_HE);
    CHECK_IOCTL_BUFFER (he_omi_iovar);
    he_omi_iovar->id = WL_HE_CMD_OMI;
    he_omi_iovar->len = sizeof(wl_he_omi_t);
    memcpy(he_omi_iovar->data, (uint8_t *)&he_omi, sizeof(wl_he_omi_t) );
    RETURN_WITH_ASSERT(whd_proto_set_iovar(ifp, buffer, NULL) );
}

whd_result_t whd_wifi_bss_max_idle(whd_interface_t ifp, uint16_t period)
{
    whd_buffer_t buffer;
    whd_driver_t whd_driver;
    uint32_t *iovar_data;

    CHECK_IFP_NULL(ifp);
    whd_driver = ifp->whd_driver;
    CHECK_DRIVER_NULL(whd_driver);

    iovar_data = (uint32_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer, 8, IOVAR_WNM_MAXIDLE);
    CHECK_IOCTL_BUFFER (iovar_data);
    /* set bss_max_idle_period */
    iovar_data[0] = period;
    /* set bss_idle_opt */
    iovar_data[1] = 1;
    RETURN_WITH_ASSERT(whd_proto_set_iovar(ifp, buffer, NULL) );
}

whd_result_t whd_wifi_itwt_setup(whd_interface_t ifp, whd_itwt_setup_params_t *twt_params)
{
    whd_buffer_t buffer;
    whd_xtlv_t *twt_iovar;
    whd_driver_t whd_driver;
    wl_twt_setup_t itwt_setup;

    CHECK_IFP_NULL(ifp);
    whd_driver = ifp->whd_driver;
    CHECK_DRIVER_NULL(whd_driver);

    memset( (uint8_t *)&itwt_setup, 0x00, sizeof(itwt_setup) );
    itwt_setup.version = WL_TWT_SETUP_VER;
    itwt_setup.length = sizeof(wl_twt_setup_t) - 4;
    itwt_setup.desc.negotiation_type = TWT_CTRL_NEGO_TYPE_0;
    itwt_setup.desc.flow_flags = WL_TWT_FLOW_FLAG_REQUEST;
    if (twt_params == NULL)
    {
        WPRINT_WHD_INFO( ("Trigger Individual TWT with default value\n") );
        itwt_setup.desc.setup_cmd = TWT_SETUP_CMD_SUGGEST_TWT;
        itwt_setup.desc.wake_dur = 255 * 256;
        itwt_setup.desc.wake_int = 8192 * (1 << 10);
        itwt_setup.desc.flow_id = 0xFF;
        itwt_setup.desc.flow_flags |= WL_TWT_FLOW_FLAG_TRIGGER;
        itwt_setup.desc.flow_flags |= WL_TWT_FLOW_FLAG_UNANNOUNCED;
    }
    else
    {
        itwt_setup.desc.setup_cmd = twt_params->setup_cmd;
        itwt_setup.desc.wake_dur = twt_params->wake_duration * 256;
        itwt_setup.desc.wake_int = twt_params->mantissa * (1 << twt_params->exponent);
        itwt_setup.desc.flow_id = twt_params->flow_id;
        itwt_setup.desc.flow_flags |= (twt_params->trigger) ? WL_TWT_FLOW_FLAG_TRIGGER : 0;
        itwt_setup.desc.flow_flags |= (twt_params->flow_type) ? WL_TWT_FLOW_FLAG_UNANNOUNCED : 0;
        itwt_setup.desc.wake_time_h = twt_params->wake_time_h;
        itwt_setup.desc.wake_time_l = twt_params->wake_time_l;
    }
    twt_iovar = (whd_xtlv_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer, sizeof(wl_twt_setup_t) + 4,
                                                         IOVAR_STR_TWT);
    CHECK_IOCTL_BUFFER (twt_iovar);
    twt_iovar->id = WL_TWT_CMD_SETUP;
    twt_iovar->len = sizeof(wl_twt_setup_t);
    memcpy(twt_iovar->data, (uint8_t *)&itwt_setup, sizeof(wl_twt_setup_t) );
    RETURN_WITH_ASSERT(whd_proto_set_iovar(ifp, buffer, NULL) );
}

whd_result_t whd_wifi_btwt_join(whd_interface_t ifp, whd_btwt_join_params_t *twt_params)
{
    whd_buffer_t buffer;
    whd_xtlv_t *twt_iovar;
    whd_driver_t whd_driver;
    wl_twt_setup_t btwt_setup;

    if (twt_params == NULL)
    {
        WPRINT_WHD_ERROR( ("Invalid param in func %s at line %d\n", __func__, __LINE__) );
        return WHD_WLAN_BADARG;
    }
    CHECK_IFP_NULL(ifp);
    whd_driver = ifp->whd_driver;
    CHECK_DRIVER_NULL(whd_driver);

    memset( (uint8_t *)&btwt_setup, 0x00, sizeof(btwt_setup) );
    btwt_setup.version = WL_TWT_SETUP_VER;
    btwt_setup.length = sizeof(wl_twt_setup_t) - 4;
    btwt_setup.desc.flow_flags = WL_TWT_FLOW_FLAG_REQUEST;
    btwt_setup.desc.flow_id = 0; /* map to bTWT recommendation subfield */
    btwt_setup.desc.negotiation_type = TWT_CTRL_NEGO_TYPE_3;
    btwt_setup.desc.wake_type = WL_TWT_TIME_TYPE_BSS;
    btwt_setup.desc.setup_cmd = twt_params->setup_cmd;
    btwt_setup.desc.flow_flags |= (twt_params->trigger) ? WL_TWT_FLOW_FLAG_TRIGGER : 0;
    btwt_setup.desc.flow_flags |= (twt_params->flow_type) ? WL_TWT_FLOW_FLAG_UNANNOUNCED : 0;
    btwt_setup.desc.wake_dur = twt_params->wake_duration * 256;
    btwt_setup.desc.wake_int = twt_params->mantissa * (1 << twt_params->exponent);
    btwt_setup.desc.bid = twt_params->bid;

    twt_iovar = (whd_xtlv_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer, sizeof(wl_twt_setup_t) + 4,
                                                         IOVAR_STR_TWT);
    CHECK_IOCTL_BUFFER (twt_iovar);
    twt_iovar->id = WL_TWT_CMD_SETUP;
    twt_iovar->len = sizeof(wl_twt_setup_t);
    memcpy(twt_iovar->data, (uint8_t *)&btwt_setup, sizeof(wl_twt_setup_t) );
    RETURN_WITH_ASSERT(whd_proto_set_iovar(ifp, buffer, NULL) );
}

whd_result_t whd_wifi_twt_teardown(whd_interface_t ifp, whd_twt_teardown_params_t *twt_params)
{
    whd_buffer_t buffer;
    whd_xtlv_t *twt_iovar;
    whd_driver_t whd_driver;
    wl_twt_teardown_t twt_teardown;

    if (twt_params == NULL)
    {
        WPRINT_WHD_ERROR( ("Invalid param in func %s at line %d\n", __func__, __LINE__) );
        return WHD_WLAN_BADARG;
    }
    CHECK_IFP_NULL(ifp);
    whd_driver = ifp->whd_driver;
    CHECK_DRIVER_NULL(whd_driver);

    memset( (uint8_t *)&twt_teardown, 0x00, sizeof(twt_teardown) );
    twt_teardown.version = WL_TWT_TEARDOWN_VER;
    twt_teardown.length = sizeof(wl_twt_teardown_t) - 4;
    twt_teardown.teardesc.negotiation_type = twt_params->negotiation_type;
    twt_teardown.teardesc.flow_id = twt_params->flow_id;
    twt_teardown.teardesc.bid = twt_params->bcast_twt_id;
    twt_teardown.teardesc.alltwt = twt_params->teardown_all_twt;

    twt_iovar = (whd_xtlv_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer, sizeof(wl_twt_teardown_t) + 4,
                                                         IOVAR_STR_TWT);
    CHECK_IOCTL_BUFFER (twt_iovar);
    twt_iovar->id = WL_TWT_CMD_TEARDOWN;
    twt_iovar->len = sizeof(wl_twt_teardown_t);
    memcpy(twt_iovar->data, (uint8_t *)&twt_teardown, sizeof(wl_twt_teardown_t) );
    RETURN_WITH_ASSERT(whd_proto_set_iovar(ifp, buffer, NULL) );
}

whd_result_t whd_wifi_twt_information_frame(whd_interface_t ifp, whd_twt_information_params_t *twt_params)
{
    whd_buffer_t buffer;
    whd_xtlv_t *twt_iovar;
    whd_driver_t whd_driver;
    wl_twt_info_t twt_information;

    if (twt_params == NULL)
    {
        WPRINT_WHD_ERROR( ("Invalid param in func %s at line %d\n", __func__, __LINE__) );
        return WHD_WLAN_BADARG;
    }
    CHECK_IFP_NULL(ifp);
    whd_driver = ifp->whd_driver;
    CHECK_DRIVER_NULL(whd_driver);

    memset( (uint8_t *)&twt_information, 0x00, sizeof(twt_information) );
    twt_information.version = WL_TWT_INFO_VER;
    twt_information.length = sizeof(wl_twt_info_t) - 4;
    twt_information.infodesc.flow_flags |= WL_TWT_INFO_FLAG_ALL_TWT;
    twt_information.infodesc.flow_id = twt_params->flow_id;
    if (twt_params->suspend == 1)
    {
        twt_information.infodesc.next_twt_h = 0;
        twt_information.infodesc.next_twt_l = 0;
    }
    else
    {
        twt_information.infodesc.flow_flags = WL_TWT_INFO_FLAG_RESUME;
        twt_information.infodesc.next_twt_h = 0;
        twt_information.infodesc.next_twt_l = (twt_params->resume_time << 20) & 0xFFFFFFFF;
    }

    twt_iovar = (whd_xtlv_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer, sizeof(wl_twt_info_t) + 4,
                                                         IOVAR_STR_TWT);
    CHECK_IOCTL_BUFFER (twt_iovar);
    twt_iovar->id = WL_TWT_CMD_INFO;
    twt_iovar->len = sizeof(wl_twt_info_t);
    memcpy(twt_iovar->data, (uint8_t *)&twt_information, sizeof(wl_twt_info_t) );
    RETURN_WITH_ASSERT(whd_proto_set_iovar(ifp, buffer, NULL) );
}

whd_result_t whd_wifi_btwt_config(whd_interface_t ifp, whd_btwt_config_params_t *twt_params)
{
    whd_buffer_t buffer;
    whd_xtlv_t *twt_iovar;
    whd_driver_t whd_driver;

    if (twt_params == NULL)
    {
        WPRINT_WHD_ERROR( ("Invalid param in func %s at line %d\n", __func__, __LINE__) );
        return WHD_WLAN_BADARG;
    }
    CHECK_IFP_NULL(ifp);
    wl_twt_setup_t config_btwt;
    whd_driver = ifp->whd_driver;
    CHECK_DRIVER_NULL(whd_driver);

    memset( (uint8_t *)&config_btwt, 0x00, sizeof(config_btwt) );
    config_btwt.version = WL_TWT_SETUP_VER;
    config_btwt.length = sizeof(wl_twt_setup_t) - 4;
    config_btwt.desc.negotiation_type = TWT_CTRL_NEGO_TYPE_2;
    config_btwt.desc.wake_type = WL_TWT_TIME_TYPE_BSS;
    config_btwt.desc.setup_cmd = twt_params->setup_cmd;
    config_btwt.desc.wake_dur = twt_params->wake_duration * 256;
    config_btwt.desc.wake_int = twt_params->mantissa * (1 << twt_params->exponent);
    config_btwt.desc.bid = twt_params->bid;
    config_btwt.desc.flow_flags |= (twt_params->trigger) ? WL_TWT_FLOW_FLAG_TRIGGER : 0;
    config_btwt.desc.flow_flags |= (twt_params->flow_type) ? WL_TWT_FLOW_FLAG_UNANNOUNCED : 0;

    twt_iovar = (whd_xtlv_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer, sizeof(wl_twt_setup_t) + 4,
                                                         IOVAR_STR_TWT);
    CHECK_IOCTL_BUFFER (twt_iovar);
    twt_iovar->id = WL_TWT_CMD_SETUP;
    twt_iovar->len = sizeof(wl_twt_setup_t);
    memcpy(twt_iovar->data, (uint8_t *)&config_btwt, sizeof(wl_twt_setup_t) );
    RETURN_WITH_ASSERT(whd_proto_set_iovar(ifp, buffer, NULL) );
}

uint32_t whd_wifi_mbo_add_chan_pref(whd_interface_t ifp, whd_mbo_add_chan_pref_params_t *mbo_params)
{
    whd_buffer_t buffer;
    whd_iov_buf_t *mbo_iovar;
    whd_driver_t whd_driver;
    mbo_add_chan_pref_t ch_pref;

    CHECK_IFP_NULL(ifp);
    whd_driver = ifp->whd_driver;
    CHECK_DRIVER_NULL(whd_driver);

    uint16_t wlan_chip_id = whd_chip_get_chip_id(whd_driver);
    if ((wlan_chip_id != 55500) && (wlan_chip_id != 55900))
    {
        WPRINT_WHD_ERROR(("Connected Chip doesn't support MBO \n"));
        return WHD_UNSUPPORTED;
    }

    memset( (uint8_t *)&ch_pref, 0x00, sizeof(ch_pref) );
    //opclass
    ch_pref.opclass.id = WL_MBO_XTLV_OPCLASS;
    ch_pref.opclass.len = sizeof(mbo_params->opclass);
    memcpy(ch_pref.opclass.data, (uint32_t *)&mbo_params->opclass, sizeof(mbo_params->opclass));
    //channel
    ch_pref.chan.id = WL_MBO_XTLV_CHAN;
    ch_pref.chan.len = sizeof(mbo_params->chan);
    memcpy(ch_pref.chan.data, (uint32_t *)&mbo_params->chan, sizeof(mbo_params->chan));
    //channel preference
    ch_pref.pref.id = WL_MBO_XTLV_PREFERENCE;
    ch_pref.pref.len = sizeof(mbo_params->pref);
    memcpy(ch_pref.pref.data, (uint32_t *)&mbo_params->pref, sizeof(mbo_params->pref));
    //reason
    ch_pref.reason.id = WL_MBO_XTLV_REASON_CODE;
    ch_pref.reason.len = sizeof(mbo_params->reason);
    memcpy(ch_pref.reason.data, (uint32_t *)&mbo_params->reason, sizeof(mbo_params->reason));

    mbo_iovar = (whd_iov_buf_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer, sizeof(whd_iov_buf_t) + sizeof(ch_pref),
                                                         IOVAR_STR_MBO);
    CHECK_IOCTL_BUFFER (mbo_iovar);
    mbo_iovar->version = WL_MBO_IOV_VERSION;
    mbo_iovar->len = sizeof(ch_pref);
    mbo_iovar->id = WL_MBO_CMD_ADD_CHAN_PREF;
    memcpy(mbo_iovar->data, (uint16_t *)&ch_pref, sizeof(mbo_add_chan_pref_t) );
    RETURN_WITH_ASSERT(whd_proto_set_iovar(ifp, buffer, NULL) );
}

uint32_t whd_wifi_mbo_del_chan_pref(whd_interface_t ifp, whd_mbo_del_chan_pref_params_t *mbo_params)
{
    whd_buffer_t buffer;
    whd_iov_buf_t *mbo_iovar;
    whd_driver_t whd_driver;
    mbo_del_chan_pref_t ch_pref;

    CHECK_IFP_NULL(ifp);
    whd_driver = ifp->whd_driver;
    CHECK_DRIVER_NULL(whd_driver);

    uint16_t wlan_chip_id = whd_chip_get_chip_id(whd_driver);
    if ((wlan_chip_id != 55500) && (wlan_chip_id != 55900))
    {
        WPRINT_WHD_ERROR(("Connected Chip doesn't support MBO \n"));
        return WHD_UNSUPPORTED;
    }

    memset( (uint8_t *)&ch_pref, 0x00, sizeof(ch_pref) );
    //opclass
    ch_pref.opclass.id = WL_MBO_XTLV_OPCLASS;
    ch_pref.opclass.len = sizeof(mbo_params->opclass);
    memcpy(ch_pref.opclass.data, (uint32_t *)&mbo_params->opclass, sizeof(mbo_params->opclass));
    //channel
    ch_pref.chan.id = WL_MBO_XTLV_CHAN;
    ch_pref.chan.len = sizeof(mbo_params->chan);
    memcpy(ch_pref.chan.data, (uint32_t *)&mbo_params->chan, sizeof(mbo_params->chan));

    mbo_iovar = (whd_iov_buf_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer, sizeof(whd_iov_buf_t) + sizeof(ch_pref),
                                                         IOVAR_STR_MBO);
    CHECK_IOCTL_BUFFER (mbo_iovar);
    mbo_iovar->version = WL_MBO_IOV_VERSION;
    mbo_iovar->len = sizeof(ch_pref);
    mbo_iovar->id = WL_MBO_CMD_DEL_CHAN_PREF;
    memcpy(mbo_iovar->data, (uint16_t *)&ch_pref, sizeof(mbo_del_chan_pref_t) );
    RETURN_WITH_ASSERT(whd_proto_set_iovar(ifp, buffer, NULL) );
}

uint32_t whd_wifi_mbo_send_notif(whd_interface_t ifp, uint8_t sub_elem_type)
{
    whd_buffer_t buffer;
    whd_iov_buf_t *mbo_iovar;
    whd_driver_t whd_driver;
    mbo_xtlv_t sub_elem;

    if (sub_elem_type != MBO_ATTR_CELL_DATA_CAP &&
            sub_elem_type != MBO_ATTR_NON_PREF_CHAN_REPORT)
    {
        WPRINT_WHD_ERROR( ("Invalid value in func %s at line %d\n", __func__, __LINE__) );
        return WHD_WLAN_BADARG;
    }

    CHECK_IFP_NULL(ifp);
    whd_driver = ifp->whd_driver;
    CHECK_DRIVER_NULL(whd_driver);

    uint16_t wlan_chip_id = whd_chip_get_chip_id(whd_driver);
    if ((wlan_chip_id != 55500) && (wlan_chip_id != 55900))
    {
        WPRINT_WHD_ERROR(("Connected Chip doesn't support MBO \n"));
        return WHD_UNSUPPORTED;
    }

    memset( (uint8_t *)&sub_elem, 0x00, sizeof(sub_elem) );
    sub_elem.id = WL_MBO_XTLV_SUB_ELEM_TYPE;
    sub_elem.len = sizeof(sub_elem_type);
    memcpy(sub_elem.data, (uint32_t *)&sub_elem_type, sizeof(sub_elem_type));

    mbo_iovar = (whd_iov_buf_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer, sizeof(whd_iov_buf_t) + sizeof(sub_elem),
                                                         IOVAR_STR_MBO);
    CHECK_IOCTL_BUFFER (mbo_iovar);
    mbo_iovar->version = WL_MBO_IOV_VERSION;
    mbo_iovar->len = sizeof(sub_elem);
    mbo_iovar->id = WL_MBO_CMD_SEND_NOTIF;
    memcpy(mbo_iovar->data, (uint16_t *)&sub_elem, sizeof(mbo_xtlv_t) );
    RETURN_WITH_ASSERT(whd_proto_set_iovar(ifp, buffer, NULL) );
}

whd_result_t whd_wifi_set_ioctl_value(whd_interface_t ifp, uint32_t ioctl, uint32_t value)
{
    whd_buffer_t buffer;
    uint32_t *data;
    whd_driver_t whd_driver;

    CHECK_IFP_NULL(ifp);

    whd_driver = ifp->whd_driver;

    CHECK_DRIVER_NULL(whd_driver);

    data = (uint32_t *)whd_proto_get_ioctl_buffer(whd_driver, &buffer, (uint16_t)sizeof(value) );
    CHECK_IOCTL_BUFFER(data);
    *data = htod32(value);
    CHECK_RETURN(whd_proto_set_ioctl(ifp, ioctl, buffer, 0) );

    return WHD_SUCCESS;
}

whd_result_t whd_wifi_get_ioctl_value(whd_interface_t ifp, uint32_t ioctl, uint32_t *value)
{
    whd_buffer_t buffer;
    whd_buffer_t response;
    whd_driver_t whd_driver;
    uint8_t  *data = NULL;

    if (value == NULL)
        return WHD_BADARG;

    CHECK_IFP_NULL(ifp);

    whd_driver = ifp->whd_driver;

    CHECK_DRIVER_NULL(whd_driver);

    CHECK_IOCTL_BUFFER(whd_proto_get_ioctl_buffer(whd_driver, &buffer, (uint16_t)sizeof(*value) ) );
    CHECK_RETURN_UNSUPPORTED_OK(whd_proto_get_ioctl(ifp, ioctl, buffer, &response) );
    data = whd_buffer_get_current_piece_data_pointer(whd_driver, response);
    CHECK_PACKET_NULL(data, WHD_NO_REGISTER_FUNCTION_POINTER);
    *value = dtoh32(*(uint32_t *)data);

    CHECK_RETURN(whd_buffer_release(whd_driver, response, WHD_NETWORK_RX) );

    return WHD_SUCCESS;
}

whd_result_t whd_wifi_set_ioctl_buffer(whd_interface_t ifp, uint32_t ioctl, void *in_buffer, uint16_t in_buffer_length)
{
    whd_buffer_t buffer;
    uint32_t *data;
    whd_driver_t whd_driver = ifp->whd_driver;

    data = (uint32_t *)whd_proto_get_ioctl_buffer(whd_driver, &buffer, in_buffer_length);
    CHECK_IOCTL_BUFFER(data);

    memcpy(data, in_buffer, in_buffer_length);

    CHECK_RETURN(whd_proto_set_ioctl(ifp, ioctl, buffer, NULL) );

    return WHD_SUCCESS;
}

whd_result_t whd_wifi_get_ioctl_buffer(whd_interface_t ifp, uint32_t ioctl, uint8_t *out_buffer, uint16_t out_length)
{
    whd_buffer_t buffer;
    uint32_t *data;
    whd_buffer_t response;
    whd_result_t result;
    whd_driver_t whd_driver;

    CHECK_IFP_NULL(ifp);

    whd_driver = ifp->whd_driver;
    data = (uint32_t *)whd_proto_get_ioctl_buffer(whd_driver, &buffer, out_length);
    CHECK_IOCTL_BUFFER(data);
    memcpy(data, out_buffer, out_length);

    result = whd_proto_get_ioctl(ifp, ioctl, buffer, &response);

    /* it worked: copy the result to the output buffer */
    if (WHD_SUCCESS == result)
    {
        data = (uint32_t *)whd_buffer_get_current_piece_data_pointer(whd_driver, response);
        CHECK_PACKET_NULL(data, WHD_NO_REGISTER_FUNCTION_POINTER);
        *data = dtoh32(*data);
        memcpy(out_buffer, data, out_length);
        CHECK_RETURN(whd_buffer_release(whd_driver, response, WHD_NETWORK_RX) );
    }

    CHECK_RETURN(result);

    return WHD_SUCCESS;
}

whd_result_t whd_wifi_set_iovar_void(whd_interface_t ifp, const char *iovar)
{
    whd_buffer_t buffer;
    whd_driver_t whd_driver = ifp->whd_driver;

    whd_proto_get_iovar_buffer(whd_driver, &buffer, (uint16_t)0, iovar);

    return whd_proto_set_iovar(ifp, buffer, NULL);
}

whd_result_t whd_wifi_set_iovar_value(whd_interface_t ifp, const char *iovar, uint32_t value)
{
    whd_buffer_t buffer;
    uint32_t *data;
    whd_driver_t whd_driver = ifp->whd_driver;

    data = (uint32_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer, (uint16_t)sizeof(value), iovar);
    CHECK_IOCTL_BUFFER(data);
    *data = htod32(value);
    return whd_proto_set_iovar(ifp, buffer, NULL);
}

whd_result_t whd_wifi_get_iovar_value(whd_interface_t ifp, const char *iovar, uint32_t *value)
{
    whd_buffer_t buffer;
    whd_buffer_t response;
    whd_driver_t whd_driver = ifp->whd_driver;
    uint8_t *data = NULL;

    if (value == NULL)
        return WHD_BADARG;

    CHECK_IOCTL_BUFFER(whd_proto_get_iovar_buffer(whd_driver, &buffer, 4, iovar) );
    CHECK_RETURN_UNSUPPORTED_OK(whd_proto_get_iovar(ifp, buffer, &response) );
    data = whd_buffer_get_current_piece_data_pointer(whd_driver, response);
    CHECK_PACKET_NULL(data, WHD_NO_REGISTER_FUNCTION_POINTER);
    *value = dtoh32(*(uint32_t *)data);
    CHECK_RETURN(whd_buffer_release(whd_driver, response, WHD_NETWORK_RX) );

    return WHD_SUCCESS;
}

whd_result_t whd_wifi_set_iovar_buffer(whd_interface_t ifp, const char *iovar, void *in_buffer, uint16_t in_buffer_length)
{
    return whd_wifi_set_iovar_buffers(ifp, iovar, (const void **)&in_buffer, (const uint16_t *)&in_buffer_length, 1);
}

whd_result_t whd_wifi_get_iovar_buffer(whd_interface_t ifp, const char *iovar_name, uint8_t *out_buffer,
                                   uint16_t out_length)
{
    uint32_t *data;
    whd_buffer_t buffer;
    whd_buffer_t response;
    whd_result_t result;
    whd_driver_t whd_driver = ifp->whd_driver;

    data = whd_proto_get_iovar_buffer(whd_driver, &buffer, (uint16_t)out_length, iovar_name);
    CHECK_IOCTL_BUFFER(data);

    result = whd_proto_get_iovar(ifp, buffer, &response);

    /* it worked: copy the result to the output buffer */
    if (WHD_SUCCESS == result)
    {
        data = (uint32_t *)whd_buffer_get_current_piece_data_pointer(whd_driver, response);
        *data = dtoh32(*data);
        memcpy(out_buffer, data, out_length);
        CHECK_RETURN(whd_buffer_release(whd_driver, response, WHD_NETWORK_RX) );
    }

    return result;
}

/*
 * format an iovar buffer
 */
static whd_result_t
whd_iovar_mkbuf(const char *name, char *data, uint32_t datalen, char *iovar_buf, uint16_t buflen)
{
    uint32_t iovar_len;

    iovar_len = strlen(name) + 1;

    /* check for overflow */
    if ( (iovar_len + datalen) > buflen )
    {
        return WHD_BADARG;
    }

    /* copy data to the buffer past the end of the iovar name string */
    if (datalen > 0)
        memmove(&iovar_buf[iovar_len], data, datalen);

    /* copy the name to the beginning of the buffer */
    strncpy(iovar_buf, name, (iovar_len - 1) );

    return WHD_SUCCESS;
}

whd_result_t whd_wifi_get_iovar_buffer_with_param(whd_interface_t ifp, const char *iovar_name, void *param,
                                                  uint32_t paramlen, uint8_t *out_buffer, uint32_t out_length)
{
    uint32_t *data;
    whd_buffer_t buffer;
    whd_buffer_t response;
    whd_result_t result;
    whd_driver_t whd_driver;

    if (!ifp || !iovar_name || !param || !out_buffer)
    {
        WPRINT_WHD_ERROR( ("Invalid param in func %s at line %d \n",
                           __func__, __LINE__) );
        return WHD_WLAN_BADARG;
    }

    whd_driver = (whd_driver_t)ifp->whd_driver;

    /* Format the input string */
    result = whd_iovar_mkbuf(iovar_name, param, paramlen, (char *)out_buffer, (uint16_t)out_length);
    if (result != WHD_SUCCESS)
        return result;

    data = whd_proto_get_ioctl_buffer(whd_driver, &buffer, (uint16_t)out_length);

    if (data == NULL)
        return WHD_WLAN_NOMEM;

    memcpy(data, out_buffer, out_length);

    result = (whd_result_t)whd_proto_get_ioctl(ifp, WLC_GET_VAR, buffer, &response);

    if (result == WHD_SUCCESS)
    {
        memcpy(out_buffer, whd_buffer_get_current_piece_data_pointer(whd_driver, response),
               (size_t)MIN_OF(whd_buffer_get_current_piece_size(whd_driver, response), out_length) );
        CHECK_RETURN(whd_buffer_release(whd_driver, response, WHD_NETWORK_RX) );
    }

    return result;
}

whd_result_t whd_wifi_set_iovar_buffers(whd_interface_t ifp, const char *iovar, const void **in_buffers,
                                    const uint16_t *lengths, const uint8_t num_buffers)
{
    whd_buffer_t buffer;
    uint32_t *data;
    int tot_in_buffer_length = 0;
    uint8_t buffer_num = 0;
    whd_driver_t whd_driver = ifp->whd_driver;

    /* get total length of all buffers: they will be copied into memory one after the other. */
    for (; buffer_num < num_buffers; buffer_num++)
    {
        tot_in_buffer_length += lengths[buffer_num];
    }

    /* get a valid buffer */
    data = (uint32_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer, (uint16_t)tot_in_buffer_length, iovar);
    CHECK_IOCTL_BUFFER(data);

    /* copy all data into buffer */
    for (buffer_num = 0; buffer_num < num_buffers; buffer_num++)
    {
        memcpy(data, in_buffers[buffer_num], lengths[buffer_num]);
        data += lengths[buffer_num];
    }

    /* send iovar */
    return whd_proto_set_iovar(ifp, buffer, NULL);
}

whd_result_t whd_wifi_get_clm_version(whd_interface_t ifp, char *version, uint8_t length)
{
    whd_result_t result;

    CHECK_IFP_NULL(ifp);

    if (version == NULL)
        return WHD_BADARG;

    version[0] = '\0';

    result = whd_wifi_get_iovar_buffer(ifp, IOVAR_STR_CLMVER, (uint8_t *)version, length);
    if ( (result == WHD_SUCCESS) && version[0] )
    {
        uint8_t version_length;
        char *p;

        version_length = strlen(version);

        /* -2 becase \0 termination needs a char and strlen doesn't include length of \0 */
        if (version_length > length - 2)
            version_length = length - 2;
        version[version_length + 1] = '\0';

        /* Replace all newline/linefeed characters with space character */
        p = version;
        while ( (p = strchr(p, '\n') ) != NULL )
        {
            *p = ' ';
        }
    }

    CHECK_RETURN(result);
    return WHD_SUCCESS;
}

whd_result_t whd_wifi_get_wifi_version(whd_interface_t ifp, char *buf, uint8_t length)
{
    whd_result_t result;
    uint8_t ver_len;

    CHECK_IFP_NULL(ifp);

    if (buf == NULL)
        return WHD_BADARG;

    result = whd_wifi_get_iovar_buffer(ifp, IOVAR_STR_VERSION, (uint8_t *)buf, length);

    ver_len = strlen(buf);

    if (ver_len > length - 2)
        ver_len = length - 2;

    if ( (ver_len > 1) && (buf[ver_len + 1] == '\n') )
    {
        buf[ver_len + 1] = '\0';
    }

    CHECK_RETURN(result);
    return WHD_SUCCESS;
}

whd_result_t whd_network_get_ifidx_from_ifp(whd_interface_t ifp, uint8_t *ifidx)
{
    CHECK_IFP_NULL(ifp);

    if (!ifidx)
        return WHD_BADARG;

    *ifidx = ifp->ifidx;

    return WHD_SUCCESS;
}

whd_result_t whd_network_get_bsscfgidx_from_ifp(whd_interface_t ifp, uint8_t *bsscfgidx)
{
    CHECK_IFP_NULL(ifp);

    if (!bsscfgidx)
        return WHD_BADARG;

    *bsscfgidx = ifp->bsscfgidx;

    return WHD_SUCCESS;
}

whd_result_t whd_wifi_ap_set_beacon_interval(whd_interface_t ifp, uint16_t interval)
{
    CHECK_IFP_NULL(ifp);

    CHECK_RETURN(whd_wifi_set_ioctl_value(ifp, WLC_SET_BCNPRD, interval) );
    return WHD_SUCCESS;
}

whd_result_t whd_wifi_ap_set_dtim_interval(whd_interface_t ifp, uint16_t interval)
{
    CHECK_IFP_NULL(ifp);

    CHECK_RETURN(whd_wifi_set_ioctl_value(ifp, WLC_SET_DTIMPRD, interval) );
    return WHD_SUCCESS;
}

whd_result_t whd_wifi_get_bss_info(whd_interface_t ifp, wl_bss_info_t *bi)
{
    whd_buffer_t buffer, response;
    uint32_t result;
    uint8_t  *data;
    whd_driver_t whd_driver;

    CHECK_IFP_NULL(ifp);

    whd_driver = ifp->whd_driver;

    CHECK_DRIVER_NULL(whd_driver);

    if (bi == NULL)
        return WHD_BADARG;

    if (whd_proto_get_ioctl_buffer(whd_driver, &buffer, WLC_IOCTL_SMLEN) == NULL)
    {
        WPRINT_WHD_INFO( ("%s: Unable to malloc WLC_GET_BSS_INFO buffer\n", __FUNCTION__) );
        return WHD_SUCCESS;
    }
    result = whd_proto_get_ioctl(ifp, WLC_GET_BSS_INFO, buffer, &response);
    if (result != WHD_SUCCESS)
    {
        WPRINT_WHD_INFO( ("%s: WLC_GET_BSS_INFO Failed\n", __FUNCTION__) );
        return result;
    }
    data = whd_buffer_get_current_piece_data_pointer(whd_driver, response);
    CHECK_PACKET_NULL(data, WHD_NO_REGISTER_FUNCTION_POINTER);
    memcpy(bi, data  + 4, sizeof(wl_bss_info_t) );

    CHECK_RETURN(whd_buffer_release(whd_driver, response, WHD_NETWORK_RX) );

    return WHD_SUCCESS;
}

whd_result_t whd_wifi_set_coex_config(whd_interface_t ifp, whd_coex_config_t *coex_config)
{
    CHECK_IFP_NULL(ifp);

    if (coex_config == NULL)
        return WHD_BADARG;

    return whd_wifi_set_iovar_buffer(ifp, IOVAR_STR_BTC_LESCAN_PARAMS, &coex_config->le_scan_params,
                                     sizeof(whd_btc_lescan_params_t) );
}

whd_result_t whd_wifi_set_auth_status(whd_interface_t ifp, whd_auth_req_status_t *params)
{
    whd_buffer_t buffer;
    whd_driver_t whd_driver;
    whd_auth_req_status_t *auth_status;

    CHECK_IFP_NULL(ifp);

    whd_driver = ifp->whd_driver;

    CHECK_DRIVER_NULL(whd_driver);

    if (params == NULL)
    {
        WPRINT_WHD_ERROR( ("Invalid param in func %s at line %d \n", __func__, __LINE__) );
        return WHD_WLAN_BADARG;
    }

    auth_status = (whd_auth_req_status_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer,
                                                                      sizeof(whd_auth_req_status_t),
                                                                      IOVAR_STR_AUTH_STATUS);
    CHECK_IOCTL_BUFFER (auth_status);
    memcpy(auth_status, params, sizeof(whd_auth_req_status_t) );
    if (params->flags == DOT11_SC_SUCCESS)
    {
        auth_status->flags = WL_EXTAUTH_SUCCESS;
    }
    else
    {
        auth_status->flags = WL_EXTAUTH_FAIL;
    }
    RETURN_WITH_ASSERT(whd_proto_set_iovar(ifp, buffer, NULL) );
}

whd_result_t whd_wifi_get_fwcap(whd_interface_t ifp, uint32_t *value)
{
    whd_driver_t whd_driver;

    CHECK_IFP_NULL(ifp);

    whd_driver = ifp->whd_driver;

    CHECK_DRIVER_NULL(whd_driver);

    *value = whd_driver->chip_info.fwcap_flags;
    return WHD_SUCCESS;
}

/*
 * ARP Offload version
 *    ARP version in the WLAN Firmware
 *
 * @param[in]    ifp            - whd interface Instance
 * @param[out]    version        - pointer to store version #
 *
 * @return @ref whd_result_t
 */
whd_result_t whd_arp_version(whd_interface_t ifp, uint32_t *value)
{
    CHECK_IFP_NULL(ifp);

    return whd_wifi_get_iovar_value(ifp, IOVAR_STR_ARP_VERSION, value);
}

whd_result_t whd_arp_peerage_get(whd_interface_t ifp, uint32_t *value)
{
    CHECK_IFP_NULL(ifp);

    return whd_wifi_get_iovar_value(ifp, IOVAR_STR_ARP_PEERAGE, value);
}

whd_result_t whd_arp_peerage_set(whd_interface_t ifp, uint32_t value)
{
    CHECK_IFP_NULL(ifp);

    return whd_wifi_set_iovar_value(ifp, IOVAR_STR_ARP_PEERAGE, value);
}

whd_result_t whd_arp_arpoe_get(whd_interface_t ifp, uint32_t *value)
{
    CHECK_IFP_NULL(ifp);

    return whd_wifi_get_iovar_value(ifp, IOVAR_STR_ARPOE, value);
}

whd_result_t whd_arp_arpoe_set(whd_interface_t ifp, uint32_t value)
{
    CHECK_IFP_NULL(ifp);

    return whd_wifi_set_iovar_value(ifp, IOVAR_STR_ARPOE, value);
}

whd_result_t whd_arp_cache_clear(whd_interface_t ifp)
{
    whd_result_t whd_ret;
    CHECK_IFP_NULL(ifp);

    whd_ret = whd_wifi_set_iovar_void(ifp, IOVAR_STR_ARP_TABLE_CLEAR);
    return whd_ret;
}

whd_result_t whd_arp_features_get(whd_interface_t ifp, uint32_t *features)
{
    if ( (ifp == NULL) || (features == NULL) )
    {
        return WHD_BADARG;
    }

    if (whd_wifi_get_iovar_buffer(ifp, IOVAR_STR_ARP_OL, (uint8_t *)features, sizeof(uint32_t) ) != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR( ("%s() failed to get arp_ol for features\n", __func__) );
        return WHD_IOCTL_FAIL;
    }

    return WHD_SUCCESS;
}

whd_result_t whd_arp_features_set(whd_interface_t ifp, uint32_t features)
{
    CHECK_IFP_NULL(ifp);

    return whd_wifi_set_iovar_buffer(ifp, IOVAR_STR_ARP_OL, (uint8_t *)&features, sizeof(features) );
}

whd_result_t whd_arp_features_print(uint32_t features, const char *title)
{
    if (title != NULL)
    {
        WPRINT_MACRO( ("%s\n", title) );
    }
    WPRINT_MACRO( ("            features     : 0x%x\n", (int)features) );
    WPRINT_MACRO( ("            agent_enabled: (0x%x) %s\n", (int)(features & ARP_OL_AGENT),
                   (features & ARP_OL_AGENT) ? "Enabled" : "  disabled") );
    WPRINT_MACRO( ("            snoop_enabled: (0x%x) %s\n", (int)(features & ARP_OL_SNOOP),
                   (features & ARP_OL_SNOOP) ? "Enabled" : "  disabled") );
    WPRINT_MACRO( ("  host_auto_reply_enabled: (0x%x) %s\n", (int)(features & ARP_OL_HOST_AUTO_REPLY),
                   (features & ARP_OL_HOST_AUTO_REPLY) ? "Enabled" : "  disabled") );
    WPRINT_MACRO( ("  peer_auto_reply_enabled: (0x%x) %s\n", (int)(features & ARP_OL_PEER_AUTO_REPLY),
                   (features & ARP_OL_PEER_AUTO_REPLY) ? "Enabled" : "  disabled") );

    return WHD_SUCCESS;
}

whd_result_t whd_arp_hostip_list_add(whd_interface_t ifp, uint32_t *host_ipv4_list, uint32_t count)
{
    uint32_t filled = 0;
    uint32_t current_ipv4_list[ARP_MULTIHOMING_MAX];
    CHECK_IFP_NULL(ifp);

    whd_result_t whd_ret = WHD_SUCCESS;
    if (host_ipv4_list == NULL)
    {
        WPRINT_WHD_ERROR( ("%s() BAD ARGS ifp:%p host_ipv4_list:%u count %d\n", __func__, ifp, (int)host_ipv4_list,
                           (int)count) );
        return WHD_BADARG;
    }
    /* check if unique */
    whd_ret = whd_arp_hostip_list_get(ifp, ARP_MULTIHOMING_MAX, current_ipv4_list, &filled);
    if ( (whd_ret == WHD_SUCCESS) && (filled > 0) )
    {
        uint32_t curr_index;
        uint32_t new_index;

        for (curr_index = 0; curr_index < filled; curr_index++)
        {
            for (new_index = 0; new_index < count; new_index++)
            {
                WPRINT_WHD_DEBUG( ("%s() curr:%ld of %ld curr:0x%lx new:%ld of %ld:0x%lx\n", __func__, curr_index,
                                   filled, current_ipv4_list[curr_index],
                                   new_index, count, host_ipv4_list[new_index]) );
                if (current_ipv4_list[curr_index] == host_ipv4_list[new_index])
                {
                    /* decrement count */
                    count--;
                    if (new_index < count)
                    {
                        /* copy next one down */
                        WPRINT_WHD_DEBUG( ("move %ld (+1) of %ld \n", new_index, count) );
                        host_ipv4_list[new_index] = host_ipv4_list[new_index + 1];

                    }
                    break;
                }
            }
        }
    }
    else if (whd_ret != WHD_SUCCESS)
    {
        WPRINT_WHD_DEBUG( ("%s() whd_arp_hostip_list_get() failed:%d\n", __func__, (int)whd_ret) );
    }

    if (count > 0)
    {
        uint32_t new_index;
        WPRINT_WHD_DEBUG( ("%s() whd_wifi_set_iovar_buffer( %p, %lx)\n", __func__, host_ipv4_list, count) );
        for (new_index = 0; new_index < count; new_index++)
        {
            WPRINT_WHD_DEBUG( ("  0x%lx\n", host_ipv4_list[new_index]) );
        }
#ifdef CYCFG_ULP_SUPPORT_ENABLED
        CHECK_RETURN(whd_configure_wowl(ifp, WL_WOWL_ARPOFFLOAD));
        whd_ret = whd_wifi_set_iovar_buffer(ifp, IOVAR_STR_WOWL_ARP_HOST_IP, host_ipv4_list, (count * sizeof(uint32_t) ) );
        if (whd_ret != WHD_SUCCESS)
        {
            WPRINT_WHD_ERROR( ("Failed to set arp_hostip 0x%x error:%d\n", (int)host_ipv4_list[0], (int)whd_ret) );
        }
#endif
        whd_ret = whd_wifi_set_iovar_buffer(ifp, IOVAR_STR_ARP_HOSTIP, host_ipv4_list, (count * sizeof(uint32_t) ) );
        if (whd_ret != WHD_SUCCESS)
        {
            WPRINT_WHD_ERROR( ("Failed to set arp_hostip 0x%x error:%d\n", (int)host_ipv4_list[0], (int)whd_ret) );
        }
    }
    return whd_ret;
}

whd_result_t whd_arp_hostip_list_add_string(whd_interface_t ifp, const char *ip_addr)
{
    /* convert string to uint32_t */
    uint32_t addr;
    CHECK_IFP_NULL(ifp);

    whd_str_to_ip(ip_addr, strlen(ip_addr), &addr);

    return whd_arp_hostip_list_add(ifp, &addr, 1);
}

whd_result_t whd_arp_hostip_list_clear_id(whd_interface_t ifp, uint32_t ipv4_addr)
{
    whd_result_t whd_ret;
    uint32_t filled;
    uint32_t host_ipv4_list[ARP_MULTIHOMING_MAX];
    CHECK_IFP_NULL(ifp);

    if (ipv4_addr == 0x00l)
    {
        return WHD_BADARG;
    }
    memset(host_ipv4_list, 0x00, sizeof(host_ipv4_list) );
    whd_ret = whd_arp_hostip_list_get(ifp, ARP_MULTIHOMING_MAX, host_ipv4_list, &filled);
    if ( (whd_ret == WHD_SUCCESS) && (filled > 0) )
    {
        uint32_t index;

        /* clear the list in the WLAN processor */
        whd_ret = whd_wifi_set_iovar_void(ifp, IOVAR_STR_ARP_HOSTIP_CLEAR);
        if (whd_ret != WHD_SUCCESS)
        {
            WPRINT_WHD_ERROR( ("%d %s() whd_wifi_set_iovar_void() failed:%d\n", __LINE__, __func__, (int)whd_ret) );
            return whd_ret;
        }

        /* remove the one address from the list and re-write arp_hostip list */
        for (index = 0; index < filled; index++)
        {
            WPRINT_WHD_DEBUG( ("%d %s() drop() 0x%lx == 0x%lx ? %s\n", __LINE__, __func__, host_ipv4_list[index],
                               ipv4_addr, (host_ipv4_list[index] == ipv4_addr) ? "DROP" : "") );
            if (host_ipv4_list[index] == ipv4_addr)
            {
                uint32_t drop;
                /* drop this one, move rest up */
                for (drop = index; drop < (filled - 1); drop++)
                {
                    host_ipv4_list[drop] = host_ipv4_list[drop + 1];
                }
                filled--;
                /* IP addresses must be added one at a time */
                for (drop = 0; drop < filled; drop++)
                {
                    whd_ret = whd_arp_hostip_list_add(ifp, &host_ipv4_list[drop], sizeof(uint32_t) );
                }
                break;
            }
        }
    }
    else if (whd_ret != WHD_SUCCESS)
    {
        WPRINT_WHD_DEBUG( ("%s() whd_arp_hostip_list_get() failed:%d\n", __func__, (int)whd_ret) );
    }
    return WHD_SUCCESS;
}

whd_result_t whd_arp_hostip_list_clear_id_string(whd_interface_t ifp, const char *ip_addr)
{
    /* convert string to uint32_t */
    uint32_t addr;
    CHECK_IFP_NULL(ifp);

    whd_str_to_ip(ip_addr, strlen(ip_addr), &addr);

    return whd_arp_hostip_list_clear_id(ifp, addr);
}

whd_result_t whd_arp_hostip_list_clear(whd_interface_t ifp)
{
    CHECK_IFP_NULL(ifp);
    return whd_wifi_set_iovar_void(ifp, IOVAR_STR_ARP_HOSTIP_CLEAR);
}

whd_result_t whd_arp_hostip_list_get(whd_interface_t ifp, uint32_t count, uint32_t *host_ipv4_list, uint32_t *filled)
{
    whd_result_t whd_ret = WHD_SUCCESS;
    uint32_t temp[ARP_MULTIHOMING_MAX];
    arp_ol_stats_t arp_stats;               /* WL struct, not ours! */
    CHECK_IFP_NULL(ifp);

    if ( (host_ipv4_list == NULL) || (filled == NULL) )
    {
        return WHD_BADARG;
    }

    /* set up the buffer to retrieve the stats data */
    memset(&arp_stats, 0x00, sizeof(arp_ol_stats_t) );
    whd_ret = whd_wifi_get_iovar_buffer(ifp, "arp_stats", (uint8_t *)&arp_stats, sizeof(arp_ol_stats_t) );
    if (whd_ret != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR( ("%s() failed to get arp_stats\n", __func__) );
        return WHD_IOCTL_FAIL;
    }

    *filled = 0;
    whd_ret =  whd_wifi_get_iovar_buffer(ifp, IOVAR_STR_ARP_HOSTIP, (uint8_t *)&temp, sizeof(temp) );
    /* transfer the info */
    if (whd_ret == WHD_SUCCESS)
    {
        uint32_t index;
        for (index = 0; (index < count) && (index < arp_stats.host_ip_entries); index++)
        {
            /* only IPv4 !!! */
            if (htod32(temp[index]) != 0L)
            {
                host_ipv4_list[*filled] =  temp[index];
                *filled = *filled + 1;
            }
        }
    }
    return whd_ret;
}

whd_result_t whd_arp_stats_clear(whd_interface_t ifp)
{
    whd_result_t whd_ret;
    CHECK_IFP_NULL(ifp);
    whd_ret = whd_wifi_set_iovar_void(ifp, IOVAR_STR_ARP_STATS_CLEAR);
    return whd_ret;
}

whd_result_t whd_arp_stats_get(whd_interface_t ifp, whd_arp_stats_t *arp_stats)
{
    whd_result_t whd_ret;
    uint32_t filled;
    static whd_arp_stats_t arp_stats_test;  /* read twice to make sure we match */
    CHECK_IFP_NULL(ifp);

    if (arp_stats == NULL)
    {
        return WHD_BADARG;
    }

    /* set up the buffer to retreive the data */
    memcpy(&arp_stats_test, arp_stats, sizeof(whd_arp_stats_t) );
    memset(arp_stats, 0xFF, sizeof(whd_arp_stats_t) );

    /* read multiple times to make sure we got valid data */
    do
    {
        /* get them until they match */
        whd_ret =
            whd_wifi_get_iovar_buffer(ifp, IOVAR_STR_ARP_STATS,   (uint8_t *)&arp_stats->stats,
                                      sizeof(arp_ol_stats_t) );
        if (whd_ret != WHD_SUCCESS)
        {
            WPRINT_WHD_ERROR( ("%s() failed to get arp_stats\n", __func__) );
            return WHD_IOCTL_FAIL;
        }
        /* get all feature info in one call */
        whd_ret =
            whd_wifi_get_iovar_buffer(ifp, IOVAR_STR_ARP_OL, (uint8_t *)&arp_stats->features_enabled,
                                      sizeof(arp_stats->features_enabled) );
        if (whd_ret != WHD_SUCCESS)
        {
            WPRINT_WHD_ERROR( ("%s() failed to get arp_ol\n", __func__) );
            return WHD_IOCTL_FAIL;
        }
        whd_ret = whd_wifi_get_iovar_value(ifp, IOVAR_STR_ARP_VERSION, &(arp_stats->version) );
        if (whd_ret != WHD_SUCCESS)
        {
            WPRINT_WHD_ERROR( ("%s() failed to get arp_version\n", __func__) );
            return WHD_IOCTL_FAIL;
        }
        whd_ret = whd_wifi_get_iovar_value(ifp, IOVAR_STR_ARP_PEERAGE, &(arp_stats->peerage) );
        if (whd_ret != WHD_SUCCESS)
        {
            WPRINT_WHD_ERROR( ("%s() failed to get arp_peerage\n", __func__) );
            return WHD_IOCTL_FAIL;
        }
        whd_ret = whd_wifi_get_iovar_value(ifp, IOVAR_STR_ARPOE, &(arp_stats->arpoe) );
        if (whd_ret != WHD_SUCCESS)
        {
            WPRINT_WHD_ERROR( ("%s() failed to get some settings\n", __func__) );
            return WHD_IOCTL_FAIL;
        }

        /* set endian correctly */
        arp_stats->stats.host_ip_entries     = dtoh32(arp_stats->stats.host_ip_entries);
        arp_stats->stats.host_ip_overflow     = dtoh32(arp_stats->stats.host_ip_overflow);
        arp_stats->stats.arp_table_entries     = dtoh32(arp_stats->stats.arp_table_entries);
        arp_stats->stats.arp_table_overflow = dtoh32(arp_stats->stats.arp_table_overflow);
        arp_stats->stats.host_request         = dtoh32(arp_stats->stats.host_request);
        arp_stats->stats.host_reply         = dtoh32(arp_stats->stats.host_reply);
        arp_stats->stats.host_service         = dtoh32(arp_stats->stats.host_service);
        arp_stats->stats.peer_request         = dtoh32(arp_stats->stats.peer_request);
        arp_stats->stats.peer_request_drop     = dtoh32(arp_stats->stats.peer_request_drop);
        arp_stats->stats.peer_reply         = dtoh32(arp_stats->stats.peer_reply);
        arp_stats->stats.peer_reply_drop     = dtoh32(arp_stats->stats.peer_reply_drop);
        arp_stats->stats.peer_service         = dtoh32(arp_stats->stats.peer_service);

        whd_ret = whd_arp_hostip_list_get(ifp, ARP_MULTIHOMING_MAX, arp_stats->host_ip_list, &filled);
        if (whd_ret != WHD_SUCCESS)
        {
            WPRINT_WHD_ERROR( ("%s() failed to get host_ip_list\n", __func__) );
            return WHD_IOCTL_FAIL;
        }

        if (memcmp(&arp_stats_test, arp_stats, sizeof(whd_arp_stats_t) ) == 0)
        {
            break;
        }

        memcpy(&arp_stats_test, arp_stats, sizeof(whd_arp_stats_t) );
    } while (1);

    return whd_ret;
}

whd_result_t whd_arp_stats_print(whd_arp_stats_t *arp_stats, const char *title)
{
    uint32_t index;

    if (arp_stats == NULL)
    {
        return WHD_BADARG;
    }

    if (title != NULL)
    {
        WPRINT_MACRO( ("%s\n", title) );
    }
    WPRINT_MACRO( ("                  version: 0x%lx\n", (unsigned long int)arp_stats->version) );
    WPRINT_MACRO( ("          host_ip_entries: %d\n", (int)arp_stats->stats.host_ip_entries) );
    WPRINT_MACRO( ("         host_ip_overflow: %d\n", (int)arp_stats->stats.host_ip_overflow) );
    WPRINT_MACRO( ("        arp_table_entries: %d\n", (int)arp_stats->stats.arp_table_entries) );
    WPRINT_MACRO( ("       arp_table_overflow: %d\n", (int)arp_stats->stats.arp_table_overflow) );
    WPRINT_MACRO( ("             host_request: %d\n", (int)arp_stats->stats.host_request) );
    WPRINT_MACRO( ("               host_reply: %d\n", (int)arp_stats->stats.host_reply) );
    WPRINT_MACRO( ("             host_service: %d\n", (int)arp_stats->stats.host_service) );
    WPRINT_MACRO( ("             peer_request: %d\n", (int)arp_stats->stats.peer_request) );
    WPRINT_MACRO( ("        peer_request_drop: %d\n", (int)arp_stats->stats.peer_request_drop) );
    WPRINT_MACRO( ("               peer_reply: %d\n", (int)arp_stats->stats.peer_reply) );
    WPRINT_MACRO( ("          peer_reply_drop: %d\n", (int)arp_stats->stats.peer_reply_drop) );
    WPRINT_MACRO( ("             peer_service: %d\n", (int)arp_stats->stats.peer_service) );
    WPRINT_MACRO( ("                  peerage: %d\n", (int)arp_stats->peerage) );
    WPRINT_MACRO( ("                    arpoe: %d %s\n", (int)arp_stats->arpoe,
                   (arp_stats->arpoe != 0) ? "Enabled" : "  disabled") );

    whd_arp_features_print(arp_stats->features_enabled, NULL);

    if (arp_stats->stats.host_ip_entries > 0)
    {
        WPRINT_MACRO( ("WLAN Device Host IP entries\n") );
        for (index = 0; index < arp_stats->stats.host_ip_entries; index++)
        {
            uint32_t ipv4_addr = arp_stats->host_ip_list[index];
            char ipv4_string[32];
            memset(ipv4_string, 0x00, sizeof(ipv4_string) );
            whd_ip4_to_string(&ipv4_addr, ipv4_string);
            WPRINT_MACRO( ("  %d of %d IPV4: 0x%x %s\n", (int)index, (int)arp_stats->stats.host_ip_entries,
                           (int)arp_stats->host_ip_list[index], ipv4_string) );
        }
    }
    return WHD_SUCCESS;
}

whd_result_t
whd_wifi_toggle_packet_filter(whd_interface_t ifp, uint8_t filter_id, whd_bool_t enable)
{
    whd_buffer_t buffer;
    whd_driver_t whd_driver;

    CHECK_IFP_NULL(ifp);

    whd_driver = ifp->whd_driver;
    wl_pkt_filter_enable_t *data = (wl_pkt_filter_enable_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer,
                                                                                        sizeof(wl_pkt_filter_enable_t),
                                                                                        IOVAR_STR_PKT_FILTER_ENABLE);
    CHECK_IOCTL_BUFFER(data);
    data->id     = (uint32_t)filter_id;
    data->enable = (uint32_t)enable;
    RETURN_WITH_ASSERT(whd_proto_set_iovar(ifp, buffer, NULL) );
}

whd_result_t
whd_pf_enable_packet_filter(whd_interface_t ifp, uint8_t filter_id)
{
    return whd_wifi_toggle_packet_filter(ifp, filter_id, WHD_TRUE);
}

whd_result_t
whd_pf_disable_packet_filter(whd_interface_t ifp, uint8_t filter_id)
{
    return whd_wifi_toggle_packet_filter(ifp, filter_id, WHD_FALSE);
}

whd_result_t
whd_pf_add_packet_filter(whd_interface_t ifp, const whd_packet_filter_t *settings)
{
    wl_pkt_filter_t *packet_filter;
    whd_driver_t whd_driver;
    whd_buffer_t buffer;
    uint32_t buffer_length =
        (uint32_t)( (2 * (uint32_t)settings->mask_size) + WL_PKT_FILTER_FIXED_LEN + WL_PKT_FILTER_PATTERN_FIXED_LEN );

    CHECK_IFP_NULL(ifp);

    whd_driver = ifp->whd_driver;

    packet_filter = (wl_pkt_filter_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer, (uint16_t)buffer_length,
                                                                  IOVAR_STR_PKT_FILTER_ADD);
    CHECK_IOCTL_BUFFER(packet_filter);

    /* Copy filter entries */
    packet_filter->id                   = settings->id;
    packet_filter->type                 = 0;
    packet_filter->negate_match         = settings->rule;
    packet_filter->u.pattern.offset     = (uint32_t)settings->offset;
    packet_filter->u.pattern.size_bytes = settings->mask_size;

    /* Copy mask */
    memcpy(packet_filter->u.pattern.mask_and_pattern, settings->mask, settings->mask_size);

    /* Copy filter pattern */
    memcpy(packet_filter->u.pattern.mask_and_pattern + settings->mask_size, settings->pattern, settings->mask_size);

    RETURN_WITH_ASSERT(whd_proto_set_iovar(ifp, buffer, NULL) );
}

whd_result_t
whd_pf_remove_packet_filter(whd_interface_t ifp, uint8_t filter_id)
{
    whd_buffer_t buffer;
    whd_driver_t whd_driver;
    CHECK_IFP_NULL(ifp);

    whd_driver = ifp->whd_driver;

    uint32_t *data = (uint32_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer, sizeof(uint32_t),
                                                            IOVAR_STR_PKT_FILTER_DELETE);
    CHECK_IOCTL_BUFFER(data);
    *data = (uint32_t)filter_id;
    RETURN_WITH_ASSERT(whd_proto_set_iovar(ifp, buffer, NULL) );
}

whd_result_t
whd_pf_get_packet_filter_stats(whd_interface_t ifp, uint8_t filter_id, whd_pkt_filter_stats_t *stats)
{
    whd_buffer_t buffer;
    whd_buffer_t response;
    whd_driver_t whd_driver;
    uint8_t *pdata;

    CHECK_IFP_NULL(ifp);

    whd_driver = ifp->whd_driver;

    uint32_t *data =
        (uint32_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer, sizeof(uint32_t) + sizeof(wl_pkt_filter_stats_t),
                                               IOVAR_STR_PKT_FILTER_STATS);
    CHECK_IOCTL_BUFFER(data);

    memset(data, 0, sizeof(uint32_t) + sizeof(wl_pkt_filter_stats_t) );
    *data = (uint32_t)filter_id;

    CHECK_RETURN(whd_proto_get_iovar(ifp, buffer, &response) );
    pdata = whd_buffer_get_current_piece_data_pointer(whd_driver, response);
    CHECK_PACKET_NULL(pdata, WHD_NO_REGISTER_FUNCTION_POINTER);
    memcpy( (char *)stats, (char *)pdata, (sizeof(wl_pkt_filter_stats_t) ) );
    CHECK_RETURN(whd_buffer_release(whd_driver, response, WHD_NETWORK_TX) );

    return WHD_SUCCESS;
}

whd_result_t
whd_wifi_clear_packet_filter_stats(whd_interface_t ifp, uint32_t filter_id)
{
    RETURN_WITH_ASSERT(whd_wifi_set_iovar_value(ifp, IOVAR_STR_PKT_FILTER_CLEAR_STATS, (uint32_t)filter_id) );
}

whd_result_t
whd_pf_get_packet_filter_mask_and_pattern(whd_interface_t ifp, uint8_t filter_id, uint32_t max_size, uint8_t *mask,
                                          uint8_t *pattern, uint32_t *size_out)
{
    whd_bool_t enabled_list;
    whd_driver_t whd_driver;
    CHECK_IFP_NULL(ifp);

    whd_driver = ifp->whd_driver;

    for (enabled_list = WHD_FALSE; enabled_list <= WHD_TRUE; enabled_list++)
    {

        whd_buffer_t buffer;
        whd_buffer_t response;
        uint32_t *data;
        wl_pkt_filter_list_t *filter_list;
        wl_pkt_filter_t *filter_ptr;
        uint32_t i;
        wl_pkt_filter_t *in_filter;

        data = whd_proto_get_iovar_buffer(whd_driver, &buffer, PACKET_FILTER_LIST_BUFFER_MAX_LEN,
                                          IOVAR_STR_PKT_FILTER_LIST);
        CHECK_IOCTL_BUFFER(data);
        *data = (uint32_t)enabled_list;

        CHECK_RETURN(whd_proto_get_iovar(ifp, buffer, &response) );

        filter_list  = (wl_pkt_filter_list_t *)whd_buffer_get_current_piece_data_pointer(whd_driver, response);
        filter_ptr   = filter_list->filter;
        for (i = 0; i < filter_list->num; i++)
        {
            in_filter  = filter_ptr;

            if (in_filter->id == filter_id)
            {
                *size_out = MIN_OF(in_filter->u.pattern.size_bytes, max_size);
                memcpy (mask,    in_filter->u.pattern.mask_and_pattern, *size_out);
                memcpy (pattern, in_filter->u.pattern.mask_and_pattern + in_filter->u.pattern.size_bytes, *size_out);
                CHECK_RETURN(whd_buffer_release(whd_driver, response, WHD_NETWORK_RX) );
                if (*size_out < in_filter->u.pattern.size_bytes)
                {
                    return WHD_PARTIAL_RESULTS;
                }
                return WHD_SUCCESS;
            }

            /* Update WL filter pointer */
            filter_ptr =
                (wl_pkt_filter_t *)( (char *)filter_ptr +
                                     (WL_PKT_FILTER_FIXED_LEN + WL_PKT_FILTER_PATTERN_FIXED_LEN + 2 *
                                      in_filter->u.pattern.size_bytes) );

            /* WLAN returns word-aligned filter list */
            filter_ptr = (wl_pkt_filter_t *)ROUND_UP( (unsigned long)filter_ptr, 4 );
        }
    }
    return WHD_FILTER_NOT_FOUND;
}

/* Set/Get TKO retry & interval parameters */
whd_result_t
whd_tko_param(whd_interface_t ifp, whd_tko_retry_t *whd_retry, uint8_t set)
{
    uint32_t len = 0;
    uint8_t *data = NULL;
    wl_tko_t *tko = NULL;
    whd_buffer_t buffer;
    whd_buffer_t response;
    wl_tko_param_t *wl_param_p = NULL;
    whd_result_t result = WHD_SUCCESS;
    whd_driver_t whd_driver;
    CHECK_IFP_NULL(ifp);

    whd_driver = ifp->whd_driver;

    len = (int)(WHD_PAYLOAD_MTU - strlen(IOVAR_STR_TKO) - 1);
    data = (uint8_t * )whd_proto_get_iovar_buffer(whd_driver, &buffer, (uint16_t)len, IOVAR_STR_TKO);
    if (data == NULL)
    {
        WPRINT_WHD_ERROR( ("%s: Failed to get iovar buf\n", __func__) );
        return WHD_IOCTL_FAIL;
    }

    tko = (wl_tko_t *)data;
    tko->subcmd_id = WL_TKO_SUBCMD_PARAM;
    tko->len = TKO_DATA_OFFSET;
    wl_param_p = (wl_tko_param_t *)tko->data;
    tko->len += sizeof(wl_tko_param_t);

    tko->subcmd_id = htod16(tko->subcmd_id);
    tko->len = htod16(tko->len);

    if (set)
    {
        /* SET parameters */

        /* Set defaults if needed */
        wl_param_p->interval = whd_retry->tko_interval ==
                               0 ? TCP_KEEPALIVE_OFFLOAD_INTERVAL_SEC : whd_retry->tko_interval;
        wl_param_p->retry_count = whd_retry->tko_retry_count ==
                                  0 ? TCP_KEEPALIVE_OFFLOAD_RETRY_COUNT : whd_retry->tko_retry_count;
        wl_param_p->retry_interval = whd_retry->tko_retry_interval ==
                                     0 ? TCP_KEEPALIVE_OFFLOAD_RETRY_INTERVAL_SEC : whd_retry->tko_retry_interval;

        result = whd_proto_set_iovar(ifp, buffer, NULL);
        if (result != WHD_SUCCESS)
        {
            WPRINT_WHD_ERROR( ("%s: Cannot set params\n", __func__) );
        }
    }
    else
    {
        /* GET paramters */
        wl_tko_param_t tko_param_real;

        result = whd_proto_get_iovar(ifp, buffer, &response);
        if (result == WHD_SUCCESS)
        {
            wl_param_p = &tko_param_real;
            memcpy( (char *)wl_param_p,
                    (char *)whd_buffer_get_current_piece_data_pointer(whd_driver, response) + TKO_DATA_OFFSET,
                    (sizeof(wl_tko_param_t) ) );
            CHECK_RETURN(whd_buffer_release(whd_driver, response, WHD_NETWORK_TX) );

            /* Copy items from wl level struct to higher level struct */
            whd_retry->tko_interval = wl_param_p->interval;
            whd_retry->tko_retry_interval = wl_param_p->retry_interval;
            whd_retry->tko_retry_count = wl_param_p->retry_count;
        }
        else
        {
            WPRINT_WHD_ERROR( ("%s: Cannot get params.\n", __func__) );
        }
    }

    return result;
}

/* Query Status */
whd_result_t
whd_tko_get_status(whd_interface_t ifp, whd_tko_status_t *whd_status)
{
    whd_result_t result = WHD_SUCCESS;
    uint32_t len = 0;
    uint8_t *data = NULL;
    wl_tko_t *tko = NULL;
    whd_buffer_t buffer;
    whd_buffer_t response;
    whd_driver_t whd_driver;
    CHECK_IFP_NULL(ifp);

    whd_driver = ifp->whd_driver;

    /* Get a buffer */
    len = (int)(100 - strlen(IOVAR_STR_TKO) - 1);
    data = (uint8_t * )whd_proto_get_iovar_buffer(whd_driver, &buffer, (uint16_t)len, IOVAR_STR_TKO);
    CHECK_IOCTL_BUFFER(data);

    /* Fill buffer with request */
    tko = (wl_tko_t *)data;
    tko->subcmd_id = WL_TKO_SUBCMD_STATUS;
    tko->len = TKO_DATA_OFFSET;

    tko->len += sizeof(wl_tko_status_t);

    tko->subcmd_id = htod16(tko->subcmd_id);
    tko->len = htod16(tko->len);

    /* Make request and get result */
    result = whd_proto_get_iovar(ifp, buffer, &response);
    if (result != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR( ("%s: send iovar failed\n", __func__) );
        return result;
    }

    /* Parse result */
    tko = (wl_tko_t *)whd_buffer_get_current_piece_data_pointer(whd_driver, response);
    if (tko)
    {
        len = htod16(tko->len);

        if (len >= MAX_TKO_CONN + 1)   /* MAX_TKO status's + 1 for the count */
        {
            memcpy(whd_status, tko->data, MAX_TKO_CONN + 1);
        }
    }
    CHECK_RETURN(whd_buffer_release(whd_driver, response, WHD_NETWORK_TX) );
    return result;
}

/* Query FW for number tko max tcp connections */
whd_result_t
whd_tko_max_assoc(whd_interface_t ifp, uint8_t *max)
{
    uint32_t len = 0;
    uint8_t *data = NULL;
    uint8_t *pdata = NULL;
    wl_tko_t *tko = NULL;
    whd_buffer_t buffer;
    whd_buffer_t response;
    wl_tko_max_tcp_t *tko_max_tcp = NULL;
    wl_tko_max_tcp_t tcp_result;
    whd_driver_t whd_driver;
    whd_result_t result = WHD_SUCCESS;
    CHECK_IFP_NULL(ifp);

    whd_driver = ifp->whd_driver;

    len = (int)(100 - strlen(IOVAR_STR_TKO) - 1);
    data = (uint8_t * )whd_proto_get_iovar_buffer(whd_driver, &buffer, (uint16_t)len, IOVAR_STR_TKO);
    CHECK_IOCTL_BUFFER(data);

    tko = (wl_tko_t *)data;

    tko->subcmd_id = WL_TKO_SUBCMD_MAX_TCP;
    tko->len = TKO_DATA_OFFSET;

    tko_max_tcp = (wl_tko_max_tcp_t *)tko->data;
    tko->len += sizeof(wl_tko_max_tcp_t);

    tko->subcmd_id = htod16(tko->subcmd_id);
    tko->len = htod16(tko->len);

    result = whd_proto_get_iovar(ifp, buffer, &response);
    if (result != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR( ("%s: send iovar failed\n", __func__) );
        return result;
    }
    tko_max_tcp = &tcp_result;
    pdata = whd_buffer_get_current_piece_data_pointer(whd_driver, response);
    CHECK_PACKET_NULL(pdata, WHD_NO_REGISTER_FUNCTION_POINTER);
    memcpy( (char *)tko_max_tcp,
            (char *)pdata + TKO_DATA_OFFSET,
            (sizeof(wl_tko_max_tcp_t) ) );
    CHECK_RETURN(whd_buffer_release(whd_driver, response, WHD_NETWORK_TX) );

    *max = tko_max_tcp->max;
    return WHD_SUCCESS;
}

/* Exercise GET of wl_tko_connect_t IOVAR */
/* Given a index, return info about that index */
whd_result_t
whd_tko_get_FW_connect(whd_interface_t ifp, uint8_t index, whd_tko_connect_t *whd_connect, uint16_t buflen)
{
    uint32_t len = 0;
    uint8_t *data = NULL;
    wl_tko_t *tko = NULL;
    wl_tko_connect_t *connect = NULL;
    whd_result_t result = WHD_SUCCESS;
    whd_buffer_t response;
    whd_buffer_t buffer;
    whd_driver_t whd_driver;
    CHECK_IFP_NULL(ifp);

    whd_driver = ifp->whd_driver;
    CHECK_DRIVER_NULL(whd_driver);

    len = (int)(WHD_PAYLOAD_MTU - strlen(IOVAR_STR_TKO) - 1);
    data = (uint8_t * )whd_proto_get_iovar_buffer(whd_driver, &buffer, (uint16_t)len, IOVAR_STR_TKO);
    CHECK_IOCTL_BUFFER(data);

    tko = (wl_tko_t *)data;

    tko->subcmd_id = WL_TKO_SUBCMD_CONNECT;
    tko->len = offsetof(wl_tko_t, data);
    connect = (wl_tko_connect_t *)tko->data;
    connect->index = index;

    tko->subcmd_id = htod16(tko->subcmd_id);
    tko->len = htod16(tko->len);

    result = whd_proto_get_iovar(ifp, buffer, &response);
    if (result != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR( ("%s: send iovar failed\n", __func__) );
        return result;
    }
    tko = (wl_tko_t *)whd_buffer_get_current_piece_data_pointer(whd_driver, response);
    tko->subcmd_id = dtoh16(tko->subcmd_id);
    tko->len = dtoh16(tko->len);

    if (tko->subcmd_id  != WL_TKO_SUBCMD_CONNECT)
    {
        WPRINT_WHD_ERROR( ("%s: IOVAR returned garbage!\n", __func__) );
        return WHD_BADARG;
    }
    connect = (wl_tko_connect_t *)tko->data;
    if (tko->len >= sizeof(*connect) )
    {
        connect->local_port = dtoh16(connect->local_port);
        connect->remote_port = dtoh16(connect->remote_port);
        connect->local_seq = dtoh32(connect->local_seq);
        connect->remote_seq = dtoh32(connect->remote_seq);
        if (connect->ip_addr_type != 0)
        {
            WPRINT_WHD_ERROR( ("%s: Address type not IPV4\n", __func__) );
            return WHD_BADARG;
        }
        if (connect->ip_addr_type == 0)
        {
            /* IPv4 */
            uint16_t mylen;
            mylen = sizeof(wl_tko_connect_t) + (2 * IPV4_ADDR_LEN) + connect->request_len + connect->response_len;
            if (buflen < mylen)
            {
                WPRINT_WHD_ERROR( ("%s: Buf len (%d) too small , need %d\n", __func__, buflen, mylen) );
                return WHD_BADARG;
            }

            /*
             * Assumes whd_tko_connect_t and wl_tko_connect_t are the same.
             * If/when they become different (due to different FW versions, etc) than
             * this may have to be copied field by field instead.
             */
            memcpy(whd_connect, connect, MIN_OF(mylen, buflen) );
        }
    }
    return WHD_SUCCESS;
}

whd_result_t
whd_tko_toggle(whd_interface_t ifp, whd_bool_t enable)
{
    uint32_t len = 0;
    uint8_t *data = NULL;
    wl_tko_t *tko = NULL;
    whd_buffer_t buffer;
    wl_tko_enable_t *tko_enable = NULL;
    whd_driver_t whd_driver;
    whd_result_t result;
    CHECK_IFP_NULL(ifp);

    whd_driver = ifp->whd_driver;

    len = (int)(WHD_PAYLOAD_MTU - strlen(IOVAR_STR_TKO) - 1);
    data = (uint8_t * )whd_proto_get_iovar_buffer(whd_driver, &buffer, (uint16_t)len, IOVAR_STR_TKO);
    CHECK_IOCTL_BUFFER(data);

    tko = (wl_tko_t *)data;

    tko->subcmd_id = WL_TKO_SUBCMD_ENABLE;
    tko->len = TKO_DATA_OFFSET;

    tko_enable = (wl_tko_enable_t *)tko->data;
    tko_enable->enable = enable;

    tko->len += sizeof(wl_tko_enable_t);

    tko->subcmd_id = htod16(tko->subcmd_id);
    tko->len = htod16(tko->len);

    /* invoke SET iovar */
    result = whd_proto_set_iovar(ifp, buffer, NULL);
    if (result != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR( ("%s: tko %s FAILED\n", __func__, (enable == WHD_TRUE ? "enable" : "disable") ) );
        return result;
    }
    else
    {
        WPRINT_WHD_ERROR( ("%s: Successfully %s\n", __func__, (enable == WHD_TRUE ? "enabled" : "disabled") ) );
    }
    return result;
}

static whd_result_t
whd_tko_autoenab(whd_interface_t ifp, whd_bool_t enable)
{
    uint32_t len = 0;
    uint8_t *data = NULL;
    wl_tko_t *tko = NULL;
    whd_buffer_t buffer;
    wl_tko_autoenab_t *tko_autoenab = NULL;
    whd_driver_t whd_driver;
    whd_result_t result;

    CHECK_IFP_NULL(ifp);

    whd_driver = ifp->whd_driver;

    len = (int)(WHD_PAYLOAD_MTU - strlen(IOVAR_STR_TKO) - 1);
    data = (uint8_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer, (uint16_t)len, IOVAR_STR_TKO);
    CHECK_IOCTL_BUFFER(data);

    tko = (wl_tko_t *)data;

    tko->subcmd_id = WL_TKO_SUBCMD_AUTOENAB;
    tko->len = TKO_DATA_OFFSET;

    tko_autoenab = (wl_tko_autoenab_t *)tko->data;
	tko_autoenab->enable = enable;
	tko_autoenab->version = WL_TKO_AUTO_VER;
    tko_autoenab->length = REMAINING_LEN(tko_autoenab, wl_tko_autoenab_t, length);
    tko->len += sizeof(wl_tko_autoenab_t);

    tko->subcmd_id = htod16(tko->subcmd_id);
    tko->len = htod16(tko->len);

    /* invoke SET iovar */
    result = whd_proto_set_iovar(ifp, buffer, NULL);
    if (result != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR(("%s: tko autoenab %s FAILED\n", __func__, (enable == WHD_TRUE ? "enable" : "disable")));
    }
    else
    {
        WPRINT_WHD_INFO(("%s: Successfully %s tko autoenab\n", __func__, (enable == WHD_TRUE ? "enabled" : "disabled")));
    }

    return result;
}

whd_result_t
whd_tko_filter(whd_interface_t ifp, whd_tko_auto_filter_t * whd_filter, uint8_t filter_flag)
{
   uint32_t len = 0;
   uint8_t *data = NULL;
   wl_tko_t *tko = NULL;
   whd_buffer_t buffer;
   wl_tko_filter_t *wl_filter = NULL;
   whd_result_t result = WHD_SUCCESS;
   whd_driver_t whd_driver;
   CHECK_IFP_NULL(ifp);

   whd_driver = ifp->whd_driver;
   CHECK_DRIVER_NULL(whd_driver);

   len = (int)(WHD_PAYLOAD_MTU - strlen(IOVAR_STR_TKO) - 1);
   data = (uint8_t * )whd_proto_get_iovar_buffer(whd_driver, &buffer, (uint16_t)len, IOVAR_STR_TKO);
   CHECK_IOCTL_BUFFER(data);

    if (data == NULL)
    {
        WPRINT_WHD_ERROR( ("%s: Failed to get iovar buf\n", __func__) );
        return WHD_IOCTL_FAIL;
    }

	tko = (wl_tko_t *)data;
	tko->subcmd_id = WL_TKO_SUBCMD_FILTER;
	tko->len = TKO_DATA_OFFSET;
	wl_filter=(wl_tko_filter_t*)tko->data;
	tko->len += sizeof(wl_tko_filter_t);

	tko->subcmd_id = htod16(tko->subcmd_id);
	tko->len = htod16(tko->len);

	/* SET parameters */
	wl_filter->version = WL_TKO_AUTO_VER;
	wl_filter->length = REMAINING_LEN(wl_filter, wl_tko_autoenab_t, length);
	if(filter_flag & TKO_FILTER_SRC_PORT)
		wl_filter->sport = whd_filter->sport;

    if(filter_flag & TKO_FILTER_DST_PORT)
		wl_filter->dport =whd_filter->dport;

	if(filter_flag & TKO_FILTER_SRC_IP)
		memcpy(wl_filter->ip_src,whd_filter->ip_src,IPV6_ADDR_LEN);

	if(filter_flag & TKO_FILTER_DST_IP)
		memcpy(wl_filter->ip_dst,whd_filter->ip_dst,IPV6_ADDR_LEN);

    result=whd_proto_set_iovar(ifp,buffer,NULL);
		if (result != WHD_SUCCESS)
        {
            WPRINT_WHD_ERROR( ("%s: Cannot set filter\n", __func__) );
        }

	return result;
}

whd_result_t
whd_get_wowl_cap(whd_interface_t ifp, uint32_t *value)
{
    whd_result_t ret;
    ret = whd_wifi_get_iovar_value(ifp, IOVAR_STR_WOWL, value );
    WPRINT_WHD_DEBUG( ("%s : wowl %lx\n",__func__ ,*value));
    return ret;
}

whd_result_t
whd_set_wowl_cap(whd_interface_t ifp, uint32_t value)
{
    WPRINT_WHD_DEBUG( ("%s : wowl %lx\n",__func__ , (unsigned long)value));
    return whd_wifi_set_iovar_value(ifp, IOVAR_STR_WOWL, value);
}

whd_result_t
whd_wowl_clear(whd_interface_t ifp)
{
    uint32_t value = 0;
    WPRINT_WHD_ERROR( ("%s :  %lx\n", __func__, (unsigned long)value));
    return whd_wifi_set_iovar_value(ifp, IOVAR_STR_WOWL_CLEAR, value);
}

whd_result_t
whd_wowl_activate(whd_interface_t ifp, uint32_t value)
{
    WPRINT_WHD_ERROR( ("%s :  %lx\n", __func__, (unsigned long)value));
    return whd_wifi_set_iovar_value(ifp, IOVAR_STR_WOWL_ACTIVATE, value);
}

whd_result_t
whd_set_wowl_pattern(whd_interface_t ifp, char* opt, uint32_t offset, uint8_t mask_size,
		     uint8_t * mask, uint8_t pattern_size, uint8_t * pattern, uint8_t type)
{
    whd_buffer_t buffer;
    whd_driver_t whd_driver = ifp->whd_driver;
    char* data;
    wl_wowl_pattern_t *wl_pattern;

    if (strcmp(opt, "add") != 0 &&
        strcmp(opt, "del") != 0 &&
        strcmp(opt, "clr") != 0) {
        WPRINT_WHD_ERROR( ("%s : operation not add, del, cl  \n",__func__ ));
        return WHD_BADARG;
    }
    WPRINT_WHD_DEBUG(("%s : %s, offset %lu, pattern %s  len %d %d\n",__func__ , opt, (unsigned long)offset,
                     pattern, mask_size, pattern_size));
    data = (char*)whd_proto_get_iovar_buffer(whd_driver, &buffer,
            strlen(opt) + 1 + sizeof(wl_wowl_pattern_t) + mask_size + pattern_size , IOVAR_STR_WOWL_PATTERN);

    if(data == NULL)
    {
         WPRINT_WHD_ERROR( ("%s : %d \n",__FUNCTION__ , __LINE__));
         return WHD_BUFFER_ALLOC_FAIL;
    }

    strncpy(data, opt, strlen(opt));
    data += strlen(opt) + 1;

    wl_pattern = (wl_wowl_pattern_t *)data;

    if (strcmp(opt, "clr") != 0)
    {
        wl_pattern->offset = offset;
        wl_pattern->masksize = mask_size;
        wl_pattern->patternsize = pattern_size;
        wl_pattern->patternoffset = sizeof(wl_wowl_pattern_t) + mask_size;
        wl_pattern->id = 0;
        wl_pattern->reasonsize = 0x00;
        wl_pattern->type = type;

        data = (char*)(data + sizeof(wl_wowl_pattern_t));
        memcpy(data, (const char *)mask, mask_size);

        data = (char*)(data + mask_size);
        memcpy(data, (const char *)pattern, pattern_size);
    }

    return whd_proto_set_iovar(ifp, buffer, NULL);
}

whd_result_t
whd_get_wowl_pattern(whd_interface_t ifp,uint32_t pattern_num,  wl_wowl_pattern_t * pattern)
{
    whd_result_t ret;
    whd_buffer_t buffer;
    whd_buffer_t response;
    whd_driver_t whd_driver = ifp->whd_driver;
    uint32_t i, cnt = 0;
    wl_wowl_pattern_list_t* list;
    wl_wowl_pattern_t *wl_pattern;
    uint8_t *ptr;
    uint32_t tot;

    whd_proto_get_iovar_buffer(whd_driver, &buffer, WHD_PAYLOAD_MTU, IOVAR_STR_WOWL_PATTERN);
    ret = whd_proto_get_iovar(ifp, buffer, &response);
    if (ret != WHD_SUCCESS)
    {
        whd_buffer_release(whd_driver, response, WHD_NETWORK_RX);
        return WHD_WLAN_ERROR;
    }

    list = (wl_wowl_pattern_list_t*) whd_buffer_get_current_piece_data_pointer(whd_driver, response);
    ptr = (uint8_t *)list->pattern;
    cnt = (pattern_num < list->count ? pattern_num : list->count);
    tot = 0;
    for (i = 0; i < cnt; i++) {
        wl_pattern = (wl_wowl_pattern_t *)ptr + tot;
        tot += (wl_pattern->masksize + wl_pattern->patternsize + sizeof(wl_wowl_pattern_t));
    }
    memcpy(pattern, list->pattern, tot);
    ret = whd_buffer_release(whd_driver, response, WHD_NETWORK_RX);
    return ret;
}

whd_result_t
whd_wowl_activate_secure(whd_interface_t ifp, tls_param_info_t *tlsparam)
{
    whd_result_t ret;
    whd_buffer_t buffer;
    whd_driver_t whd_driver = ifp->whd_driver;
    uint8_t *data;
    data = (uint8_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer,
           sizeof(tls_param_info_t), IOVAR_STR_WOWL_ACTIVATE_SECURE);
    memcpy(data, tlsparam, sizeof(tls_param_info_t));
    ret = whd_proto_set_iovar(ifp, buffer, NULL);
    if (ret == WHD_SUCCESS)
    {
        CHECK_RETURN(whd_wowl_activate(ifp, 1));
    }
    else
    {
        WPRINT_WHD_ERROR( ("whd_wowl_activate_secure failed and error - %ld\n", (unsigned long)ret));
    }
    return ret;
}

whd_result_t
whd_wowl_get_secure_session_status(whd_interface_t ifp, secure_sess_info_t *tls_sess_info)
{
    whd_result_t ret;
    whd_buffer_t buffer;
    whd_buffer_t response;
    whd_driver_t whd_driver = ifp->whd_driver;
    uint8_t *data = NULL;
    struct secure_sess_info *sess_info;
    data = (uint8_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer, sizeof(struct secure_sess_info),
	  IOVAR_STR_WOWL_SEC_SESS_INFO);
    CHECK_IOCTL_BUFFER(data);
    CHECK_RETURN(whd_proto_get_iovar(ifp, buffer, &response));
    sess_info = (secure_sess_info_t*) whd_buffer_get_current_piece_data_pointer(whd_driver, response);
    whd_mem_memcpy(tls_sess_info, sess_info, sizeof(struct secure_sess_info));
    ret = whd_buffer_release(whd_driver, response, WHD_NETWORK_RX);
    return ret;
}

#ifdef CYCFG_ULP_SUPPORT_ENABLED
whd_result_t
whd_wifi_get_deepsleep_stats(whd_driver_t whd_driver, char *buf, uint32_t buflen)
{
    whd_result_t ret;
    whd_interface_t ifp;
    char *param = "ulpstats";
    char *trunc = NULL;

    if(!CHECK_BUFLEN(buflen, MAX_DUMP_BUF_LEN, MIN_DUMP_BUF_LEN))
    {
        WPRINT_WHD_ERROR( ("Invalid buffer length for ulp statistics") );
        return WHD_BADARG;
    }

    memset(buf, 0, buflen);

    /* Getting ulpstats is supported only in Station mode, default interface mode is STA(0) */
    ifp = whd_driver->iflist[CY_WCM_INTERFACE_TYPE_STA];

    ret = whd_wifi_get_iovar_buffer_with_param(ifp, IOVAR_STR_DUMP, param, (uint32_t)strlen(param), (uint8_t*)buf, buflen);

    if(ret == WHD_SUCCESS)
    {
        trunc = strstr(buf, "DS2 Counters");
        if (trunc != NULL)
        {
            memset(trunc, '\0', strlen(trunc) + 1);
            buflen = (uint32_t)strlen(buf);
        }
        WPRINT_WHD_INFO( ("ULP statistics: \n%s\n", buf));
        whd_driver->ds_cb_info.callback(whd_driver->ds_cb_info.ctx, buf, buflen);
    }
    else
    {
        WPRINT_WHD_ERROR( ("Failed to get ULP statistics, error code: %lu", (unsigned long)ret) );
    }

    return ret;
}

whd_result_t
whd_wifi_register_ds_callback(whd_interface_t ifp, whd_ds_callback_t callback, void *ctx, char *buf, uint32_t buflen)
{
    whd_driver_t whd_driver;

    if(ifp == NULL)
    {
        return WHD_UNKNOWN_INTERFACE;
    }

    whd_driver = ifp->whd_driver;

    whd_driver->ds_cb_info.callback = callback;
    whd_driver->ds_cb_info.ctx = ctx;
    whd_driver->ds_cb_info.buf = buf;
    whd_driver->ds_cb_info.buflen = buflen;

    return WHD_SUCCESS;
}

whd_result_t
whd_wifi_deregister_ds_callback(whd_interface_t ifp, whd_ds_callback_t callback)
{
    whd_driver_t whd_driver;

    if(ifp == NULL)
    {
        return WHD_UNKNOWN_INTERFACE;
    }

    whd_driver = ifp->whd_driver;

    memset(&whd_driver->ds_cb_info, 0, sizeof(whd_driver->ds_cb_info));

    return WHD_SUCCESS;
}
#endif
whd_result_t whd_wifi_icmp_echo_req_cmd_handler(whd_interface_t ifp, wl_icmp_echo_req_cmd_type_t cmd_type,
                                                         whd_icmp_echo_req_info_t *whd_peer_info)
{
    wl_icmp_echo_req_cmd_t *icmp_echo_req_iovar;
    whd_buffer_t buffer;
    uint16_t data_length, iovar_buf_length;
    uint8_t *data_addr;

    CHECK_IFP_NULL(ifp);
    CHECK_DRIVER_NULL(ifp->whd_driver);

    switch(cmd_type)
    {
        case WL_ICMP_ECHO_REQ_ENAB:
            data_length = sizeof(icmp_echo_req_enable);
            data_addr = &icmp_echo_req_enable;
            break;

        case WL_ICMP_ECHO_REQ_ADD:
            data_length = sizeof(icmp_peer_config);
            data_addr = (uint8_t *) &icmp_peer_config;
            break;

        case WL_ICMP_ECHO_REQ_DEL:
        case WL_ICMP_ECHO_REQ_START:
        case WL_ICMP_ECHO_REQ_STOP:
        case WL_ICMP_ECHO_REQ_INFO:
            data_length = sizeof(icmp_peer_ip);
            data_addr = (uint8_t *) &icmp_peer_ip;
            break;

        default:
            return WHD_BADARG;
    }

    if (cmd_type != WL_ICMP_ECHO_REQ_INFO)
    {
        iovar_buf_length = sizeof(wl_icmp_echo_req_cmd_t) + data_length;
    }
    else
    {
        iovar_buf_length = sizeof(wl_icmp_echo_req_cmd_t) + sizeof(wl_icmp_echo_req_get_peer_info_t);
    }
    icmp_echo_req_iovar = (wl_icmp_echo_req_cmd_t *)whd_proto_get_iovar_buffer(ifp->whd_driver, &buffer, iovar_buf_length,
                                                                               IOVAR_STR_ICMP_ECHO_REQ);
    CHECK_IOCTL_BUFFER(icmp_echo_req_iovar);
    icmp_echo_req_iovar->version = WL_ICMP_ECHO_REQ_VER;
    icmp_echo_req_iovar->length = iovar_buf_length;
    icmp_echo_req_iovar->cmd_type = (uint8_t) cmd_type;
    whd_mem_memcpy(icmp_echo_req_iovar->data, data_addr, data_length);

    if (cmd_type != WL_ICMP_ECHO_REQ_INFO)
    {
        CHECK_RETURN(whd_proto_set_iovar(ifp, buffer, NULL));
    }
    else
    {
        whd_buffer_t response;
        wl_icmp_echo_req_get_info_t *iovar_get_info;
        wl_icmp_echo_req_get_peer_info_t *iovar_get_peer_info;
        if (whd_proto_get_iovar(ifp, buffer, &response) != WHD_SUCCESS)
        {
            WPRINT_WHD_ERROR( ("%s: get iovar failed\n", __func__) );
            return WHD_WLAN_ERROR;
        }
        iovar_get_info = (wl_icmp_echo_req_get_info_t *) whd_buffer_get_current_piece_data_pointer(ifp->whd_driver, response);
        whd_peer_info->enable = iovar_get_info->enable;
        whd_peer_info->count = iovar_get_info->count;
        iovar_get_peer_info = (wl_icmp_echo_req_get_peer_info_t *) iovar_get_info->data;
        whd_peer_info->state = iovar_get_peer_info->state;
        whd_peer_info->periodicity = iovar_get_peer_info->config.periodicity;
        whd_peer_info->duration = iovar_get_peer_info->config.duration;
        whd_peer_info->ip_ver = iovar_get_peer_info->config.ip_ver;
        if (whd_peer_info->ip_ver == WHD_IPV4)
        {
            whd_mem_memcpy(whd_peer_info->u.ipv4, iovar_get_peer_info->config.u.ipv4.addr, IPV4_ADDR_LEN);
        }
        else
        {
            whd_mem_memcpy(whd_peer_info->u.ipv6, iovar_get_peer_info->config.u.ipv6.addr, IPV6_ADDR_LEN);
        }
        whd_mem_memcpy(whd_peer_info->mac_addr, iovar_get_peer_info->config.mac_addr, sizeof(whd_mac_t));
        CHECK_RETURN(whd_buffer_release(ifp->whd_driver, response, WHD_NETWORK_RX) );
    }

    return WHD_SUCCESS;
}

whd_result_t whd_wifi_icmp_echo_req_enable(whd_interface_t ifp, whd_bool_t Enable)
{
    icmp_echo_req_enable = (uint8_t) Enable;

    return whd_wifi_icmp_echo_req_cmd_handler(ifp, WL_ICMP_ECHO_REQ_ENAB, NULL);
}

whd_result_t whd_wifi_icmp_echo_req_add(whd_interface_t ifp, whd_ip_ver_t ip_ver, uint8_t *peer_ip, whd_mac_t *peer_mac,
                                               uint32_t periodicity, uint32_t duration)
{
    if (peer_ip == NULL)
    {
        return WHD_BADARG;
    }

    icmp_peer_config.ip_ver = ip_ver;
    memset(&icmp_peer_config.u, 0, IPV6_ADDR_LEN);
    if (icmp_peer_config.ip_ver == WHD_IPV4)
    {
        whd_mem_memcpy(&icmp_peer_config.u.ipv4.addr, peer_ip, IPV4_ADDR_LEN);
    }
    else if (icmp_peer_config.ip_ver == WHD_IPV6)
    {
        whd_mem_memcpy(&icmp_peer_config.u.ipv6.addr, peer_ip, IPV6_ADDR_LEN);
    }
    else
    {
        return WHD_BADARG;
    }
    whd_mem_memcpy(&icmp_peer_config.mac_addr, peer_mac, sizeof(whd_mac_t));
    icmp_peer_config.periodicity = periodicity;
    icmp_peer_config.duration = duration;

    return whd_wifi_icmp_echo_req_cmd_handler(ifp, WL_ICMP_ECHO_REQ_ADD, NULL);
}

whd_result_t whd_wifi_icmp_echo_req_del(whd_interface_t ifp, whd_ip_ver_t ip_ver, uint8_t *peer_ip)
{
    if (peer_ip == NULL)
    {
        return WHD_BADARG;
    }

    icmp_peer_ip.ip_ver = ip_ver;
    memset(&icmp_peer_ip.u, 0, IPV6_ADDR_LEN);
    if (icmp_peer_ip.ip_ver == WHD_IPV4)
    {
        whd_mem_memcpy(&icmp_peer_ip.u.ipv4.addr, peer_ip, IPV4_ADDR_LEN);
    }
    else if (icmp_peer_ip.ip_ver == WHD_IPV6)
    {
        whd_mem_memcpy(&icmp_peer_ip.u.ipv6.addr, peer_ip, IPV6_ADDR_LEN);
    }
    else
    {
        return WHD_BADARG;
    }

    return whd_wifi_icmp_echo_req_cmd_handler(ifp, WL_ICMP_ECHO_REQ_DEL, NULL);
}

whd_result_t whd_wifi_icmp_echo_req_start(whd_interface_t ifp, whd_ip_ver_t ip_ver, uint8_t *peer_ip)
{
    if (peer_ip == NULL)
    {
        return WHD_BADARG;
    }

    icmp_peer_ip.ip_ver = ip_ver;
    memset(&icmp_peer_ip.u, 0, IPV6_ADDR_LEN);
    if (icmp_peer_ip.ip_ver == WHD_IPV4)
    {
        whd_mem_memcpy(&icmp_peer_ip.u.ipv4.addr, peer_ip, IPV4_ADDR_LEN);
    }
    else if (icmp_peer_ip.ip_ver == WHD_IPV6)
    {
        whd_mem_memcpy(&icmp_peer_ip.u.ipv6.addr, peer_ip, IPV6_ADDR_LEN);
    }
    else
    {
        return WHD_BADARG;
    }

    return whd_wifi_icmp_echo_req_cmd_handler(ifp, WL_ICMP_ECHO_REQ_START, NULL);
}

whd_result_t whd_wifi_icmp_echo_req_stop(whd_interface_t ifp, whd_ip_ver_t ip_ver, uint8_t *peer_ip)
{
    if (peer_ip == NULL)
    {
        return WHD_BADARG;
    }

    icmp_peer_ip.ip_ver = ip_ver;
    memset(&icmp_peer_ip.u, 0, IPV6_ADDR_LEN);
    if (icmp_peer_ip.ip_ver == WHD_IPV4)
    {
        whd_mem_memcpy(&icmp_peer_ip.u.ipv4.addr, peer_ip, IPV4_ADDR_LEN);
    }
    else if (icmp_peer_ip.ip_ver == WHD_IPV6)
    {
        whd_mem_memcpy(&icmp_peer_ip.u.ipv6.addr, peer_ip, IPV6_ADDR_LEN);
    }
    else
    {
        return WHD_BADARG;
    }

    return whd_wifi_icmp_echo_req_cmd_handler(ifp, WL_ICMP_ECHO_REQ_STOP, NULL);
}

whd_result_t whd_wifi_icmp_echo_req_get_info(whd_interface_t ifp, whd_ip_ver_t ip_ver, uint8_t *peer_ip,
                                                whd_icmp_echo_req_info_t *whd_peer_info)
{
    if ((peer_ip == NULL) || (whd_peer_info == NULL))
    {
        return WHD_BADARG;
    }

    icmp_peer_ip.ip_ver = ip_ver;
    memset(&icmp_peer_ip.u, 0, IPV6_ADDR_LEN);
    if (icmp_peer_ip.ip_ver == WHD_IPV4)
    {
        whd_mem_memcpy(&icmp_peer_ip.u.ipv4.addr, peer_ip, IPV4_ADDR_LEN);
    }
    else if (icmp_peer_ip.ip_ver == WHD_IPV6)
    {
        whd_mem_memcpy(&icmp_peer_ip.u.ipv6.addr, peer_ip, IPV6_ADDR_LEN);
    }
    else
    {
        return WHD_BADARG;
    }

    return whd_wifi_icmp_echo_req_cmd_handler(ifp, WL_ICMP_ECHO_REQ_INFO, whd_peer_info);
}

/** Handle icmp echo req events
 *
 *  This is called when the WLC_E_ICMP_ECHO_REQ event is received, and parses the reason and peer's ip to the user application.
 *
 * @param event_header     : The event details
 * @param event_data       : The data for the event which contains the scan result structure
 * @param handler_user_data: data which will be passed to user application
 *
 * @returns : handler_user_data parameter
 */
static void *whd_wifi_icmp_echo_req_events_handler(whd_interface_t ifp, const whd_event_header_t *event_header,
                                                             const uint8_t *event_data,
                                                             void *handler_user_data)
{
    whd_icmp_echo_req_event_data_t whd_event_data;
    const wl_icmp_echo_req_event_t *wl_event_data = (const wl_icmp_echo_req_event_t *) event_data;
    whd_driver_t whd_driver = ifp->whd_driver;

    if (whd_driver->internal_info.icmp_echo_req_callback == NULL)
    {
        WPRINT_WHD_ERROR( ("No set callback function in %s at %d \n", __func__, __LINE__) );
        return handler_user_data;
    }

    if ((event_header->event_type == WLC_E_ICMP_ECHO_REQ) && (wl_event_data->version == WL_ICMP_ECHO_REQ_EVENT_VER))
    {
        whd_event_data.reason = (IcmpEchoReq_event_reason_e) wl_event_data->reason;
        whd_event_data.echo_req_cnt = wl_event_data->echo_req_cnt;
        whd_event_data.ip_ver = wl_event_data->ip_ver;
        memset(&whd_event_data.u, 0, IPV6_ADDR_LEN);
        if(whd_event_data.ip_ver == WHD_IPV4)
        {
            memcpy(whd_event_data.u.ipv4, wl_event_data->u.ipv4.addr, IPV4_ADDR_LEN);
        }
        else
        {
            memcpy(whd_event_data.u.ipv6, wl_event_data->u.ipv6.addr, IPV6_ADDR_LEN);
        }
        whd_driver->internal_info.icmp_echo_req_callback(&whd_event_data);
    }

    return handler_user_data;
}

whd_result_t whd_wifi_icmp_echo_req_register_callback(whd_interface_t ifp, whd_icmp_echo_req_callback_t callback,
                                                                 whd_bool_t Register)
{
    CHECK_IFP_NULL(ifp);
    whd_driver_t whd_driver = ifp->whd_driver;
    uint16_t event_entry = 0xFF;

    if (Register)
    {
        whd_assert("Bad args", callback != NULL);
        if (ifp->event_reg_list[WHD_ICMP_ECHO_REQ_EVENT_ENTRY] != WHD_EVENT_NOT_REGISTERED)
        {
            whd_wifi_deregister_event_handler(ifp, ifp->event_reg_list[WHD_ICMP_ECHO_REQ_EVENT_ENTRY]);
            ifp->event_reg_list[WHD_ICMP_ECHO_REQ_EVENT_ENTRY] = WHD_EVENT_NOT_REGISTERED;
        }
        CHECK_RETURN(whd_management_set_event_handler(ifp, icmp_echo_req_events,
                                                      whd_wifi_icmp_echo_req_events_handler, NULL, &event_entry));

        if (event_entry >= WHD_MAX_EVENT_SUBSCRIPTION)
        {
            WPRINT_WHD_ERROR( ("ICMP_ECHO_REQ events registration failed in function %s and line %d", __func__, __LINE__) );
            return WHD_UNFINISHED;
        }
        ifp->event_reg_list[WHD_ICMP_ECHO_REQ_EVENT_ENTRY] = event_entry;

        whd_driver->internal_info.icmp_echo_req_callback = callback;
    }
	else
	{
        whd_driver->internal_info.icmp_echo_req_callback = NULL;
        whd_wifi_deregister_event_handler(ifp, ifp->event_reg_list[WHD_ICMP_ECHO_REQ_EVENT_ENTRY]);
        ifp->event_reg_list[WHD_ICMP_ECHO_REQ_EVENT_ENTRY] = WHD_EVENT_NOT_REGISTERED;
    }
    return WHD_SUCCESS;
}
