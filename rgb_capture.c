#include "g_config.h"
#include "rgb_capture.h"
#include "pio_programs.h"
#include "v_buf.h"

#include "hardware/clocks.h"
#include "../../hardware_dma/include/hardware/dma.h"
#include "hardware/irq.h"
#include "../../hardware_sync/include/hardware/sync.h"
#include "hardware/structs/pll.h"
#include "hardware/structs/systick.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

// LEDs
#define PIN_LED (25u)

static int dma_ch0;
static int dma_ch1;
uint8_t *cap_buf;
settings_t capture_settings;
uint16_t offset;

uint32_t frame_count = 0;

static uint32_t cap_dma_buf[2][CAP_DMA_BUF_SIZE / 4];
#define DMA_ADD 8//(-8)//20
static uint32_t cap_dma_line_buf[2][(V_BUF_W + DMA_ADD) / 4 + 1];
static uint32_t *cap_dma_buf_addr[2];

void check_settings(settings_t *settings)
{

  if (settings->video_out_mode > VIDEO_OUT_MODE_MAX)
    settings->video_out_mode = VIDEO_OUT_MODE_MAX;
  // else if (settings->video_out_mode < VIDEO_OUT_MODE_MIN)
  //   settings->video_out_mode = VIDEO_OUT_MODE_MIN;

  if (settings->cap_sync_mode > CAP_SYNC_MODE_MAX)
    settings->cap_sync_mode = CAP_SYNC_MODE_MAX;
  // else if (settings->cap_sync_mode < CAP_SYNC_MODE_MIN)
  //   settings->cap_sync_mode = CAP_SYNC_MODE_MIN;

  if (settings->frequency > FREQUENCY_MAX)
    settings->frequency = FREQUENCY_MAX;
  else if (settings->frequency < FREQUENCY_MIN)
    settings->frequency = FREQUENCY_MIN;

  if (settings->ext_clk_divider > EXT_CLK_DIVIDER_MAX)
    settings->ext_clk_divider = EXT_CLK_DIVIDER_MAX;
  else if (settings->ext_clk_divider < EXT_CLK_DIVIDER_MIN)
    settings->ext_clk_divider = EXT_CLK_DIVIDER_MIN;

  if (settings->delay > DELAY_MAX)
    settings->delay = DELAY_MAX;
  else if (settings->delay < DELAY_MIN)
    settings->delay = DELAY_MIN;

  if (settings->shX > shX_MAX)
    settings->shX = shX_MAX;
  else if (settings->shX < shX_MIN)
    settings->shX = shX_MIN;

  if (settings->shY > shY_MAX)
    settings->shY = shY_MAX;
  else if (settings->shY < shY_MIN)
    settings->shY = shY_MIN;

  if (settings->pin_inversion_mask & ~PIN_INVERSION_MASK)
    settings->pin_inversion_mask = PIN_INVERSION_MASK;
}

void set_capture_settings(settings_t *settings)
{
  memcpy(&capture_settings, settings, sizeof(settings_t));
  check_settings(&capture_settings);
}

int16_t set_capture_shX(int16_t shX)
{
  if (shX > shX_MAX)
    capture_settings.shX = shX_MAX;
  else if (shX < shX_MIN)
    capture_settings.shX = shX_MIN;
  else
    capture_settings.shX = shX;

  return capture_settings.shX;
}

int16_t set_capture_shY(int16_t shY)
{
  if (shY > shY_MAX)
    capture_settings.shY = shY_MAX;
  else if (shY < shY_MIN)
    capture_settings.shY = shY_MIN;
  else
    capture_settings.shY = shY;

  return capture_settings.shY;
}

int16_t set_capture_len_VS(int16_t len_VS)
{
  if (len_VS > len_VS_MAX)
    capture_settings.len_VS = len_VS_MAX;
  else if (len_VS < len_VS_MIN)
    capture_settings.len_VS = len_VS_MIN;
  else
    capture_settings.len_VS = len_VS;

  return capture_settings.len_VS;
}

int8_t set_capture_delay(int8_t delay)
{
  if (delay > DELAY_MAX)
    capture_settings.delay = DELAY_MAX;
  else if (delay < DELAY_MIN)
    capture_settings.delay = DELAY_MIN;
  else
    capture_settings.delay = delay;
  if (capture_settings.cap_sync_mode == EXT2)
    PIO_CAP->instr_mem[offset + pio_capture_2_offset_delay] = nop_opcode | (capture_settings.delay << 8);
  else if (capture_settings.cap_sync_mode == EXT)
    PIO_CAP->instr_mem[offset + pio_capture_1_offset_delay] = nop_opcode | (capture_settings.delay << 8);

  return capture_settings.delay;
}

void set_video_sync_mode(bool video_sync_mode)
{
  capture_settings.video_sync_mode = video_sync_mode;
}

inline int min(int a, int b)
{
  return a > b ? b : a;
}

uint16_t line_no = 0;
extern bool _80DS;
extern settings_t settings_arr[2];

static int dma_size=0;
static volatile uint16_t len_hist[26] = {};

void __not_in_flash_func(dma_handler_capture2())
{
  static uint32_t dma_buf_idx;
  pio_sm_put(PIO_CAP, SM_CAP, dma_size - 2);
  int nShXadd = _80DS ? -44 : 80;
  int shX = capture_settings.shX + nShXadd;
  pio_sm_put(PIO_CAP, SM_CAP, shX);
  pio_sm_put(PIO_CAP, SM_CAP, line_no == 2 ? capture_settings.shY * 640 : 0);

  dma_hw->ints1 = 1u << dma_ch1;
  dma_channel_set_read_addr(dma_ch1, &cap_dma_buf_addr[dma_buf_idx & 1], false);

  if (cap_buf == NULL)
    cap_buf = get_v_buf_in();

  const uint32_t *buf32 = cap_dma_buf_addr[dma_buf_idx++ & 1];

  uint8_t len = ~*(uint8_t *)buf32;
  buf32 = (uint32_t *)(((uint8_t *)buf32) + 1);

  uint8_t *cap_buf8 = cap_buf + (V_BUF_H - line_no) * V_BUF_W / 2;

  uint32_t nShift = _80DS ? 0 : 60;
  if (!_80DS)
  {
    uint8_t c = *((uint8_t *)buf32) & 0xF;
    c |= c << 4;
    for (uint32_t k = nShift / 4; k--;)
      *cap_buf8++ = *cap_buf8++ = c;
  }
  //set_80_ds();
  len_hist[len/10]++;
  for (uint32_t k = (dma_size /* - nShift  - shX */) / 4; k--;)
  {
    uint32_t val32 = (*buf32++) & 0x0f0f0f0f;
    uint32_t v = (val32 | (val32 >> 4));
    *cap_buf8++ = v;         // & 0xFF;
    *cap_buf8++ = (v >> 16); // & 0xFF;
  }

  if (--line_no == 0)
  {
    
    line_no = V_BUF_H;
    uint16_t max_val = 0;
    uint8_t max_idx = 0;
    for (uint8_t i=7; i<26; i++)
    {
      if (len_hist[i] > max_val)
      {
        max_idx = i;
        max_val = len_hist[i];
      }
      len_hist[i] = 0;
    }
    if (_80DS)
    {
      if (max_idx >= 10 && max_idx <= 11)
      {
        _80DS = false;
        set_capture_settings(&settings_arr[_80DS]);
      }
    }
    else
    {
      if (max_idx >= 16 && max_idx <= 18)
      {
        _80DS = true;
        set_capture_settings(&settings_arr[_80DS]);
      }
    }
    cap_buf8 = cap_buf = get_v_buf_in();

    frame_count++;
    gpio_put(PIN_LED, frame_count & 0x20);
  }
}

void __not_in_flash_func(dma_handler_capture())
{
  static uint32_t dma_buf_idx;
  static uint8_t pix8_s;
  static int x_s;
  static int y_s;
  uint16_t len_VS_pix = capture_settings.len_VS;

  uint8_t sync_mask = capture_settings.video_sync_mode ? ((1u << VS_PIN) | (1u << HS_PIN)) : (1u << HS_PIN);

  dma_hw->ints1 = 1u << dma_ch1;
  dma_channel_set_read_addr(dma_ch1, &cap_dma_buf_addr[dma_buf_idx & 1], false);

  if (cap_buf == NULL)
    cap_buf = get_v_buf_in();
  int shX = shX_MAX - capture_settings.shX;
  int shY = capture_settings.shY;

  uint8_t *buf8 = (uint8_t *)cap_dma_buf[dma_buf_idx & 1];
  dma_buf_idx++;
  gpio_put(PIN_LED, frame_count & 0x20);

  register uint8_t pix8 = pix8_s;
  register int x = x_s;
  register int y = y_s;

  static uint8_t *cap_buf8_s = g_v_buf;
  uint8_t *cap_buf8 = cap_buf8_s;

  static uint CS_idx_s = 0;
  uint CS_idx = CS_idx_s;

  const uint8_t HS_MASK = 1u << HS_PIN;
  const uint8_t VS_MASK = 1u << VS_PIN;
  const uint8_t VHS_MASK = VS_MASK | HS_MASK;
  const uint32_t sync32_mask = (((uint32_t)VHS_MASK) << 24) | (((uint32_t)VHS_MASK) << 16) | (((uint32_t)VHS_MASK) << 8) | ((uint32_t)VHS_MASK);
  for (uint32_t k = CAP_DMA_BUF_SIZE; k--;)
  {
#if 0
    if (((uint32_t)buf8 & 3u) == 0 && k > 3)
    {
      if (y<0 || y>=V_BUF_H)
      {
        while (k>3)
        {
          uint32_t val32 = *(uint32_t*)buf8;
          if ((val32 & sync32_mask) != sync32_mask)
            break;
          x += 4;
          buf8 += 4;
          k -= 4;
        }
      }
      else
      {
        uint32_t val32 = *(uint32_t*)buf8;
        if (x<-3  && (val32 & sync32_mask) == sync32_mask)
        {
          uint32_t nPos = -x;
          if (nPos > k)
            nPos = k;
          nPos &= ~3;
          buf8 += nPos;
          x += nPos;
          k -= nPos;
        }

        uint32_t n_rep = V_BUF_W - x;
        if (n_rep > k)
          n_rep = k;
        n_rep >>= 2;
        uint32_t i;
        for (i = 0; i < n_rep; ++i)
        {
          uint32_t val32 = *(uint32_t*)buf8;
          if ((val32 & sync32_mask) != sync32_mask)
          {
            //x_s = x;
            break;
          }
          //goto sync;
          //x += 4;
          buf8 += 4;
          //k -= 4;
          //if (x >= V_BUF_W)
          //  break;
          val32 = val32 & 0x0f0f0f0f;
          uint32_t v = (val32 | (val32 >> 4));
          *cap_buf8++ = v;// & 0xFF;
          *cap_buf8++ = (v >> 16);// & 0xFF;
        }
        x += 4 * i;
        k -= 4 * i;

        while (k>3)
        {
          uint32_t val32 = *(uint32_t*)buf8;
          if ((val32 & sync32_mask) != sync32_mask)
            break;
          x += 4;
          buf8 += 4;
          k -= 4;
        }
      }
    }
    //else
    //  x = x | 1;
//sync:
#endif
    uint8_t val8 = *buf8++;

    x++;

    if ((val8 & sync_mask) != sync_mask) // detect active sync pulses
    {
      if ((val8 & HS_MASK) == 0 && CS_idx == H_SYNC_PULSE / 2) // start in the middle of the H_SYNC pulse // this should help ignore the spikes
      {
        y++;
        x = (-shX - 1) | 1u;
        // set the pointer to the beginning of a new line
        if ((y >= 0) && (cap_buf != NULL))
          cap_buf8 = &(((uint8_t *)cap_buf)[y * V_BUF_W / 4]);
      }

      CS_idx++;
      if (sync_mask == HS_MASK) // composite sync
      {
        if (CS_idx < V_SYNC_PULSE) // detect V_SYNC pulse
          continue;
      }
      else
      {
        if (val8 & VS_MASK)
          continue;
      }

      // start capture of a new frame
      if (y >= 0 /* && CS_idx == len_VS_pix */)
      {
        if (frame_count > 10) // power on delay // noise immunity at the sync input
          cap_buf = get_v_buf_in();

        frame_count++;
        y = -shY - 1;
      }

      continue;
    }

    if (x & 1)
    {
      // if (cap_buf == NULL)
      //   continue;

      if ((x < 0) || (y < 0))
        continue;

      if ((x >= V_BUF_W / 2) || (y >= V_BUF_H))
        continue;

      *cap_buf8++ = (pix8 & 0xf) | (val8 << 4);
    }
    else
    {
      CS_idx = 0;
      pix8 = val8;
    }
  }

  x_s = x;
  y_s = y;
  pix8_s = pix8;
  cap_buf8_s = cap_buf8;
  CS_idx_s = CS_idx;
}

void calculate_clkdiv(float freq, uint16_t *div_int, uint8_t *div_frac)
{
  uint8_t div_frac_2;

  float clock = clock_get_hz(clk_sys);
  float div = clock / (freq * 12.0);

  pio_calculate_clkdiv_from_float(div, div_int, div_frac);

  float delta_freq = (clock / ((*div_int + (float)*div_frac / 256) * 12.0)) - freq;

  if (delta_freq > 0)
    div_frac_2 = *div_frac + 1;
  else if (delta_freq < 0)
    div_frac_2 = *div_frac - 1;
  else
    return;

  float delta_freq_2 = (clock / ((*div_int + (float)div_frac_2 / 256) * 12.0)) - freq;

  if (abs(delta_freq_2) < abs(delta_freq))
    *div_frac = div_frac_2;

  return;
}

static uint16_t wrap = 0;
static uint16_t wrap_target = 0;
const pio_program_t *program = NULL;

inline uint32_t* dec_byte_ptr(uint32_t* ptr)
{
  return (uint32_t*)(((uint8_t*)ptr)-1);
}

void start_capture(settings_t *settings)
{
  set_capture_settings(settings);

  uint8_t inv_mask = capture_settings.pin_inversion_mask;

  // pinMode(PIN_LED, OUTPUT);
  gpio_init(PIN_LED);
  gpio_set_dir(PIN_LED, GPIO_OUT);
  // digitalWrite(PIN_LED, LOW);
  gpio_put(PIN_LED, 0);

  // set capture pins
  for (int i = CAP_PIN_D0; i < CAP_PIN_D0 + 7; i++)
  {
    gpio_init(i);
    gpio_set_dir(i, GPIO_IN);
    gpio_set_input_hysteresis_enabled(i, true);

    if (inv_mask & 1)
      gpio_set_inover(i, GPIO_OVERRIDE_INVERT);

    inv_mask >>= 1;
  }

  // PIO initialization
  uint16_t pio_capture_1_program_instructions2[pio_capture_1_program.length];
  memcpy(pio_capture_1_program_instructions2, pio_capture_1_program_instructions, sizeof(pio_capture_1_program_instructions));
  uint16_t pio_capture_0_program_instructions2[pio_capture_0_program.length];
  memcpy(pio_capture_0_program_instructions2, pio_capture_0_program_instructions, sizeof(pio_capture_0_program_instructions));
  uint16_t pio_capture_2_program_instructions2[pio_capture_2_program.length];
  memcpy(pio_capture_2_program_instructions2, pio_capture_2_program_instructions, sizeof(pio_capture_2_program_instructions));
  switch (capture_settings.cap_sync_mode)
  {
  case SELF:
  {
    // set initial capture delay
    pio_capture_0_program_instructions2[pio_capture_0_offset_delay] = nop_opcode | ((capture_settings.delay & 0b00011111) << 8);
    // load PIO program
    struct pio_program pio_capture_0_program2 = pio_capture_0_program;
    pio_capture_0_program2.instructions = pio_capture_0_program_instructions2;
    offset = pio_add_program(PIO_CAP, program = &pio_capture_0_program2);
    // set capture delay = 0
    pio_capture_0_program_instructions2[pio_capture_0_offset_delay] = nop_opcode;

    wrap = offset + pio_capture_0_wrap;
    wrap_target = offset + pio_capture_0_wrap_target;
    break;
  }

  case EXT:
  {
    // set initial capture delay
    pio_capture_1_program_instructions2[pio_capture_1_offset_delay] = nop_opcode | ((capture_settings.delay & 0b00011111) << 8);
    // set clock divider
    pio_capture_1_program_instructions2[pio_capture_1_offset_divider1] = set_opcode | ((capture_settings.ext_clk_divider - 1) & 0b00011111);
    pio_capture_1_program_instructions2[pio_capture_1_offset_divider2] = set_opcode | ((capture_settings.ext_clk_divider - 1) & 0b00011111);
    // load PIO program
    struct pio_program pio_capture_1_program2 = pio_capture_1_program;
    pio_capture_1_program2.instructions = pio_capture_1_program_instructions2;
    offset = pio_add_program(PIO_CAP, program = &pio_capture_1_program2);
    // set capture delay = 0
    pio_capture_1_program_instructions2[pio_capture_1_offset_delay] = nop_opcode;

    wrap = offset + pio_capture_1_wrap;
    wrap_target = offset + pio_capture_1_wrap_target;

    break;
  }

  case EXT2:
  {
    // set initial capture delay
    pio_capture_2_program_instructions2[pio_capture_2_offset_delay] = nop_opcode | ((capture_settings.delay & 0b00011111) << 8);

    // load PIO program
    struct pio_program pio_capture_2_program2 = pio_capture_2_program;
    pio_capture_2_program2.instructions = pio_capture_2_program_instructions2;
    offset = pio_add_program(PIO_CAP, program = &pio_capture_2_program2);
    // set capture delay = 0
    pio_capture_2_program_instructions2[pio_capture_2_offset_delay] = nop_opcode;

    wrap = offset + pio_capture_2_wrap;
    wrap_target = offset + pio_capture_2_wrap_target;

    break;
  }
  default:
    break;
  }

  pio_sm_config c = pio_get_default_sm_config();
  sm_config_set_wrap(&c, wrap_target, wrap);

  sm_config_set_in_shift(&c, false, false, 8); // autopush not needed
  sm_config_set_in_pins(&c, CAP_PIN_D0);
  sm_config_set_jmp_pin(&c, HS_PIN);
  bool bEXT2 = capture_settings.cap_sync_mode == EXT2;
  if (!bEXT2)
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);

  if (capture_settings.cap_sync_mode == SELF)
  {
    uint16_t div_int;
    uint8_t div_frac;

    calculate_clkdiv(capture_settings.frequency, &div_int, &div_frac);
    sm_config_set_clkdiv_int_frac(&c, div_int, div_frac);
  }

  pio_sm_init(PIO_CAP, SM_CAP, offset, &c);
  pio_sm_set_enabled(PIO_CAP, SM_CAP, true);
  if (bEXT2)
  {
    pio_sm_put_blocking(PIO_CAP, SM_CAP, capture_settings.shY);

    pio_sm_put_blocking(PIO_CAP, SM_CAP, V_BUF_W - 1);
    pio_sm_put_blocking(PIO_CAP, SM_CAP, capture_settings.shX);
    pio_sm_put(PIO_CAP, SM_CAP, 0);
  }
  // DMA initialization
  dma_ch0 = dma_claim_unused_channel(true);
  dma_ch1 = dma_claim_unused_channel(true);

  // main (data) DMA channel
  dma_channel_config c0 = dma_channel_get_default_config(dma_ch0);

  channel_config_set_transfer_data_size(&c0, DMA_SIZE_8);
  channel_config_set_read_increment(&c0, false);
  channel_config_set_write_increment(&c0, true);
  channel_config_set_dreq(&c0, DREQ_PIO_CAP + SM_CAP);
  channel_config_set_chain_to(&c0, dma_ch1);

  if (bEXT2)
  {
    cap_dma_buf_addr[0] = dec_byte_ptr(&cap_dma_line_buf[0][1]);
    cap_dma_buf_addr[1] = dec_byte_ptr(&cap_dma_line_buf[1][1]);
  }
  else
  {
    cap_dma_buf_addr[0] = &cap_dma_buf[0][0];
    cap_dma_buf_addr[1] = &cap_dma_buf[1][0];
  }

  dma_size = bEXT2 ? CAP_DMA_LINE_BUF_SIZE + 1 : CAP_DMA_BUF_SIZE;
  //if (!_80DS)
    dma_size += DMA_ADD;

  //if (!_80DS)
  //  ui32CAP_DMA_BUF_SIZE -= 120;
  dma_channel_configure(
      dma_ch0,
      &c0,
      cap_dma_buf_addr[0],   // write address
      &PIO_CAP->rxf[SM_CAP], // read address
      dma_size,  //
      false                  // don't start yet
  );

  // control DMA channel
  dma_channel_config c1 = dma_channel_get_default_config(dma_ch1);

  channel_config_set_transfer_data_size(&c1, DMA_SIZE_32);
  channel_config_set_read_increment(&c1, false);
  channel_config_set_write_increment(&c1, false);
  channel_config_set_chain_to(&c1, dma_ch0); // chain to other channel

  dma_channel_configure(
      dma_ch1,
      &c1,
      &dma_hw->ch[dma_ch0].write_addr, // write address
      &cap_dma_buf_addr[0],            // read address
      1,                               //
      false                            // don't start yet
  );

  dma_channel_set_irq1_enabled(dma_ch1, true);

  // configure the processor to run dma_handler() when DMA IRQ 1 is asserted
  line_no = V_BUF_H;
  irq_set_exclusive_handler(DMA_IRQ_1, bEXT2 ? dma_handler_capture2 : dma_handler_capture);
  irq_set_enabled(DMA_IRQ_1, true);

  dma_start_channel_mask((1u << dma_ch0));
}

void stop_capture()
{
  pio_sm_set_enabled(PIO_CAP, SM_CAP, false);
  pio_sm_init(PIO_CAP, SM_CAP, offset, NULL);
  pio_remove_program(PIO_CAP, program, offset);
  dma_channel_set_irq1_enabled(dma_ch1, false);
  dma_channel_abort(dma_ch0);
  dma_channel_abort(dma_ch1);
  dma_channel_cleanup(dma_ch0);
  dma_channel_cleanup(dma_ch1);
  dma_channel_unclaim(dma_ch0);
  dma_channel_unclaim(dma_ch1);
}
