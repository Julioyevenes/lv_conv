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

/* Includes ------------------------------------------------------------------*/
#include <unistd.h>
#include <SDL2/SDL.h>
#include "lvgl_port.h"
#include "lv_conv.h"

/* Private types -------------------------------------------------------------*/
/* Private constants ---------------------------------------------------------*/
/* Private macro -------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
/* Private function prototypes -----------------------------------------------*/
static int sdl_tick_thread(void * data);

int main(int argc, char **argv)
{
  /* Init lvgl */
  lv_init();
  
  /* Init sdl */
  lvgl_sdl_init();
  
  /* Init disp */
  lvgl_disp_init();

  /* Init indev */
  lvgl_indev_init();

  /* Tick init.
   * You have to call 'lv_tick_inc()' in periodically to inform LittelvGL about
   * how much time were elapsed Create an SDL thread to do this*/  
  SDL_CreateThread(sdl_tick_thread, "tick", NULL);

  /* Init app */
  lv_conv_init();
  
  /* Infinite loop */
  while (1)
  {
    switch (conv_main_state)
    {
      case LV_CONV_MAIN_UI:
        lvgl_run();        
        break;
        
      case LV_CONV_MAIN_RUN:        
        lvgl_run();
        lv_conv_run();
        break;        
    }
  }   
}

static int sdl_tick_thread(void * data)
{
  for(;;)
  {
    /* Sleep for 5 millisecond */
    SDL_Delay(5);
    
    /* Tell LittelvGL that 5 milliseconds were elapsed */
    lv_tick_inc(5);
  }
  
  return 0;
}
