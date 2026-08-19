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
#include <stdlib.h>
#include <SDL2/SDL.h>
#include <sysinfoapi.h>
#include "lvgl_port.h"
#include "lvgl/lvgl.h"

/* Private types -------------------------------------------------------------*/
typedef struct
{
  volatile bool sdl_refr_qry;
  uint32_t fb[LV_HOR_RES_MAX * LV_VER_RES_MAX];  
  
  SDL_Window * window;
  SDL_Renderer * renderer;
  SDL_Texture * texture;
} lv_sdl_disp_t;

typedef struct
{
  bool buttons[3];
  bool last_buttons[3];
  int16_t x;
  int16_t y;
} lv_sdl_mouse_t;

typedef struct
{
  volatile bool sdl_quit_qry;
  
  lv_sdl_disp_t disp;
  lv_sdl_mouse_t mouse;
} lv_sdl_dev_t;

/* Private constants ---------------------------------------------------------*/
/* Private macro -------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
lv_sdl_dev_t dev;

/* Private function prototypes -----------------------------------------------*/
static void 	lvgl_disp_write_cb(lv_disp_drv_t * disp_drv, const lv_area_t * area, lv_color_t * color_p);
static bool 	lvgl_mouse_read_cb(lv_indev_drv_t * indev, lv_indev_data_t * data);

static void   lvgl_sdl_event_task(lv_task_t * t);

static void   sdl_win_init(lv_sdl_disp_t * disp, const char * title, int w, int h);
static void   sdl_win_free(lv_sdl_disp_t * disp);
static void   sdl_win_update(lv_sdl_disp_t * disp);

int           sdl_quit_filter_cb(void * userdata, SDL_Event * event);

void lvgl_run(void)
{
  static uint32_t tickstart = 0;
  
  /* Call lv_task_handler() periodically every few milliseconds.
   * It will redraw the screen if required, handle input devices etc. */
  if((GetTickCount() - tickstart) > 5)
  {
    lv_task_handler();

    tickstart = GetTickCount();
  }  
}

void lvgl_sdl_init(void)
{
  lv_task_create(lvgl_sdl_event_task, 10, LV_TASK_PRIO_HIGH, &dev);
}

void lvgl_disp_init(void)
{
  lv_disp_drv_t disp_drv;
  
  /** 
    * Initialize your display
    */
  SDL_Init(SDL_INIT_VIDEO);
  SDL_SetEventFilter(sdl_quit_filter_cb, &dev);
  sdl_win_init(&dev.disp, "SDL Display", LV_HOR_RES_MAX, LV_VER_RES_MAX);

  /** 
    * Create a buffer for drawing
    */
  static lv_disp_buf_t disp_buf;
  static lv_color_t buf00[LV_HOR_RES_MAX * 48];                   /* A buffer for 48 rows */
  static lv_color_t buf01[LV_HOR_RES_MAX * 48];                   /* A buffer for 48 rows */
  lv_disp_buf_init(&disp_buf, buf00, buf01, LV_HOR_RES_MAX * 48); /* Initialize the display buffer */
    
  /** 
    * Register the display in LittlevGL
    */
  lv_disp_drv_init(&disp_drv);                                    /* Basic initialization */

  /* Used to copy the buffer's content to the display */
  disp_drv.flush_cb = lvgl_disp_write_cb;

  /* Set a display buffer */
  disp_drv.buffer = &disp_buf;

  /* Finally register the driver */
  lv_disp_drv_register(&disp_drv);    
}

void lvgl_indev_init(void)
{
  lv_indev_drv_t indev_drv;

  /**
    * Mouse
    */
  /* Initialize your mouse if you have */
  /* Nothing to do here */

  /* Register a mouse input device */
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = lvgl_mouse_read_cb;
  lv_indev_drv_register(&indev_drv);
}

static void lvgl_disp_write_cb(lv_disp_drv_t * disp_drv, const lv_area_t * area, lv_color_t * color_p)
{
  int32_t y;
  
	/* Truncate the area to the screen */
	int32_t act_x1 = area->x1 < 0 ? 0 : area->x1;
	int32_t act_y1 = area->y1 < 0 ? 0 : area->y1;
	int32_t act_x2 = area->x2 > disp_drv->hor_res - 1 ? disp_drv->hor_res - 1 : area->x2;
	int32_t act_y2 = area->y2 > disp_drv->ver_res - 1 ? disp_drv->ver_res - 1 : area->y2;
  int32_t act_w = act_x2 - act_x1 + 1;

  for(y = act_y1; y < act_y2 + 1; y++)
  {
    memcpy(&dev.disp.fb[y * disp_drv->hor_res + act_x1], color_p, act_w * sizeof(lv_color_t));
    color_p += act_w;
  }
  
  dev.disp.sdl_refr_qry = true;

  /* If it was the last part to refresh update the texture of the window */
  if(lv_disp_flush_is_last(disp_drv))
  {
    if(dev.disp.sdl_refr_qry != false)
    {
      dev.disp.sdl_refr_qry = false;
      sdl_win_update(&dev.disp);
    }
  }

  /* Inform the graphics library that you are ready with the flushing */
  lv_disp_flush_ready(disp_drv);
}

static bool lvgl_mouse_read_cb(lv_indev_drv_t * indev, lv_indev_data_t * data)
{
  lv_sdl_mouse_t * mouse = &(dev.mouse);

  /* Store the collected data */
  data->point.x = mouse->x;
  data->point.y = mouse->y;
  
  if(mouse->buttons[0])
  {
    data->state = LV_INDEV_STATE_PR;
  }
  else if(mouse->buttons[1])
  {
    data->state = LV_INDEV_STATE_RIGHT_PR;
  }
  else if(mouse->last_buttons[0])
  {
    mouse->last_buttons[0] = false;
    data->state = LV_INDEV_STATE_REL;
  }
  else if(mouse->last_buttons[1])
  {
    mouse->last_buttons[1] = false;
    data->state = LV_INDEV_STATE_RIGHT_REL;
  }

  for(uint8_t i = 0; i < 3; i++)
  {
    mouse->last_buttons[i] = mouse->buttons[i];
  }

  /* Return `false` because we are not buffering and no more data to read */
  return false;
}

static void lvgl_sdl_event_task(lv_task_t * t)
{
  SDL_Event event;
  lv_sdl_dev_t * dev = t->user_data;
  lv_sdl_disp_t * disp = &(dev->disp);
  lv_sdl_mouse_t * mouse = &(dev->mouse);
  
  while(SDL_PollEvent(&event))
  {
    switch(event.type)
    {
      case SDL_MOUSEBUTTONUP:
        if(event.button.button == SDL_BUTTON_LEFT)
        {
          mouse->buttons[0] = false;
        }
        
        if(event.button.button == SDL_BUTTON_RIGHT)
        {
          mouse->buttons[1] = false;
        }        
        break;
        
      case SDL_MOUSEBUTTONDOWN:
        if(event.button.button == SDL_BUTTON_LEFT)
        {
          mouse->buttons[0] = true;
        }
        
        if(event.button.button == SDL_BUTTON_RIGHT)
        {
          mouse->buttons[1] = true;
        }        
        break;

      case SDL_MOUSEMOTION:
        mouse->x = event.motion.x;
        mouse->y = event.motion.y;
        break;        
    }
    
    if(event.type == SDL_WINDOWEVENT)
    {
      switch(event.window.event)
      {
        case SDL_WINDOWEVENT_TAKE_FOCUS:
        case SDL_WINDOWEVENT_EXPOSED:
          sdl_win_update(disp);
          break;
        
        default:
          break;
      }
    }
  }
  
  /* Run until quit event not arrives */
  if(dev->sdl_quit_qry)
  {
    sdl_win_free(disp);
    exit(0);
  }
}

static void sdl_win_init(lv_sdl_disp_t * disp, const char * title, int w, int h)
{
  disp->window = SDL_CreateWindow(title,
                                  SDL_WINDOWPOS_UNDEFINED,
                                  SDL_WINDOWPOS_UNDEFINED,
                                  w,
                                  h,
                                  0);
  disp->renderer = SDL_CreateRenderer(disp->window, -1, SDL_RENDERER_SOFTWARE);
  disp->texture = SDL_CreateTexture(disp->renderer,
                                    SDL_PIXELFORMAT_ARGB8888,
                                    SDL_TEXTUREACCESS_STATIC,
                                    w,
                                    h);
  SDL_SetTextureBlendMode(disp->texture, SDL_BLENDMODE_BLEND);
  
  memset(disp->fb, 0, w * h * sizeof(uint32_t));
  
  disp->sdl_refr_qry = true;  
}

static void sdl_win_free(lv_sdl_disp_t * disp)
{
  SDL_DestroyTexture(disp->texture);
  SDL_DestroyRenderer(disp->renderer);
  SDL_DestroyWindow(disp->window);

  SDL_Quit();
}

static void sdl_win_update(lv_sdl_disp_t * disp)
{
  SDL_UpdateTexture(disp->texture, NULL, disp->fb, LV_HOR_RES_MAX * sizeof(uint32_t));
  SDL_RenderCopy(disp->renderer, disp->texture, NULL, NULL);
  SDL_RenderPresent(disp->renderer);
}

int sdl_quit_filter_cb(void * userdata, SDL_Event * event)
{
  lv_sdl_dev_t * dev = userdata;
  
  if(event->type == SDL_WINDOWEVENT)
  {
    if(event->window.event == SDL_WINDOWEVENT_CLOSE)
    {
      dev->sdl_quit_qry = true;
    }
  }
  else if(event->type == SDL_QUIT)
  {
    dev->sdl_quit_qry = true;
  }

  return 1;
}
