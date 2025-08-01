/***************************************************************************//**
* \file cyhal_hw_types_template.h
*
* \brief
* Provides a template for configuration resources used by the HAL. Items
* here need to be implemented for each HAL port. It is up to the environment
* being ported into what the actual types are. There are some suggestions below
* but these are not required. All that is required is that the type is defined;
* it does not matter to the HAL what type is actually chosen for the
* implementation
* All TODOs and references to 'PORT' need to be replaced by with meaningful
* values for the device being supported.
*
********************************************************************************
* \copyright
* Copyright 2018-2019 Cypress Semiconductor Corporation
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
*******************************************************************************/

/**
 * \addtogroup group_hal_hw_types PORT Hardware Types
 * \ingroup group_hal_PORT
 * \{
 * Struct definitions for configuration resources in the PORT.
 *
 * \defgroup group_hal_hw_types_data_structures Data Structures
 */

#pragma once

/*
 #include "TODO: Port specific header file"
 */

#ifdef WHD_CUSTOM_HAL

#ifdef __cplusplus
extern "C" {
#endif

#ifdef IMXRT
#include "fsl_sdio.h"
#endif

/**
 * \addtogroup group_hal_hw_types_data_structures
 * \{
 */

/** GPIO object */
typedef uint32_t /* TODO: port specific type */ cyhal_gpio_t;

/** Clock divider object */
typedef struct
{
    /* TODO: replace with port specific items */
    void *div_type;
} cyhal_clock_divider_t;

/** SDIO object */
typedef struct
{
    /* TODO: replace with port specific items */
#if defined(WHD_CUSTOM_HAL) && defined(IMXRT)
	sdio_card_t sd_card;
#else
	void *empty;
#endif /* WHD_CUSTOM_HAL && IMXRT */
} cyhal_sdio_t;

/** SPI object */
typedef struct
{
    /* TODO: replace with port specific items */
    void *empty;
} cyhal_spi_t;

/** M2M/DMA object */
typedef struct
{
    /* TODO: replace with port specific items */
    void *empty;
} cyhal_m2m_t;

/** \} group_hal_hw_types_data_structures */

#if defined(__cplusplus)
}
#endif /* __cplusplus */

/** \} group_hal_hw_types */

#endif /* WHD_CUSTOM_HAL */