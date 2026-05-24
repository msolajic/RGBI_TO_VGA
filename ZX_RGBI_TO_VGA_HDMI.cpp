//#include <Arduino.h>
#include <typeinfo>

extern "C"
{
#include "g_config.h"
#include "dvi.h"
#include "rgb_capture.h"
#include "v_buf.h"
#include "vga.h"

}
#include "../../hardware_flash/include/hardware/flash.h"
#include <cstring>
#include <string>
#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "hardware/watchdog.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/flash.h"
#include "tusb.h"
//#include "Serial.h"
using String=std::string;

class SerialStdio
{
  size_t printNumber(unsigned long n, uint8_t base)
  {
    char buf[8 * sizeof(long) + 1]; // Assumes 8-bit chars plus zero byte.
    char *str = &buf[sizeof(buf) - 1];
  
    *str = '\0';
  
    // prevent crash if called with base == 1
    if (base < 2)
      base = 10;
  
    do
    {
      char c = n % base;
      n /= base;
  
      *--str = c < 10 ? c + '0' : c + 'A' - 10;
    } while (n);
  
    return write(str);
  }
  bool _ignoreFlowControl = true;
public:
  bool available()
  {
    return tud_cdc_available();
  }
  int read()
  {
    tud_task();
    if (tud_cdc_available())
    {
      return tud_cdc_read_char();
    }
    return -1;
  }
  size_t println(const char *s)
  {
    return print(s) + print("\r\n");
  }
  size_t print(const char * s)
  {
    return write((const uint8_t *)s, strlen(s));
  }
  size_t write(const uint8_t *buf, size_t length)
  {
    // CoreMutex m(&__usb_mutex, false);
    // if (!_running || !m)
    // {
    //   return 0;
    // }

    static uint64_t last_avail_time;
    int written = 0;
    if (tud_cdc_connected() || _ignoreFlowControl)
    {
      for (size_t i = 0; i < length;)
      {
        int n = length - i;
        int avail = tud_cdc_write_available();
        if (n > avail)
        {
          n = avail;
        }
        if (n)
        {
          int n2 = tud_cdc_write(buf + i, n);
          tud_task();
          tud_cdc_write_flush();
          i += n2;
          written += n2;
          last_avail_time = time_us_64();
        }
        else
        {
          tud_task();
          tud_cdc_write_flush();
          if (!tud_cdc_connected() ||
              (!tud_cdc_write_available() && time_us_64() > last_avail_time + 1'000'000 /* 1 second */))
          {
            break;
          }
        }
      }
    }
    else
    {
      // reset our timeout
      last_avail_time = 0;
    }
    tud_task();
    return written;
  }

  size_t write(const char * s)
  {
    return print(s);
  }
  size_t print(long n, int base)
  {
    if (base == 0)
    {
      return write(n);
    }
    else if (base == 10)
    {
      if (n < 0)
      {
        int t = print('-');
        n = -n;
        return printNumber(n, 10) + t;
      }
      return printNumber(n, 10);
    }
    else
    {
      return printNumber(n, base);
    }
  }
  size_t println(long n, int base)
  {
    return print(n, base) + print("\r\n");
  }
  size_t print(char c)
  {
    return write(c);
  }
  size_t write(char c)
  {
    return write((const uint8_t *)&c, 1);
  }
  size_t println(const String& s)
  {
    return println(s.c_str());
  } 
} serial;

settings_t settings_arr[2];
video_mode_t video_mode;

#define HEX 16
#define DEC 10

const int *saved_settings = (const int *)(XIP_BASE + (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE));
volatile bool start_core0 = false;
volatile bool bRestart = false;
volatile bool bStopCore1 = false;
volatile bool bCore1Stopped = false;
bool _80DS = true;


static void save_settings()
{
  serial.println("  Saving settings...");

  check_settings(&settings_arr[0]);
  check_settings(&settings_arr[1]);

  bStopCore1 = true;
  while (!bCore1Stopped) ;
  bStopCore1 = false;
  uint32_t ints = save_and_disable_interrupts();
  flash_range_erase((PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE), FLASH_SECTOR_SIZE);
  flash_range_program((PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE), (uint8_t *)&settings_arr[0], FLASH_PAGE_SIZE);
  restore_interrupts_from_disabled(ints);
  bCore1Stopped = false;
}

void print_byte_hex(uint8_t byte)
{
  if (byte < 16)
    serial.print("0");

  serial.print(byte, HEX);
}

String binary_to_string(uint8_t value, bool mask_1)
{
  uint8_t binary = value;
  String str = "";

  for (int i = 0; i < 8; i++)
  {
    str += binary & 0b10000000 ? (mask_1 ? "X" : "1") : "0";
    binary <<= 1;
  }

  return str;
}

void print_main_menu()
{
  serial.println("");
  serial.print("      * TIM011 RGBI to VGA ");
  serial.print(FW_VERSION);
  serial.println(" *");
  serial.println("");
  serial.println("  v   set video output mode");
  serial.println("  s   set scanlines mode");
  serial.println("  b   set buffering mode");
  serial.println("  c   set capture synchronization source");
  serial.println("  f   set capture frequency");
  serial.println("  d   set external clock divider");
  serial.println("  y   set video sync mode");
  serial.println("  t   set capture delay and image position");
  serial.println("  m   set pin inversion mask");
  serial.println("");
  serial.println("  p   show configuration");
  serial.println("  h   show help (this menu)");
  serial.println("  q   exit configuration mode");
  serial.println("  w   save configuration");
  serial.println("");
}

void print_video_out_menu()
{
  serial.println("");
  serial.println("      * Video output mode *");
  serial.println("");
  serial.println("  1   HDMI   640x480 (div 2)");
  serial.println("  2   VGA    640x480 (div 2)");
  serial.println("  3   VGA    800x600 (div 2)");
  serial.println("  4   VGA   1024x768 (div 3)");
  serial.println("  5   VGA  1280x1024 (div 3)");
  serial.println("  6   VGA  1280x1024 (div 4)");
  serial.println("  7   VGA  1280x1024 (div 2h4v)");
  serial.println("");
  serial.println("  p   show configuration");
  serial.println("  h   show help (this menu)");
  serial.println("  q   exit to main menu");
  serial.println("");
}

void print_scanlines_mode_menu()
{
  serial.println("");
  serial.println("      * Scanlines mode *");
  serial.println("");
  serial.println("  s   change scanlines mode");
  serial.println("");
  serial.println("  p   show configuration");
  serial.println("  h   show help (this menu)");
  serial.println("  q   exit to main menu");
  serial.println("");
}

void print_buffering_mode_menu()
{
  serial.println("");
  serial.println("      * Buffering mode *");
  serial.println("");
  serial.println("  b   change buffering mode");
  serial.println("");
  serial.println("  p   show configuration");
  serial.println("  h   show help (this menu)");
  serial.println("  q   exit to main menu");
  serial.println("");
}

void print_cap_sync_mode_menu()
{
  serial.println("");
  serial.println("      * Capture synchronization source *");
  serial.println("");
  serial.println("  1   self-synchronizing");
  serial.println("  2   external clock");
  serial.println("");
  serial.println("  p   show configuration");
  serial.println("  h   show help (this menu)");
  serial.println("  q   exit to main menu");
  serial.println("");
}

void print_capture_frequency_menu()
{
  serial.println("");
  serial.println("      * Capture frequency *");
  serial.println("");
  serial.println("  1   7000000 Hz (ZX Spectrum  48K)");
  serial.println("  2   7093790 Hz (ZX Spectrum 128K)");
  serial.println("  3   custom");
  serial.println("");
  serial.println("  p   show configuration");
  serial.println("  h   show help (this menu)");
  serial.println("  q   exit to main menu");
  serial.println("");
}

void print_ext_clk_divider_menu()
{
  serial.println("");
  serial.println("      * External clock divider *");
  serial.println("");
  serial.println("  a   increment divider (+1)");
  serial.println("  z   decrement divider (-1)");
  serial.println("");
  serial.println("  p   show configuration");
  serial.println("  h   show help (this menu)");
  serial.println("  q   exit to main menu");
  serial.println("");
}

void print_len_VS_menu()
{
  serial.println("");
  serial.println("      * Vertical sync length (pix) *");
  serial.println("");
  serial.println("  a   increment length (+1)");
  serial.println("  z   decrement length (-1)");
  serial.println("");
  serial.println("  p   show configuration");
  serial.println("  h   show help (this menu)");
  serial.println("  q   exit to main menu");
  serial.println("");
}

void print_video_sync_mode_menu()
{
  serial.println("");
  serial.println("      * Video synchronization mode *");
  serial.println("");
  serial.println("  y   change synchronization mode");
  serial.println("");
  serial.println("  p   show configuration");
  serial.println("  h   show help (this menu)");
  serial.println("  q   exit to main menu");
  serial.println("");
}

void print_image_tuning_menu()
{
  serial.println("");
  serial.println("      * Capture delay and image position *");
  serial.println("");
  serial.println("  a   increment delay (+1)");
  serial.println("  z   decrement delay (-1)");
  serial.println("");
  serial.println("  i   shift image UP");
  serial.println("  k   shift image DOWN");
  serial.println("  j   shift image LEFT");
  serial.println("  l   shift image RIGHT");
  serial.println("");
  serial.println("  p   show configuration");
  serial.println("  h   show help (this menu)");
  serial.println("  q   exit to main menu");
  serial.println("");
}

void print_pin_inversion_mask_menu()
{
  serial.println("");
  serial.println("      * Pin inversion mask *");
  serial.println("");
  serial.println("  m   set pin inversion mask");
  serial.println("");
  serial.println("  p   show configuration");
  serial.println("  h   show help (this menu)");
  serial.println("  q   exit to main menu");
  serial.println("");
}

void print_test_menu()
{
  serial.println("");
  serial.println("      * Test *");
  serial.println("");
  serial.println("  1   draw welcome image (vertical stripes)");
  serial.println("  2   draw welcome image (horizontal stripes)");
  serial.println("  i   show captured frame count");
  serial.println("");
  serial.println("  p   show configuration");
  serial.println("  h   show help (this menu)");
  serial.println("  q   exit to main menu");
  serial.println("");
}

void print_video_out_mode()
{
  settings_t& settings = settings_arr[_80DS];
  serial.print("  Video output mode ........... ");
  switch (settings.video_out_mode)
  {
  case DVI:
    serial.println("HDMI 640x480");
    break;

  case VGA640x480:
    serial.println("VGA 640x480");
    break;

  case VGA800x600:
    serial.println("VGA 800x600");
    break;

  case VGA1024x768:
    serial.println("VGA 1024x768");
    break;

  case VGA1280x1024_d3:
    serial.println("VGA 1280x1024 (div 3)");
    break;

  case VGA1280x1024_d4:
    serial.println("VGA 1280x1024 (div 4)");
    break;

  case VGA1280x1024_d24:
    serial.println("VGA 1280x1024 (div 2h4v)");
    break;

  default:
    break;
  }
}

void print_scanlines_mode()
{
  settings_t& settings = settings_arr[_80DS];
  serial.print("  Scanlines ................... ");

  if (settings.scanlines_mode)
    serial.println("enabled");
  else
    serial.println("disabled");
}

void print_buffering_mode()
{
  settings_t& settings = settings_arr[_80DS];
  serial.print("  Buffering mode .............. ");

  if (settings.x3_buffering_mode)
    serial.println("x3");
  else
    serial.println("x1");
}

void print_cap_sync_mode()
{
  settings_t& settings = settings_arr[_80DS];
  serial.print("  Capture sync source ......... ");
  switch (settings.cap_sync_mode)
  {
  case SELF:
    serial.println("self-synchronizing");
    break;

  case EXT:
    serial.println("external clock");
    break;
  case EXT2:
    serial.println("external clock2");
    break;
  
  default:
    break;
  }
}

void print_capture_frequency()
{
  settings_t& settings = settings_arr[_80DS];
  serial.print("  Capture frequency ........... ");
  serial.print(settings.frequency, DEC);
  serial.println(" Hz");
}

void print_ext_clk_divider()
{
  settings_t& settings = settings_arr[_80DS];
  serial.print("  External clock divider ...... ");
  serial.println(settings.ext_clk_divider, DEC);
}

void print_len_VS()
{
  settings_t& settings = settings_arr[_80DS];
  serial.print("  Vertical sync (pix)...... ");
  serial.println(settings.len_VS, DEC);
}


void print_capture_delay()
{
  settings_t& settings = settings_arr[_80DS];
  serial.print("  Capture delay ext clock...... ");
  serial.println(settings.delay, DEC);
}

void print_x_offset()
{
  settings_t& settings = settings_arr[_80DS];
  serial.print("  X offset .................... ");
  serial.println(settings.shX, DEC);
}

void print_y_offset()
{
  settings_t& settings = settings_arr[_80DS];
  serial.print("  Y offset .................... ");
  serial.println(settings.shY, DEC);
}

void print_dividers()
{
  uint16_t div_int;
  uint8_t div_frac;

  settings_t& settings = settings_arr[_80DS];
  video_mode_t video_mode = *(vga_modes[settings.video_out_mode]);

  serial.println("");

  serial.print("  System clock frequency ...... ");
  serial.print(clock_get_hz(clk_sys), 1);
  serial.println(" Hz");

  serial.println("  Capture divider");

  serial.print("    calculated (SDK) .......... ");

  pio_calculate_clkdiv_from_float((float)clock_get_hz(clk_sys) / (settings.frequency * 12.0), &div_int, &div_frac);

  serial.print((div_int + (float)div_frac / 256), 8);

  serial.print(" ( ");
  serial.print("0x");
  print_byte_hex((uint8_t)(div_int >> 8));
  print_byte_hex((uint8_t)(div_int & 0xff));
  print_byte_hex(div_frac);
  serial.println(" )");

  serial.print("    optimized ................. ");

  calculate_clkdiv(settings.frequency, &div_int, &div_frac);

  serial.print((div_int + (float)div_frac / 256), 8);

  serial.print(" ( ");
  serial.print("0x");
  print_byte_hex((uint8_t)(div_int >> 8));
  print_byte_hex((uint8_t)(div_int & 0xff));
  print_byte_hex(div_frac);
  serial.println(" )");

  serial.print("  Video output clock divider .. ");

  pio_calculate_clkdiv_from_float(((float)clock_get_hz(clk_sys) * video_mode.div) / video_mode.pixel_freq, &div_int, &div_frac);

  serial.print((div_int + (float)div_frac / 256), 8);

  serial.print(" ( ");
  serial.print("0x");
  print_byte_hex((uint8_t)(div_int >> 8));
  print_byte_hex((uint8_t)(div_int & 0xff));
  print_byte_hex(div_frac);
  serial.println(" )");

  serial.println("");
}

void print_video_sync_mode()
{
  settings_t& settings = settings_arr[_80DS];
  serial.print("  Video synchronization mode .. ");
  if (settings.video_sync_mode)
    serial.println("separate");
  else
    serial.println("composite");
}

void print_pin_inversion_mask()
{
  settings_t& settings = settings_arr[_80DS];
  serial.print("  Pin inversion mask .......... ");
  serial.println(binary_to_string(settings.pin_inversion_mask, false));
}

void print_settings()
{
  serial.println("");
  print_video_out_mode();
  print_scanlines_mode();
  print_buffering_mode();
  print_cap_sync_mode();
  print_capture_frequency();
  print_ext_clk_divider();
  print_video_sync_mode();
  print_capture_delay();
  print_x_offset();
  print_y_offset();
  print_pin_inversion_mask();
  print_len_VS();
  print_dividers();
  serial.println("");
}

void set_scanlines_mode()
{
  settings_t& settings = settings_arr[_80DS];
  if (settings.video_out_mode != DVI)
    set_vga_scanlines_mode(settings.scanlines_mode);
}

void process_menu(char &inbyte)
{
  settings_t& settings = settings_arr[_80DS];
  if (inbyte != 'h' && serial.available())
    inbyte = serial.read();

  switch (inbyte)
  {
  case 'p':
    print_settings();
    inbyte = 0;
    break;

  case 'v':
  {
    inbyte = 'h';

    while (1)
    {
      sleep_ms(10);

      if (inbyte != 'h' && serial.available())
        inbyte = serial.read();

      switch (inbyte)
      {
      case 'p':
        print_video_out_mode();
        break;

      case 'h':
        print_video_out_menu();
        break;

      case '1':
        settings.video_out_mode = DVI;
        print_video_out_mode();
        break;

      case '2':
        settings.video_out_mode = VGA640x480;
        print_video_out_mode();
        break;

      case '3':
        settings.video_out_mode = VGA800x600;
        print_video_out_mode();
        break;

      case '4':
        settings.video_out_mode = VGA1024x768;
        print_video_out_mode();
        break;

      case '5':
        settings.video_out_mode = VGA1280x1024_d3;
        print_video_out_mode();
        break;
      case '6':
        settings.video_out_mode = VGA1280x1024_d4;
        print_video_out_mode();
        break;
      case '7':
        settings.video_out_mode = VGA1280x1024_d24;
        print_video_out_mode();
        break;

      default:
        break;
      }

      if (inbyte == 'q')
      {
        inbyte = 'h';
        break;
      }

      inbyte = 0;
    }

    break;
  }

  case 's':
  {
    inbyte = 'h';

    while (1)
    {
      sleep_ms(10);

      if (inbyte != 'h' && serial.available())
        inbyte = serial.read();

      switch (inbyte)
      {
      case 'p':
        print_scanlines_mode();
        break;

      case 'h':
        print_scanlines_mode_menu();
        break;

      case 's':
        settings.scanlines_mode = !settings.scanlines_mode;
        print_scanlines_mode();
        set_scanlines_mode();
        break;

      default:
        break;
      }

      if (inbyte == 'q')
      {
        inbyte = 'h';
        break;
      }

      inbyte = 0;
    }

    break;
  }

  case 'b':
  {
    inbyte = 'h';

    while (1)
    {
      sleep_ms(10);

      if (inbyte != 'h' && serial.available())
        inbyte = serial.read();

      switch (inbyte)
      {
      case 'p':
        print_buffering_mode();
        break;

      case 'h':
        print_buffering_mode_menu();
        break;

      case 'b':
        settings.x3_buffering_mode = !settings.x3_buffering_mode;
        print_buffering_mode();
        set_v_buf_buffering_mode(settings.x3_buffering_mode);
        break;

      default:
        break;
      }

      if (inbyte == 'q')
      {
        inbyte = 'h';
        break;
      }

      inbyte = 0;
    }

    break;
  }

  case 'c':
  {
    inbyte = 'h';

    while (1)
    {
      sleep_ms(10);

      if (inbyte != 'h' && serial.available())
        inbyte = serial.read();

      switch (inbyte)
      {
      case 'p':
        print_cap_sync_mode();
        break;

      case 'h':
        print_cap_sync_mode_menu();
        break;

      case '1':
        settings.cap_sync_mode = SELF;
        print_cap_sync_mode();
        break;

      case '2':
        settings.cap_sync_mode = EXT;
        print_cap_sync_mode();
        break;

      default:
        break;
      }

      if (inbyte == 'q')
      {
        inbyte = 'h';
        break;
      }

      inbyte = 0;
    }

    break;
  }

  case 'f':
  {
    inbyte = 'h';

    while (1)
    {
      sleep_ms(10);

      if (inbyte != 'h' && serial.available())
        inbyte = serial.read();

      switch (inbyte)
      {
      case 'p':
        print_capture_frequency();
        break;

      case 'h':
        print_capture_frequency_menu();
        break;

      case '1':
        settings.frequency = 7000000;
        print_capture_frequency();
        break;

      case '2':
        settings.frequency = 7093790;
        print_capture_frequency();
        break;

      case '3':
      {
        String str_frequency = "";
        uint32_t frequency = 0;

        serial.print("  Enter frequency: ");

        while (1)
        {
          sleep_ms(10);
          inbyte = 0;

          if (serial.available())
            inbyte = serial.read();

          if (inbyte >= '0' && inbyte <= '9')
          {
            serial.print(inbyte);
            str_frequency += inbyte;
          }

          if (inbyte == '\r')
          {
            serial.println("");
            // frequency = str_frequency.toInt();

            if (frequency >= FREQUENCY_MIN && frequency <= FREQUENCY_MAX)
            {
              settings.frequency = frequency;
              print_capture_frequency();
              break;
            }
            else
            {
              str_frequency = "";
              serial.print("  Allowed frequency range ..... ");
              serial.print((uint32_t)FREQUENCY_MIN, DEC);
              serial.print(" - ");
              serial.print((uint32_t)FREQUENCY_MAX, DEC);
              serial.println(" Hz");
              serial.print("  Enter frequency: ");
            }
          }
        }

        break;
      }

      default:
        break;
      }

      if (inbyte == 'q')
      {
        inbyte = 'h';
        break;
      }

      inbyte = 0;
    }

    break;
  }

  case 'd':
  {
    inbyte = 'h';

    while (1)
    {
      sleep_ms(10);

      if (inbyte != 'h' && serial.available())
        inbyte = serial.read();

      switch (inbyte)
      {

      case 'p':
        print_ext_clk_divider();
        break;

      case 'h':
        print_ext_clk_divider_menu();
        break;

      case 'a':
        settings.ext_clk_divider = settings.ext_clk_divider < EXT_CLK_DIVIDER_MAX ? (settings.ext_clk_divider + 1) : EXT_CLK_DIVIDER_MAX;
        print_ext_clk_divider();
        break;

      case 'z':
        settings.ext_clk_divider = settings.ext_clk_divider > EXT_CLK_DIVIDER_MIN ? (settings.ext_clk_divider - 1) : EXT_CLK_DIVIDER_MIN;
        print_ext_clk_divider();
        break;

      default:
        break;
      }

      if (inbyte == 'q')
      {
        inbyte = 'h';
        break;
      }

      inbyte = 0;
    }

    break;
  }
  case 'l':
  {
    inbyte = 'h';

    while (1)
    {
      sleep_ms(10);

      if (inbyte != 'h' && serial.available())
        inbyte = serial.read();

      switch (inbyte)
      {

      case 'p':
        print_len_VS();
        break;

      case 'h':
        print_len_VS_menu();
        break;

      case 'a':
        settings.len_VS = set_capture_len_VS(settings.len_VS + 1);
        print_len_VS();
        break;

      case 'z':
        settings.len_VS = set_capture_len_VS(settings.len_VS - 1);
        print_len_VS();
        break;

      default:
        break;
      }

      if (inbyte == 'q')
      {
        inbyte = 'h';
        break;
      }

      inbyte = 0;
    }

    break;
  }

  case 'y':
  {
    inbyte = 'h';

    while (1)
    {
      sleep_ms(10);

      if (inbyte != 'h' && serial.available())
        inbyte = serial.read();

      switch (inbyte)
      {
      case 'p':
        print_video_sync_mode();
        break;

      case 'h':
        print_video_sync_mode_menu();
        break;

      case 'y':
        settings.video_sync_mode = !settings.video_sync_mode;
        print_video_sync_mode();
        set_video_sync_mode(settings.video_sync_mode);
        break;

      default:
        break;
      }

      if (inbyte == 'q')
      {
        inbyte = 'h';
        break;
      }

      inbyte = 0;
    }

    break;
  }

  case 't':
  {
    inbyte = 'h';

    while (1)
    {
      sleep_ms(10);

      if (inbyte != 'h' && serial.available())
        inbyte = serial.read();

      switch (inbyte)
      {

      case 'p':
        print_capture_delay();
        print_x_offset();
        print_y_offset();
        break;

      case 'h':
        print_image_tuning_menu();
        break;

      case 'a':
        settings.delay = set_capture_delay(settings.delay + 1);
        print_capture_delay();
        break;

      case 'z':
        settings.delay = set_capture_delay(settings.delay - 1);
        print_capture_delay();
        break;

      case 'i':
        settings.shY = set_capture_shY(settings.shY + 1);
        print_y_offset();
        break;

      case 'k':
        settings.shY = set_capture_shY(settings.shY - 1);
        print_y_offset();
        break;

      case 'j':
        settings.shX = set_capture_shX(settings.shX + 1);
        print_x_offset();
        break;

      case 'l':
        settings.shX = set_capture_shX(settings.shX - 1);
        print_x_offset();
        break;

      default:
        break;
      }

      if (inbyte == 'q')
      {
        inbyte = 'h';
        break;
      }

      inbyte = 0;
    }

    break;
  }

  case 'm':
  {
    inbyte = 'h';

    while (1)
    {
      sleep_ms(10);

      if (inbyte != 'h' && serial.available())
        inbyte = serial.read();

      switch (inbyte)
      {
      case 'p':
        print_pin_inversion_mask();
        break;

      case 'h':
        print_pin_inversion_mask_menu();
        break;

      case 'm':
      {
        String str_pin_inversion_mask = "";

        serial.print("  Enter pin inversion mask: ");

        while (1)
        {
          sleep_ms(10);
          inbyte = 0;

          if (serial.available())
            inbyte = serial.read();

          if (inbyte >= '0' && inbyte <= '1')
          {
            serial.print(inbyte);
            str_pin_inversion_mask += inbyte;
          }

          if (inbyte == '\r')
          {
            serial.println("");

            uint8_t pin_inversion_mask = 0;

            for (uint32_t i = 0; i < str_pin_inversion_mask.length(); i++)
            {
              pin_inversion_mask <<= 1;
              pin_inversion_mask |= str_pin_inversion_mask[i] == '1' ? 1 : 0;
            }

            if (!(pin_inversion_mask & ~PIN_INVERSION_MASK))
            {
              settings.pin_inversion_mask = pin_inversion_mask;
              print_pin_inversion_mask();
              break;
            }
            else
            {
              str_pin_inversion_mask = "";
              serial.print("  Allowed inversion mask ...... ");
              serial.println(binary_to_string(PIN_INVERSION_MASK, true));
              serial.print("  Enter pin inversion mask: ");
            }
          }
        }

        break;
      }

      default:
        break;
      }

      if (inbyte == 'q')
      {
        inbyte = 'h';
        break;
      }

      inbyte = 0;
    }

    break;
  }

  case 'T':
  {
    inbyte = 'h';

    while (1)
    {
      sleep_ms(10);

      if (inbyte != 'h' && serial.available())
        inbyte = serial.read();

      switch (inbyte)
      {
      case 'p':
        print_settings();
        break;

      case 'h':
        print_test_menu();
        break;

      case 'i':
        serial.print("  Current frame count ......... ");
        serial.println(frame_count - 1, DEC);
        break;

      case '1':
      case '2':
      {
        uint32_t frame_count_temp = frame_count;

        sleep_ms(100);

        if (frame_count - frame_count_temp == 0) // draw welcome screen only if capture is not active
        {
          serial.println("  Drawing the welcome screen...");

          if (inbyte == '1')
            draw_welcome_screen(*(vga_modes[settings.video_out_mode]));
          else
            draw_welcome_screen_h(*(vga_modes[settings.video_out_mode]));
        }

        break;
      }

      default:
        break;
      }

      if (inbyte == 'q')
      {
        inbyte = 'h';
        break;
      }

      inbyte = 0;
    }

    break;
  }

  case 'h':
    print_main_menu();
    inbyte = 0;
    break;

  case 'w':
    inbyte = 0;
    save_settings();
    break;

  default:
    break;
  }
}


settings_t settings_mode1 =
{
  .video_out_mode = VGA1280x1024_d24,
  .scanlines_mode = false,
  .x3_buffering_mode = false,
  .video_sync_mode = true, // separate
  .cap_sync_mode = EXT2,
  .frequency = 6144000,
  .ext_clk_divider = 1,
  .delay = 6,
  .shX = 105,
  .shY = 37,
  .pin_inversion_mask = 0b0000000,
  .len_VS = 30*7,
};

settings_t settings_mode2 =
{
  .video_out_mode = VGA1280x1024_d4,
  .scanlines_mode = false,
  .x3_buffering_mode = false,
  .video_sync_mode = true, // separate
  .cap_sync_mode = EXT,
  .frequency = 7000000,
  .ext_clk_divider = 2,
  .delay = 0,
  .shX = 141,
  .shY = 45,
  .pin_inversion_mask = 0b1000000,
  .len_VS = 30*7,
};

void setup()
{
  flash_safe_execute_core_init();
  for (uint i=MODE1_PIN; i<=RESET_PIN; ++i)
  {
    gpio_init(i);
    gpio_set_dir(i, GPIO_IN);
    gpio_pull_up(i);
  }

  //Serial.begin(9600);

  // loading saved settings
  memcpy(&settings_arr[0], saved_settings, sizeof(settings_arr));
  if (settings_arr[0].video_out_mode != VGA1280x1024_d24)
    settings_arr[0] = settings_mode1;
  if (settings_arr[1].video_out_mode != VGA1280x1024_d24)
    settings_arr[1] = settings_mode1;
  // correct if there is garbage in the cells
  check_settings(&settings_arr[0]);
  check_settings(&settings_arr[1]);
  set_capture_settings(&settings_arr[_80DS]);

  set_v_buf_buffering_mode(settings_arr[_80DS].x3_buffering_mode);

  draw_welcome_screen(*(vga_modes[settings_arr[_80DS].video_out_mode]));

  set_scanlines_mode();

  if (settings_arr[_80DS].video_out_mode == DVI)
  {
    start_dvi(*(vga_modes[settings_arr[_80DS].video_out_mode]));
  }
  else
  {
    start_vga(*(vga_modes[settings_arr[_80DS].video_out_mode]));
  }

  start_core0 = true;

  serial.println("  Starting...");
  serial.println("");
}

void loop()
{
  char inbyte = 0;
  uint8_t button_pressed = 0;
  while (1)
  {
    sleep_ms(100);

    if (serial.available())
    {
      inbyte = 'h';
      break;
    }
    if (gpio_get(MODE1_PIN)==0 && button_pressed != 1)
    {
      button_pressed = 1;
      _80DS = true;
      set_capture_settings(&settings_arr[_80DS]);
    }
    else if (gpio_get(MODE2_PIN)==0 && button_pressed != 2)
    {
      button_pressed = 2;
      _80DS = false;
      set_capture_settings(&settings_arr[_80DS]);
    }
    else if (gpio_get(RESET_PIN)==0 && button_pressed != 3)
    {
      button_pressed = 3;
      save_settings(); 
    }
   else
    {
      button_pressed = 0;
    }
  }

  serial.println(" Entering the configuration mode");
  serial.println("");

  while (1)
  {
    sleep_ms(10);

    process_menu(inbyte);

    if (inbyte == 'q')
    {
      inbyte = 0;

      serial.println(" Leaving the configuration mode");
      serial.println("");
      break;
    }
  }
}


void setup1()
{
  while (!start_core0)
    sleep_ms(10);

  start_capture(&settings_arr[_80DS]);
}

void __not_in_flash_func(loop1())
{
  if (bRestart)
  {
    sleep_ms(100);
    stop_capture();
    start_capture(&settings_arr[_80DS]);
    bRestart = false;
  }
  if (bStopCore1)
  {
    bCore1Stopped = true;
    uint32_t ints = save_and_disable_interrupts();
    while (bCore1Stopped) ;
    restore_interrupts_from_disabled(ints);
  }
  sleep_ms(100);
}
