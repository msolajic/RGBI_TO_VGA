Firmware for RGBI to VGA adapter, adjusted for TIM011 v2.0.

Original by Dimitriy Cvetkov, with updates by Marko Solajic.

Updates for TIM011:
	- Updated palette with dark grey
	- Fixed capture settings so line 256 shows up on VGA screen
	- Set up default values for capture

--- original README below ---

Firmware for zxrgbi2vga&hdmi adapter. Supports extended Profi 512x240 16 colour mode without palette.

For detailed hardware and original software information, please refer to the source: [ZX_RGBI2VGA-HDMI](https://github.com/AlexEkb4ever/ZX_RGBI2VGA-HDMI/).

See .program pio_capture_2 in pio_programs.pio and void __not_in_flash_func(dma_handler_capture2()) in rgb_capture.c for 512x240 mode

SSI and KSI should be connected to corresponding pins, sync mode - separate. F should be dot clock (14 or 12 MHz) - Pin 2 U4 for Profi3.2

