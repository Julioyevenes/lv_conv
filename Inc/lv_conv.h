/**
  ******************************************************************************
  * @file    ${file_name} 
  * @author  ${user}
  * @version 
  * @date    ${date}
  * @brief   
  ******************************************************************************
  * @attention
  *
  * This program is free software: you can redistribute it and/or modify
  * it under the terms of the GNU General Public License as published by
  * the Free Software Foundation, either version 3 of the License, or
  * (at your option) any later version.
  *
  * This program is distributed in the hope that it will be useful,
  * but WITHOUT ANY WARRANTY; without even the implied warranty of
  * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  * GNU General Public License for more details.
  *
  * You should have received a copy of the GNU General Public License
  * along with this program.  If not, see <http://www.gnu.org/licenses/>.
  *
  ******************************************************************************
  */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __LV_CONV_H
#define __LV_CONV_H

#ifdef __cplusplus
 extern "C" {
#endif 

/* Includes ------------------------------------------------------------------*/
#include "lvgl/lvgl.h"

/* Exported types ------------------------------------------------------------*/
typedef enum
{
  LV_CONV_MAIN_UI = 0,
  LV_CONV_MAIN_RUN
} lv_conv_main_state_t;
   
/* Exported constants --------------------------------------------------------*/
/* Exported macro ------------------------------------------------------------*/
/* Exported variables --------------------------------------------------------*/
extern lv_conv_main_state_t conv_main_state;

/* Exported functions --------------------------------------------------------*/
void lv_conv_init(void);
void lv_conv_run(void);

#ifdef __cplusplus
}
#endif

#endif /* __LV_CONV_H */
