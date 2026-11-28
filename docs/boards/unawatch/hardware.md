# UNA Watch: hardware bring-up specification

This page describes the UNA Watch hardware as the stock UNA kernel drives it. It
is meant as a reference for porting PebbleOS to the watch: what a new OS needs
to boot, to coexist with the stock bootloader, and to drive the hardware.

**Source.** Everything here was read from the public kernel tree
`una-kernel-public` (commit `eb22e63`). The only other inputs are the shipped
bootloader binary `una-bootloader/Exe/UnaBootloader_0.1.4.hex` (disassembled,
since it ships without symbols) and the shipped shared-libc binary.

**Citations.** Paths are relative to the root of the kernel repo unless they
start with `unapebble/`. The following prefixes are used:

| Prefix | Path |
|---|---|
| `CUBE/` | `Software/App/UnaWatch-CubeIDE/` |
| `L/` | `Software/Libs/` |
| `main.h` | `CUBE/Core/Inc/main.h` |
| `main.cpp` | `CUBE/Core/Src/main.cpp` |
| `BL@0x0800xxxx` | An address in the disassembled bootloader 0.1.4 |

**Confidence.** Every statement is either **confirmed**, meaning read directly
in source or in the bootloader disassembly, or marked **(inferred)**.

The SDK git submodule (`SDK/`, pointing to `UNAWatch/una-sdk`) is not
initialised in the tree that was read. Its `Utilities/Scripts/build-cube/`
scripts were therefore not available; see section 9.

---

## 1. MCU, memory map and clocks

### 1.1 Which MCU: STM32U5A5QJ (4 MB flash), not STM32U595QI

| Evidence | Points to |
|---|---|
| The only CubeIDE build configuration is `Debug-STM32U5A5`. It sets MCU = `STM32U5A5QJIxQ` and defines `STM32U5A5xx` (`CUBE/.cproject:5,20,52,203`). | U5A5QJ |
| The build uses the U5A5 startup file and excludes the `STM32595QIIxQ/` startup directory (`CUBE/.cproject:340`). | U5A5QJ |
| The linker script targets "STM32U5A5xJ … 4096Kbytes FLASH". The kernel region is `0x08060000`, length 3712K = 4096K − 384K (`CUBE/STM32U5A5QJIXQ_FLASH.ld:8-9,95`). On a 2 MB part, 3712K would not fit. | 4 MB part |
| The CLI help gives a core module name example of `U5A5-04G` (`L/Source/Application/Components/CLIUtils/UtilModuleInfo.cpp:26`). | U5A5 with 4 GB eMMC |
| `CubeMX/UnaWatch.ioc` says `Mcu.CPN=STM32U595QII6Q`, `Mcu.Name=STM32U595QIIxQ`, package UFBGA132 (`.ioc:59,92,93,602`). | U595QI (2 MB) |
| The libc project directory is named `UnaWatch-STM32U595QIIXQ-LibC-CubeIDE`. Its `.cproject` actually targets `STM32U575ZITxQ`. | Stale template |
| The TouchGFX project says `"SubFamily": "STM32U575/585"` (`Software/App/TouchGFX-GUI/UnaWatchGUI.touchgfx`). | Stale template |
| Bootloader 0.1.4 reads the flash-size register (constant `0x0BFA0000` at BL@0x080049A4; `FLASHSIZE_BASE` = `0x0BFA07A0`). It limits images to flash size − `0x60000`, and it sets or clears `FLASH_OPTR` bit 21 depending on flash size (BL@0x08001992). | Bootloader supports both 2 MB and 4 MB parts (inferred) |

**Conclusion.** The firmware as built today targets the STM32U5A5QJIxQ (4 MB
flash). The `.ioc` is a pin-planning file left over from an earlier U595QI
(2 MB) design, and nobody migrated it. The pinout is unaffected:

- The U595 and U5A5 are the same die family in the same UFBGA132 package.
- The only difference is that the U5A5 adds the crypto blocks (AES, SAES, PKA,
  OTFDEC). The kernel does not use them: `HAL_CRYP`, `HAL_HASH` and `HAL_PKA`
  are disabled at `CUBE/Core/Inc/stm32u5xx_hal_conf.h:40,52,69`.
- (Inferred) Early or prototype watches may carry the 2 MB U595QI, and the
  bootloader appears to handle both.

**Recommendations for the port:**

- Read `FLASHSIZE_BASE` (`0x0BFA07A0`, in KB) at boot.
- Keep the image under 1664 KB (2 MB − 384 KB) unless you have confirmed the
  part on the target watch.
- The core is a Cortex-M33 with FPU and DSP. TrustZone is off: the kernel
  builds NonSecure and sets `configENABLE_TRUSTZONE 0`
  (`CUBE/Core/Inc/FreeRTOSConfig.h:66`).

### 1.2 Flash map (confirmed)

| Range | Size | Owner | Notes |
|---|---|---|---|
| `0x08000000`–`0x08027FFF` | 160 KB | Bootloader | Info block `bl.x.y.z` at `0x08027F80` (`main.h:359`). **Do not touch.** |
| `0x08028000`–`0x0805FF7F` | 224 KB − 128 B | Shared newlib "libc" image | Linked at `0x08028000` (`una-libs/.../STM32U575ZITXQ_FLASH.ld:48`). |
| `0x0805FF80`–`0x0805FFFF` | 128 B | libc info block | `sl.0.0.3` (`main.h:358`; `FwInfoBlock_t` in `L/Header/FwInfoBlock/FwInfoBlock.h:25-31`). The bootloader compares it with the `.ota` header unless that field is 0 (section 9). |
| `0x08060000`–end of flash | 3712 KB on a 4 MB part | **Kernel**, the replacement OS goes here | Vector table at offset 0. The bootloader erases from 8 KB page 0x30 to the end of flash before writing. |

**Keeping the libc region.** A replacement OS does not need the libc image at
all, but it must leave the region intact so the stock kernel can be restored.
The bootloader's flash-write routine refuses addresses below `0x08028000`.

**Fixed addresses inside libc.** The stock kernel calls libc at fixed absolute
addresses:

- About 336 `PROVIDE()` symbols in `una-libs/Exe/libc_exports_0.0.3.ld`.
- `LibcIM_init = 0x0804A409` in `una-libs/Exe/libc_exports_kernel_0.0.3.ld`.
- `__init_libc` is called from `Reset_Handler`
  (`CUBE/Core/Startup/STM325A5QJIxQ/startup_stm32u5a5qjixq.s:95`;
  `CUBE/Core/Startup/InitLibC.cpp:53-66`).

PebbleOS can link its own C library and ignore all of this.

### 1.3 RAM map

| Range | Size | Stock use | Port guidance |
|---|---|---|---|
| `0x20000000`–`0x2025DFFF` | 2424 KB | Kernel `RAM` region: `.data`, `.bss`, heap, MSP stack at the top (`_estack = 0x2025E000`) (`.ld:42,92`) | Free for PebbleOS. |
| `0x2025E000`–`0x2026FFFF` | 72 KB | `RAM_CRASH`, NOLOAD, preserved across reset (`.ld:93,199-228`): `.crash_dump`, `.log_buffer` (64 KB), `.systemlog`, `.boot_flags` | Kernel-only. The bootloader does not read it (section 9). PebbleOS may reuse it, for example for its own retained RAM. |
| `0x28000000`–`0x28003FFF` | 16 KB | SRAM4, used by libc for its `.data`/`.bss` (`.ld:94` comment; `una-libs/.../STM32U575ZITXQ_FLASH.ld:47`) | Free if you do not call the UNA libc. |

**Physical SRAM layout.** From `stm32u5a5xx.h:1757-1782`:

| Block | Base | Size |
|---|---|---|
| SRAM1 | `0x20000000` | 768 KB |
| SRAM2 | `0x200C0000` | 64 KB |
| SRAM3 | `0x200D0000` | 832 KB |
| SRAM5 | `0x201A0000` | 832 KB |

SRAM1, SRAM2, SRAM3 and SRAM5 are contiguous. Together they make 2496 KB,
ending at `0x20270000`.

**Bootloader constraint (confirmed, BL@0x08001C98).** The initial SP word at
`0x08060000` must satisfy `0x20000000 ≤ SP ≤ 0x20270000`. Otherwise the
bootloader refuses to jump and returns to its menu.

**Shared-RAM handoff.** There is no RAM or register handoff between the
bootloader and the kernel. The only shared state is in the RTC backup
registers:

- **`TAMP->BKP3R` (`RTC_BKP_DR3`) = power-off flag.** The kernel writes 1 to it
  on user power-off (`L/Source/Components/Power.cpp:563`; `main.h:329`).
  - The bootloader reads it at BL@0x080017AC. If it is set, the power button
    must be held for about 500 ms or the bootloader powers off again.
  - The bootloader then clears it.
  - The kernel never reads it.
- **Other backup registers.** Their use is listed at `main.h:325-355`:

  | Register | Contents |
  |---|---|
  | DR0–DR2 | Time, timezone, DST |
  | DR5 | Haptic calibration |
  | DR6–DR10 | GPS reference |
  | DR11–DR12 | Altitude |
  | DR13–DR23 | Magnetometer calibration |

  Leave them alone if you want the stock kernel to round-trip cleanly.

### 1.4 Vector table, VTOR and the state at kernel entry

- **Vector table.** It is at `0x08060000`, in a standard Cortex-M33 STM32U5A5
  layout (`.isr_vector` first in FLASH, `.ld:108-111`).
- **What the bootloader does before the jump (confirmed, BL@0x08001C98):**
  1. Reads SP from `[0x08060000]` and PC from `[0x08060004]`.
  2. Calls a de-init routine.
  3. Executes `cpsid i`.
  4. Sets `SCB->VTOR = 0x08060000` and loads MSP with the stack value.
  5. Branches to PC.

  There is no CRC, magic or version check at boot.
- **On the kernel side:**
  - It sets `VECT_TAB_OFFSET 0x00060000` and writes VTOR again in `SystemInit`
    (`CUBE/Core/Src/system_stm32u5xx.c:139,227`).
  - It re-enables interrupts explicitly, because the bootloader left them
    masked (`main.cpp:156-158`).
- **What is unknown about the entry state (inferred):**
  - **Clocks.** The bootloader drives the display, the eMMC (FatFs) and USB MSC
    (it references RCC, GPIOE, LPTIM1/3, SDMMC1 and USB_OTG_HS). So at entry
    the RCC is probably not at reset defaults, and LPTIM1 and LPTIM3 may still
    be toggling the panel's VCOM. The kernel does not rely on any of this: it
    runs `HAL_Init()` and reprograms the whole clock tree.
  - **What a port should do.** Assume nothing:
    - Switch SYSCLK to MSIS before reprogramming PLL1.
    - Re-initialise every peripheral you use.
    - Mask and clear the NVIC.
  - **IWDG.** No IWDG base or key constant appears in the bootloader, so it
    probably does not start the watchdog. A hardware-watchdog option byte has
    not been ruled out.
  - **Kernel watchdog.** The kernel starts IWDG at 30 s (`main.cpp:194`).

### 1.5 Clock tree (confirmed, `L/Source/Drivers/STM32U5xx/System.cpp:32-64, 334-430`)

**Sources:**

| Source | Configuration |
|---|---|
| **HSE** | 16 MHz crystal on PH0/PH1 (`HSE_VALUE 16000000`, `CUBE/Core/Inc/stm32u5xx_hal_conf.h:102`; `.ioc` "HSE-External-Oscillator"). **Off by default.** It is switched on by reference count only for the USB HS PHY (`System::RequestHSE`, `System.cpp:277-306`; `L/Source/Drivers/STM32U5xx/Usb.cpp:151-167,295-300`: `UsbPhyClockSelection = HSE`). |
| **LSE** | 32.768 kHz **external clock in bypass mode** on PC14 (`CLK_32K_SRC_TYPE RCC_LSE_BYPASS`, `main.h:40-44`; `.ioc:401` "LSE-External-Clock-Source"). LSE drive is set to LOW (`System.cpp:57-58`). It feeds the RTC, SysTick, LPTIM1/3 (display VCOM) and LPTIM4 (tick reference). |
| **LSI** | On, divided by 1 (32 kHz) (`System.cpp:341-344`). Used by IWDG. |
| **MSIS** | Range 0 = 48 MHz. PLL1 source (`System.cpp:353-358`). |
| **MSIK** | Range 4 = 4 MHz. Kernel clock for LPUART1, the GNSS UART (`System.cpp:347-350`). |
| **HSI16** | Not enabled by `ClockConfig160Mhz`. USART3 (debug) selects `RCC_USART3CLKSOURCE_HSI` (`main.h:111`), so the HAL turns HSI on when the debug UART is configured. |

**PLL1:**

| Parameter | Value |
|---|---|
| Source | MSIS 48 MHz |
| M | 3, giving 16 MHz at the PLL input (`PLLRGE` = `RCC_PLLVCIRANGE_1`) |
| N | 10, giving a 160 MHz VCO |
| R | 1, giving **SYSCLK = 160 MHz** |
| Q | 2, giving 80 MHz on CLK48/ICLK for SDMMC1 (`L/Source/Drivers/STM32U5xx/Mmc.cpp:339-341`) |
| P | 1 (unused) |
| MBOOST | DIV1 |
| FRACN | 0 |

Code at `System.cpp:360-368`.

**Bus clocks and power:**

- **Prescalers:** AHB, APB1, APB2 and APB3 all at /1. Flash latency is 4 wait
  states. Voltage scale 1 (`System.cpp:336-337,415-429`).
- **Supply:** SMPS regulator (`HAL_PWREx_ConfigSupply(PWR_SMPS_SUPPLY)`) and
  `HAL_PWREx_EnableVddIO2()` are set before the clocks (`main.cpp:664-672`).
  **VddIO2 must be enabled before port G works.**
- **Instruction cache:** `HAL_ICACHE_Enable()` (`main.cpp:154`).

**Ticks and sleep:**

- **SysTick** is clocked from LSE at 1024 Hz, which is `configTICK_RATE_HZ`
  (`CUBE/Core/Src/stm32u5xx_timebase_systick.c:4`;
  `CUBE/Core/Inc/FreeRTOSConfig.h:76`).
- **LPTIM4** on LSE is a free-running reference used to recover ticks after
  STOP2.
- **Idle** uses STOP2 (`HAL_PWREx_EnterSTOP2Mode`, `System.cpp:123-127`), woken
  by the RTC wake-up timer or an EXTI. On wake the clock is restored in this
  order (`System.cpp:129-192`):
  1. VOS1.
  2. Flash latency 4.
  3. MSIS back to range 0.
  4. Re-lock PLL1.
  5. Switch SYSCLK to PLL1.
- **Microsecond delays** use the DWT `CYCCNT` (`L/Header/Drivers/STM32U5xx/UsTimer.hpp:21-61`).

---

## 2. GPIO map

**Sources.**

- Symbolic pins: `main.h:34-225`.
- Signal and alternate-function assignment: `CUBE/CubeMX/UnaWatch.ioc:230-588`.
- Actual AF numbers and pulls: the MSP init in each driver.
- Boot-time defaults: `PinsDefaultConfig()` (`main.cpp:674-807`).

**Notation.**

| Abbreviation | Meaning |
|---|---|
| "In" / "Out" | MCU direction |
| PP / OD | Push-pull / open-drain |
| PU / PD / NP | Pull-up / pull-down / no pull |
| Active level | Logical "on" or "asserted" level |
| Default | What `PinsDefaultConfig()` sets |

All EXTI inputs use EXTI line = pin number (`L/Source/Drivers/STM32U5xx/Gpio.cpp:280-291`).

**The watch is modular.** There are four boards: a core board, a display
module ("view"), a BLE module ("link1"), and a GPS/IMU module ("link2"), plus
a body-sensor module. Each module has an N24S64B EEPROM on its I2C bus, and
the kernel only creates drivers for modules whose EEPROM answers
(`L/Source/Components/External/ComponentFactory.cpp:52-253`). Pin names
reflect the module connector: `SENSOR1_*` for link1, `SENSOR2_*` for link2,
`SENSORBODY_*` for body.

### 2.1 Clocks and debug

| Pin | Function / AF | Label | Dir | Notes |
|---|---|---|---|---|
| PH0 / PH1 | RCC_OSC_IN / OUT | HSE_CLK_IN / OUT | – | 16 MHz crystal |
| PC14 | RCC_OSC32_IN | CLK_32K_IN | In | 32.768 kHz clock input, LSE bypass. PC15 is labelled CLK_32K_OUT in `main.h:42` but is unused in bypass mode. |
| PA13 | SWDIO (AF0) | SWDIO | – | SWD |
| PA14 | SWCLK (AF0) | SWCLK | – | SWD |
| PB3 | TRACESWO (AF0) | SWO | – | SWO trace (`.ioc:337-341`) |
| PC4 | USART3_TX, AF7 | DEBUG_TX | Out | Debug log TX only, 921600 baud (section 8) |
| PC5 | USART3_RX | – | – | Assigned in `.ioc:418-419` but never configured by the kernel |

### 2.2 Buttons (section 6)

| Pin | Label | Dir / pull | Active | EXTI | Role |
|---|---|---|---|---|---|
| PD4 | SW1 | In, PU | Low | EXTI4, both edges | UP (top left, "L1") |
| PD15 | PWR_ON_1V8_L | In, PU | Low | EXTI15, both edges | SELECT / power (top right, "R1", called SW2) |
| PA5 | SW3 | In, PU | Low | EXTI5, both edges | DOWN (bottom left, "L2") |
| PD8 | SW4 | In, PU | Low | EXTI8, both edges | BACK (bottom right, "R2") |

### 2.3 Display module ("view") (section 5)

| Pin | Panel signal | Label | Function | Default and notes |
|---|---|---|---|---|
| PB10 | VST / GSP | LCD_QSPI_SCK_VST | GPIO out PP | Low |
| PA4 | VCK / GCK | LCD_QSPI_CS_VCK | GPIO out PP | Low. `Gpio.cpp:61` wrongly maps the symbolic pin to PE10; this is harmless because the display driver uses PA4 directly. |
| PA7 | ENB / GEN | LCD_QSPI_IO2_ENB | GPIO out PP | Low |
| PB1 | XRST / INTB | LCD_QSPI_IO0_XRST | GPIO out PP | Low between frames |
| PB0 | HST / BSP | LCD_QSPI_IO1_HST | GPIO out PP | Low |
| PA6 | HCK / BCK | LCD_QSPI_IO3_HCK | GPIO out PP | Low |
| PB2 | FRP / VB, the VCOM | LCD_FRP | LPTIM1_CH1, **AF1** | PWM about 30.7 Hz. PD when the panel is off. |
| PD14 | XFRP / VA | LCD_XFRP | LPTIM3_CH1, **AF14** | Inverted FRP. PD when off. |
| PE0 | R1 | LCD_R1 | GPIO out PP | |
| PE8 | R2 | LCD_R2 | GPIO out PP | |
| PE2 | G1 | LCD_G1 | GPIO out PP | |
| PE10 | G2 | LCD_G2 | GPIO out PP | |
| PE4 | B1 | LCD_B1 | GPIO out PP | |
| PE12 | B2 | LCD_B2 | GPIO out PP | |
| Other PE pins | – | – | In, PU | Reserved, because the driver writes all of `GPIOE->ODR` (`L/Source/Hardware/LS012B7DD06A.cpp:105-106,447-451`) |
| PF3 | Backlight | BACKLIGHT_ON | GPIO **OD** | **Active low**: driven low (on) at boot (`main.cpp:750`) |
| PC13 | Touch INT | TOUCH_INT_L | In, NP | No touch driver exists in the kernel |
| PF1 | Touch reset | TOUCH_RST_L | In, NP | Unused |

**Pin naming.** The `LCD_QSPI_*` names suggest the connector also supports a
QSPI-panel variant (inferred). Only the parallel MIP panel is supported in
code.

### 2.4 Core-board power, storage, USB, misc

| Pin | Function / AF | Label | Dir / type | Active | Notes |
|---|---|---|---|---|---|
| PB6 / PB7 | I2C1 SCL / SDA, AF4 OD | MAIN_SCL / MAIN_SDA | – | – | External pull-ups (`main.cpp:680-682`; `L/Source/Drivers/STM32U5xx/I2c.cpp:565-573`) |
| PG7 | EXTI7 | PWR_INT_L | In, PU (driver), falling edge | Low | Wired-OR of the PCA9420 INTB and the STUSB4500 ALERT (`L/Source/Components/Pmic.cpp:67`; `Power.cpp:501`) |
| PC6 | GPIO in | FUEL_GAUGE_ALRT | In, NP | Low (inferred) | MAX17262 ALRT, unused |
| PF0 | EXTI0 | USB_FAULT_L | In, PU, both edges | Low | NX20P USB protection fault (`L/Source/Components/UsbProtection.cpp:188`). The `.ioc` still calls it ALTIMETER_INT. |
| PB15 | GPIO out PP | USB_DAM | Out | High | Enables the Type-C debug-accessory (DAM) line switch (`UsbProtection.cpp:370`) |
| PA11 / PA12 | USB_OTG_HS DM / DP, internal HS PHY | USB_D_N / USB_D_P | – | – | HS PHY clocked from HSE |
| PD10 | GPIO out PP | EMMC_EN | Out | High | eMMC power: 35 ms after power-on (`L/Source/Components/Flash.cpp:244-283`) |
| PC12 | SDMMC1_CK, AF12 | SDIO_CK | – | – | PU (`Mmc.cpp:363-379`) |
| PD2 | SDMMC1_CMD, AF12 | SDIO_CMD | – | – | PU (external pull-up as well) |
| PC8–PC11 | SDMMC1_D0–D3, AF12 | SDIO_D0–D3 | – | – | PU |
| PF2 | GPIO out PP | HAPTIC_RST_N | Out | High | DRV2625 EN/NRST |
| PB8 | TIM4_CH3, AF2 | BUZZER_PWM | Out | – | Buzzer PWM (`L/Source/Drivers/STM32U5xx/Pwm.cpp:197-214`) |
| PB9 / PD5 | GPIO out PP | BUZZER_EN1 / BUZZER_EN2 | Out | High (inferred) | Buzzer enables |
| PG0 | GPIO out PP | ALTIMETER_EN | Out | High | AP7350 LDO for the MS5837. It must always be driven, never left floating (`main.cpp:767-768`). |

### 2.5 Link1 module (BLE, BlueNRG-2)

| Pin | Function / AF | Label | Use |
|---|---|---|---|
| PG2 / PG3 / PB5 | SPI1 SCK / MISO / MOSI, AF5 | SENSOR1_SCK / MISO / MOSI | BLE SPI |
| PA15 | GPIO out PP | SENSOR1_CS | BLE CS, active low, idles high |
| PA9 | EXTI9, rising, NP | SENSOR1_TX_INT0 | BLE IRQ, active high. USART1_TX in the `.ioc`, not used as a UART. |
| PA10 | (USART1_RX) | SENSOR1_RX_INT1 | Unused |
| PA0 | EXTI0 | SENSOR1_INT2 | Unused |
| PA8 | GPIO, OD at run time, PP low at boot | SENSOR1_RST_L | BLE reset, active low. Held low (BLE off) at boot (`main.cpp:777-780`). |
| PC0 / PC1 | I2C3 SCL / SDA, AF4 | SENSOR1_SCL / SDA | Module EEPROM only |

#### 2.5.1 The BlueNRG-2's firmware and HCI (measured)

This was measured on the watch on 2026-11-27 with the tether experiment
`tools/una/tether/experiments/bluenrg2.c`. The experiment bit-bangs the SPI
protocol on the pins above. Its modes are explained at the top of the file.
Command opcodes and event layouts come from ST's DTM sources (STSW-BLUENRG1-DK
3.2.3, `DTM_cmd_db.c`), the BlueNRG-2N datasheet DS13280 (6.5, SPI protocol)
and Zephyr's `drivers/bluetooth/hci/hci_spi_st.c`. Nothing was written to the
chip's flash, and no updater command was sent.

**SPI framing (confirmed on the watch).** UNA's driver and DS13280 agree,
and so did the chip:

- **Write.** Take CS low, wait for IRQ to go high, then exchange 5 bytes:
  `0x0A 0 0 0 0` out, and `xx WBUF_L WBUF_H RBUF_L RBUF_H` back. Send the
  packet if WBUF is at least its size. Wait for IRQ to go low, then raise CS.
- **Read.** When IRQ goes high on its own, do the same with `0x0B` and read
  RBUF bytes.
- **Packets.** They are H4 packets with their type byte: `01` command,
  `04` event, `02` ACL.
- **Bus speed.** Bit-banged at about 250 kHz, mode 1, it worked first time.
  The first byte back was `0xff` every time.

**Start-up.** The RESET line rises slowly: it still reads low 50 µs after
the pin is released. Its pull-up is on the module, since the STM32 pin has
none.

| Restart | IRQ after | Start event (`aci_blue_initialized_event`) |
|---|---|---|
| Restart | IRQ after | Start event (`aci_blue_initialized_event`) | Seen |
|---|---|---|---|
| First reset of each experiment run (RESET held low by the stub for a second or more, then a 5 ms pulse) | 2.2 s | `04 ff 03 01 00 01`: reason 1, firmware started properly | every time, 20 runs |
| `HCI_Reset`: Command Complete with status 0, then the chip restarts by itself | 71 ms | `04 ff 03 01 00 04`: reason 4, **updater mode** | 4 of 4 |
| A reset of 5 ms to 3 s, straight after a start (normal or updater) | 94 ms | reason 4 | every time, 16 times; MISO pulled up, down or floating made no difference |
| From updater mode: RESET low, the SPI pins set up again, 2 s, then a 5 ms pulse (mode 6) | 2.2 s | reason 1 | 8 of 8 |
| A reset of 5 ms or 300 ms straight after a normal start, SCK, MOSI and CS analog in reset, CS driven high 5 ms before the release (mode 7) | 2.2 s | reason 1 | 2 of 2 |
| `HCI_Reset` after a normal start, SCK, MOSI and CS analog from its Command Complete on (mode 8; approved, decision 261127-233626-01) | 1.7 s | reason 1 | 2 of 2 (the experiment, and the NimBLE transport once) |
| PebbleOS's transport at boot: RESET held since the device's init, CS an input, the SPI pins analog | 2.2 s | reason 1 | 9 of 9 boots |

The DTM's reason codes are: 2, updater by ACI command; 3, bad BLUE flag;
4, updater by boot pin. In ST's DTM the boot pin is the chip's IO3, which
ST's updater source gives as the SPI MOSI line; MOSI held high starts the
updater (see the DTM image below), and the state of MISO doesn't decide it.
What decides it is the state of the SPI lines during the reset: with SCK,
MOSI and CS driven (CS high) a reset of any length after a start ends in
the updater; with them undriven (analog), even a 5 ms reset straight after
a start starts normally (mode 7, 2026-11-27). Mode 6 left them as the stub
found them, undriven. Driving CS high may feed the chip through its pin
while RESET is low, so that it does not go through a power-on start
(inferred). What PA8 really drives on the
module is not known: the slow RESET rise and the 2.2 s start look like an
RC or a supply switch (inferred).

In updater mode, every HCI and ACI command gets Command Status `0x01`
(unknown command) until a restart that starts normally. Stock UNA gets
away with a 5 ms pulse because it resets only after the chip has been held
in reset since boot, and it never sends `HCI_Reset`. Consequences for a
driver:

- **It must not send `HCI_Reset` with the SPI lines driven.** With them
  released from its Command Complete on, the chip restarts normally
  (mode 8); PebbleOS's transport sends none anyway, as nothing needs it.
- **It must check the start event's reason**, and restart with the mode 6
  sequence until it gets reason 1.
- **It must leave the SPI lines undriven while RESET is low**, CS too, and
  drive CS high only shortly before the release.

**What it runs.** After a normal start:

| Query | Answer |
|---|---|
| `HCI_Read_Local_Version_Information` (0x1001) | HCI version 0x0B, revision 0x1242, LMP version 0x0B, manufacturer 0x0030 (ST), subversion 0x0013 |
| `aci_hal_get_fw_build_number` (0xFC00) | 2353 |
| `aci_hal_get_firmware_details` (0xFC01) | **DTM v3.2.0, SPI variant**; **BLE stack v2.1.3** (ST writes it 2.1c), variant 0x000F (privacy, secure connections, central role, data length extension), build 2353 |
| `HCI_Read_BD_ADDR` / config data 0x00 | 02:80:E1:00:00:00: no public address programmed |
| config data 0x80 | a static random address, which stock UNA uses as the watch's address |
| config data 0x2C (link layer only) | 0, and 1 after the write below |

So the module runs ST's standard **DTM network-coprocessor firmware**,
with ST's BLE stack 2.1 on the chip and the SPI updater kept. It is not a
custom UNA image.

**Standard HCI.** The chip accepts the standard LE controller commands,
both with its stack on and in link-layer-only mode:

| Command | Answer |
|---|---|
| `HCI_LE_Read_Buffer_Size` | 251-byte ACL packets, 7 of them |
| `HCI_LE_Read_Local_Supported_Features` | `e9 00 00 08`: encryption, peripheral-initiated feature exchange, data length extension, LL privacy, extended scanner filter policies, bit 27. **No 2M PHY, no extended advertising, no CSA #2, no LE ping, no connection parameters request.** |
| `HCI_LE_Read_Maximum_Data_Length` | 251 bytes / 2120 µs both ways |
| White list / resolving list | 31 / 15 entries |
| `HCI_LE_Read_Advertising_Channel_Tx_Power` | 8 dBm |
| `HCI_Read_Local_Supported_Commands` | Every LE 4.2 command: advertising, scanning, connections, encryption, LTK replies, data length, P-256 key and DHKey, resolving list and address resolution. None from 5.0: no PHY commands. Of the controller-baseband commands, only Reset, Set Event Mask, Read Transmit Power Level and Read RSSI. |
| `HCI_Read_Buffer_Size` (0x1005) | unknown command, as on an LE-only controller |
| `HCI_Set_Event_Mask`, `HCI_LE_Set_Event_Mask` | success |
| `HCI_LE_Set_Advertising_Parameters` / `Data` / `Enable` | success. The non-connectable advertising was seen on air by a scanner next to the watch. |

**Link-layer-only mode.** `aci_hal_write_config_data(0x2C, 1, 0x01)`
(`01 0c fc 03 2c 01 01`), sent as the first command after the start event,
returns success. Config 0x2C then reads 1, and the HCI commands above
behave as before. The setting is in RAM and lasts until the next restart.
A restart, even a short one, clears it (mode 7: it reads 0 again).

ST's DTM source and the drivers that use the mode (Zephyr's
`st,hci-spi-v2` with `req-ll-only`, STM32duinoBLE on the BlueNRG-M2SP)
have the DTM pass ACL data to and from the host as H4 packets (`02`).
**On this chip it does not** (PebbleOS's NimBLE transport and a BlueZ
central, 2026-11-27):

| Run | Seen |
|---|---|
| Mode on, then `aci_gatt_init` and `aci_gap_init` or not; NimBLE as host (4 runs) | LE Enhanced Connection Complete, a Data Length Change (251 bytes), then nothing from the chip but HCI events for 22 s: no ACL, no vendor event. The host's first ACL packet, 15 bytes, gets no Number Of Completed Packets. The central's service discovery times out and it disconnects. |
| Mode off, `aci_gatt_init` and `aci_gap_init` (1 run) | The central discovers the chip's own GAP and GATT services in 1 s, reads Appearance, Device Name (empty) and the preferred connection parameters, and writes the Service Changed CCCD: the chip's stack answers. NimBLE's ACL packet gets no completion either. |
| Mode written after a restart from `HCI_Reset` (mode 8's restart), then `aci_gatt_init` and `aci_gap_init`, as STM32duinoBLE orders them; NimBLE as host (1 run) | The same as with the mode on: no ACL, discovery timed out (decision 261127-233959-01). |

So the mode turns the chip's host off, but does not route the link's data
to the SPI. ST's community forum has the same report for the stock DTM,
and ST's answer is a DTM built with its `LL_ONLY` option (decision
261127-204244-01). Other findings of those runs:

- **Disconnections** come with reason `0x03`, whoever ends the link.
- **LE Read Remote Features** just after a connection gets a Command
  Status of Controller Busy (`0x3A`) whose opcode reads `0x0016`, the OGF
  bits clear (decision 261127-194000-01).
- **Config 0x80** reads `ce 45 ec e6 ce 01` on this watch, least
  significant octet first: its top two bits are clear, so it is no static
  random address as it stands (decision 261127-193700-01). That last byte
  was cut by the SPI: it is `0xcd` (2.5.2).
- The connection came at an interval of 36 (45 ms) from BlueZ, and the
  link was updated to 251-byte PDUs.

**The DTM's ACL path and its updater, from ST's source.** Read in public
mirrors of ST's BlueNRG-1_2 DK, 3.1.0 and 3.2.3 (DTM 3.4.0); the chip runs
DTM 3.2, of SDK 3.2.1, whose source was not found. Not checked on the chip.

- In DTM 3.1.0 `hci_parser.c` forwards ACL data, both ways, only under
  `#if defined(LL_ONLY) || DTM_DEBUG==1`; in 3.4.0 the guard is gone. The
  DTM 3.2 on the chip most likely still has it, which would explain why
  config 0x2C alone passes no ACL (inferred). `LL_ONLY` is a preprocessor
  define of the DTM's build, with the same prebuilt stack library; with it
  the DTM writes 0x2C itself at each start. ST ships no `LL_ONLY` image in
  the DK; stm32duino's X-NUCLEO-BNRG2A1 package has `DTM_LLOnly.bin`, not
  checked to be the SPI build. SDK 3.2.0/3.2.1 had Atollic TrueSTUDIO
  (GCC, free) projects, 3.2.2 and later WiSE-Studio GCC ones. The DK is
  under ST's Production Limited License Agreement, which forbids combining
  it with open-source code.
- `HCI_Reset` answers, then `NVIC_SystemReset()`: the chip boots again
  through the updater into the DTM, its RAM, config 0x2C included, lost.
  `aci_hal_updater_start` (0xFC20) sets a RAM flag at 0x20000030 before the
  same reset: reason 2. The updater (0x10040000, 8 KiB) starts the DTM at
  0x10042000 unless that flag, a bad BLUE flag at 0x10042014 (reason 3) or
  IO3 high (reason 4) says otherwise.
- In the updater, 0xFC21 reboot, 0xFC22 version, 0xFC23 buffer size (64),
  0xFC24/0xFC25 erase and set the BLUE flag, 0xFC26 erase a 2 KiB page,
  0xFC27 program, **0xFC28 read a block**, 0xFC29 CRC-32 of 2 KiB pages,
  0xFC2A hardware version. They check only `addr >= 0x10042000`: the
  updater cannot be overwritten, nor read back; the DTM image, and the
  stack's data page at 0x1007F000, can. So the image could be backed up,
  64 bytes a read, before anything is written. Nothing of this was sent.

**The DTM image, backed up and analysed (2026-11-28).** Read through the
updater (`0xFC28`, 64-byte blocks) from 0x10042000 to the end of flash,
three times, identical, and matching the updater's CRC in 8-page chunks
(`0xFC29`); kept outside the repository, as it is ST's code, with its
SHA-256 (`9a355e01…7393cb5`, which `tools/una/bluenrg2/patch.py` checks).
The updater answered version 6, buffer size 64, hardware version 1, and
refused a read below the DTM (0x10040000: status `0xFA`). What it holds:

- The code from 0x10042000 to 0x10062cb8, blank flash after it, a block of
  device data at 0x10067804 (32 bytes, then 6 that resemble the address at
  config 0x80, then 0xAAAAAAAA), the rest blank, the page at 0x1007F000
  too. The vector table has 0xAA5555AA at +0x10, the BLUE flag at +0x14 and
  0x20BB8, the image's length, at +0x18.
- Matched against the objects of ST's stack library (DK 3.2.3): about 420
  functions are found as they are. **Both ACL paths are missing.** The HCI
  parser's packet dispatch (at 0x1004e45a) handles commands and vendor
  packets only: an ACL packet is accepted by the SPI framing and then
  dropped. The stack's `hci_tx_acl_data` is not linked at all. On the way
  up, the stack does hand received data to `hci_rx_acl_data` in
  link-layer-only mode (the check is the bit 0 of its context at +0xA4),
  which queues it, but the callback that would send it to the host
  (0x10045e8e) calls a weak default that the linker reduced to
  `movs r0, #0`. So the DTM was built without `LL_ONLY`, and no setting
  can make it pass ACL data.
- What a patch would use is there: the packet allocation, write, transmit
  and release functions the stack's own L2CAP uses, `get_connection_no`,
  the SPI event queue (`send_event_isr`, 0x100477ac), and the completion
  path that reports Number Of Completed Packets.

**A binary patch, tried and undone.** `tools/una/bluenrg2` holds our code
for the two paths (268 bytes, for 0x10068000, in blank flash) and a tool
that points the parser's dispatch and the callback at it, writing only the
three pages concerned (decision 261128-003746-01). Written and read back
exactly, page by page, CRC-checked: **the patched DTM never starts**, no
start event in 3 s. Bisected, each time restored: the code page alone
starts; either redirection alone does not; a single padding byte changed
in the code (0x1004e492, never executed) does not either. So something
checks the DTM's code before it runs, most likely over the 0x20BB8 bytes
the vector table gives; the DTM's own code has no reference to that length
or to a CRC of it, so the check is probably in the updater, which cannot
be read back through itself, or in the ROM (inferred). The reference value
was not found: none of CRC-32, the updater's CRC, a sum or SHA-256 over the
plausible ranges matches a word or the 32-byte block in the image.

ST's own update flow (BLUE flag erased, the first page rewritten with the
flag blank, the pages written, the flag set and read back) gives the same:
no start. A word written in blank flash just after the code (0x10062cc0,
0x10062ffc) does not stop the start; the length word at +0x18 changed to 0
does. So the check covers the vector table and the code, and nothing
after it, and its reference is made neither by the update flow nor kept in
readable flash (decision 261128-004526-01).

**The updater by its boot pin.** ST's updater source names the boot pin
"IO3 -> SPI MOSI PIN", not MISO. With MOSI (PB5) driven high through the
release of RESET, the updater starts (reason 4) whatever the DTM does: 8
of 8, among them every time the patched DTM did not start. MISO pulled up
or driven high did nothing. That is the recovery for a DTM that does not
start; each patched variant was restored this way and the chip started
normally again. The flash read back as the original image at the end.

**Replacing the firmware.** There are three ways:

1. **The DTM's SPI updater.** It lives at 0x10040000 to 0x10042000 in the
   chip's flash. It starts by command (`aci_hal_updater_start`, 0xFC20),
   when IO3 (MOSI) is high at start-up, or when the BLUE flag is bad. Its
   commands 0xFC21 to 0xFC2A erase, program and CRC the application area
   only. Because a bad BLUE flag starts it, an interrupted update starts
   the updater again rather than a broken image. That makes it
   recoverable, as long as the updater is not overwritten, which it
   refuses to do in software.
2. **The ROM UART bootloader.** It starts when IO7 is high at reset; IO7
   is the SPI IRQ line, PA9. It talks on DIO8/DIO11, and those UART pins
   are not known to reach the STM32. PA10 (SENSOR1_RX_INT1) is a
   candidate (inferred).
3. **SWD on the module.** It needs the module opened.

Nothing needs replacing: the image the module already runs works as an
HCI controller in link-layer-only mode. ST's images are under ST's
licences (PLLA, SLA0055), not open ones.

**Module EEPROM.** This is an N24S64B at 0x50, with its identification
page at 0x58. Both answered on I2C3. Stock UNA reads only its unique ID,
to find out whether the module is fitted.

#### 2.5.2 The chip's own stack, through ACI (measured)

Measured on the watch on 2026-11-28 with the ACI transport
(`third_party/nimble/aci`, decision 261128-081601-02) and a BlueZ central.
The commands and events are those of ST's ACI for the BlueNRG-1 and -2, as
X-CUBE-BLE2's headers give them (read for the interface only); DTM 3.2.0
takes every one used here with those layouts.

- **The last byte of a read.** Over the STM32's SPI the last byte of every
  transfer came back with only its two low bits, at 1.25 MHz as at
  625 kHz; the bit-banged reads of the tether experiment were right. The
  cause is an erratum of the SPI: with the CRC off and TSIZE set, a CRCSIZE
  below 8 bits corrupts the last frame received, and the driver wrote it 0;
  it now keeps the reset value (decisions 261128-092026-01,
  261128-100132-01). Before that, config 0x80 read `ce 45 ec e6 ce 01`: it
  is `ce 45 ec e6 ce cd`, the static random address `CD:CE:E6:EC:45:CE`
  (decision 261128-100132-02). The disconnection reason `0x03` of every
  link, and the Command Status opcode `0x0016` in 2.5.1, were that byte
  cut.
- **Start.** `aci_gatt_init`, `aci_gap_init` (peripheral, no privacy, a
  32-byte name: GAP service at `0x0005`, Device Name `0x0006`, Appearance
  `0x0008`; the GATT service at `0x0001`), then
  `aci_gatt_set_access_permission` makes the name and appearance read only
  (the chip has them writable by peers).
- **GATT database.** `aci_gatt_add_service` and `aci_gatt_add_char` give the
  handles in order from `0x000c`; the value is at the declaration plus one,
  the CCCD next. The values of every characteristic share about a kilobyte:
  seven of 64 bytes, two of 244 and the GAP's fit, a third of 244 got
  `0x48`, out of memory. The ATT MTU is at most **220**, whatever the
  central offers (BlueZ asks 517).
- **Reads and writes.** A characteristic with
  `GATT_NOTIFY_READ_REQ_AND_WAIT_FOR_APPL_RESP` raises a read-permit event,
  answered with `aci_gatt_update_char_value_ext` and `aci_gatt_allow_read`
  or `aci_gatt_deny_read`; with `GATT_NOTIFY_WRITE_REQ_AND_WAIT_FOR_APPL_RESP`
  a write-permit event, answered with `aci_gatt_write_resp`; with
  `GATT_NOTIFY_ATTRIBUTE_WRITE` an attribute-modified event, the CCCD's
  writes included.
- **Pairing.** `aci_gap_set_io_capability(display yes/no)` and
  `aci_gap_set_authentication_requirement(bonding, MITM, SC only, keys of
  16)`: a BlueZ central pairs by numeric comparison
  (`aci_gap_numeric_comparison_value_event`, answered with
  `aci_gap_numeric_comparison_value_confirm_yesno`), then
  `aci_gap_pairing_complete_event`; the encryption change comes before the
  pairing's end, and `aci_gap_get_security_level` gives mode 1 level 3.
  On a reconnection with a bond the chip raises
  `aci_gap_pairing_complete_event` too, before the encryption change. The
  bond is in `aci_gap_get_bonded_devices` and survives a restart of the
  chip. No event comes before a pairing starts: it cannot be refused before
  its first confirmation.
- **Connections.** HCI LE advertising, as NimBLE sends it, works with the
  stack on: the connection is the stack's. BlueZ connects at 45 ms and
  grants the L2CAP parameter updates PebbleOS asks for (15 ms; 45 to 60 ms
  with a latency of 4). Data length 251 both ways.

### 2.6 Link2 module (GNSS AG3335M, BMI270, BMM350)

| Pin | Function / AF | Label | Use |
|---|---|---|---|
| PA2 / PA3 | **LPUART1** TX / RX, **AF8** (USART2 AF7 in the `.ioc`) | SENSOR2_TX_INT0 / RX_INT1 | GNSS UART at 921600 (`L/Source/Drivers/STM32U5xx/LpUart.cpp:588-615`) |
| PG5 | GPIO out PP | SENSOR2_RST_L | Really **GNSS_EN**, active high, with a pull-up on the GPS board. Driven low (GNSS off) at boot (`main.cpp:793-798`). |
| PA1 | EXTI1, rising, NP | SENSOR2_INT2 | BMI270 interrupt |
| PC2 | EXTI2, rising, PD (SPI2_MISO in the `.ioc`) | SENSOR2_MISO | BMM350 interrupt |
| PC3 / PD1 | SPI2 MOSI / SCK | SENSOR2_MOSI / SCK | Unused. PC3 has a pull-down on the GPS board (RTC_EINT). |
| PB12 | GPIO | SENSOR2_CS | Unused |
| PD12 / PD13 | I2C4 SCL / SDA, AF4 | SENSOR2_SCL / SDA | EEPROM, BMI270, BMM350 |

#### 2.6.1 The AG3335M on PebbleOS (measured)

Read on a watch through PebbleOS's driver (`drivers/gnss/ag3335.c`,
decisions 261127-194525-01 and -02), indoors:

- **Firmware**: `AG3335M_V3.2.1.AG3335_20240426`, which the receiver
  announces on its own (`$PAIR021,...`) after power-up and answers to
  PAIR020.
- **Power-up**: about 0.35 s after GNSS_EN goes high it sends
  `$PAIR001,002,1` then `$PAIR001,002,0` (GNSS subsystem on), with
  `$PAIR011,001` (started) and its aiding requests, `$PAIR010,1,-1` (time) and
  `$PAIR010,2,-1` (location), which nothing answers yet. It acknowledges a
  command in 2 to 80 ms, often first with result 1 (processing); a query's
  answer comes after its acknowledgement.
- **Lost commands**: a command is sometimes neither acknowledged nor carried
  out, seen after PAIR003 (GNSS subsystem off) and always with its system
  sleep unlocked (PAIR382,0): the receiver's UART seems to sleep, and the
  first characters wake it. Sending again after 0.5 s gets the answer; the
  driver sends a command up to four times.
- **Defaults**: GPS, GLONASS, Galileo, BeiDou and QZSS searched, NavIC not
  (`$PAIR067,1,1,1,1,1,0`); L5 off once set so (`$PAIR105,0`). Before it knows
  the time, its RMC dates are 1980-01-05 or -06.
- **Output**: one burst a second: GGA, RMC, four GSA, GSV per constellation
  and signal, and three sentences more, about 450 bytes with no satellites in
  view, under 6 ms of the line.
  Indoors it listed up to 4 satellites in view, 2 or 3 of them with a signal
  of 17 to 21 dB-Hz, and had the UTC time within 3.5 to 4.5 minutes of power-up,
  without a fix.
- **Draw**, the receiver searching, the watch otherwise idle on its watchface
  with the CPU in Stop 2 about 97% of the time: about 10 mA with its system
  sleep locked on L1, as UNA's firmware runs it, 9.2 mA with it unlocked,
  8 to 9 mA on GPS alone, 16 to 17.5 mA on L1 and L5; back to the board's
  0.8 to 1.0 mA off (index, Power).
- **Start-up garbage**: each power-up gives 4 lines that are not sentences
  (bad checksums or stray characters) before `$PAIR001,002`.

#### 2.6.2 The BMI270 and the BMM350 (measured)

Read on a watch, with PebbleOS's drivers (decisions 261127-194845-01 to -03):

- I2C4 answers at `0x14` (BMM350), `0x50` and `0x58` (the module's EEPROM)
  and `0x68` (BMI270), not at `0x69`.
- BMI270: chip ID `0x24`. Until Bosch's 8 KiB configuration file is loaded,
  INTERNAL_STATUS reads 0 and the accelerometer reads zeros, its power on or
  not; loaded, the status reads 1 (init OK) within the first 10 ms poll.
- In advanced power save, FIFO reads of about 40 frames (240 bytes) and
  more return filler bytes (`0x90`, `0x8b`) and frames out of step, whether
  or not the 450 us Bosch's SensorAPI waits after every access in it are
  kept; 25 frames read clean. With the power save off for the read, reads of
  125 frames (750 bytes) and 128 at 1 MHz are clean.
- Lying still, apparently face up: the part's Z reads +1 g, PebbleOS's
  x = -24, y = 44, z = -1009 mg; noise 1.5 to 2.1 mg rms per axis at 25 Hz,
  low power mode averaging 4 samples. Batches of 25 at 25 Hz interrupt once a
  second, of 125 once every 5 s.
- BMM350: chip ID `0x33`, after the two dummy bytes every read of it starts
  with. Its raw data reads `0x7f` until the OTP has been read and powered off
  and the part reset magnetically. Compensated, still on the desk: about
  x = 96, y = 28, z = -28 uT on the watch's axes (100 uT in all, twice the
  Earth's field: the watch's own hard iron, for the compass's calibration).

### 2.7 Body module (PPG PAH8316LS, MS5837 barometer)

| Pin | Function / AF | Label | Use |
|---|---|---|---|
| PB13 / PB14 | I2C2 SCL / SDA, AF4 | SENSORBODY_SCL / SDA | PPG, barometer, EEPROM |
| PF12 | EXTI12, rising, PD | SENSORBODY_INT1 | PAH8316 interrupt |
| PB11 / PF14 / PG6 | EXTI | SENSORBODY_INT0 / INT2 / INT3 | Unused. INT2 is used by the factory test. |
| PF13 | GPIO out PP, active high | SENSORBODY_RST_L | Really **PPG_EN** (NCP110 LDO), with a board pull-down |
| PG10 | GPIO out PP, active high (SPI3_MISO in the `.ioc`) | SENSORBODY_MISO | Really **VLED_EN** (NCP705 LDO), with a board pull-down |
| PG9 / PD6 / PG12 | SPI3 SCK (AF6) / MOSI (AF5) / CS | SENSORBODY_SCK / MOSI / CS | Unused |

#### 2.7.1 The MS5837 (measured)

With ALTIMETER_EN (PG0) high, the barometer answers at `0x76` on I2C2 (the
PPG at `0x40` stays off, its supply off). Its PROM: C0 `0x7001` (CRC 7, which
checks; version 0, the 02BA01), C1 to C6 `0xa550 0x9ce9 0x6324 0x63c0 0x7d0c
0x672b`; the eighth word, `0xAE`, gets no answer (a bus error). Indoors, at
OSR 4096, three reads gave 102056, 102060 and 102058 Pa.

---

## 3. Buses

### 3.1 Common settings

- **I2C.** Clock source is SYSCLK (`I2c.cpp:557-558`). 7-bit addressing,
  analog filter on, digital filter 0. Interrupt-driven, with no DMA. NVIC
  priority 5.
- **I2C speed per port** (`I2c.cpp:484-495`; `L/Header/Drivers/STM32U5xx/I2c.hpp:172-181`):

  | Ports | Speed | `TIMINGR` |
  |---|---|---|
  | I2C1, I2C2 | 400 kHz | `0x00F07BFF` |
  | I2C3, I2C4 | 1 MHz (Fm+) | `0x00701F6B` |

  The `.ioc` has `0x00701F6B` for all four, but the code overrides it.
- **I2C addresses.** All addresses in the driver code are 7-bit. The driver
  shifts them itself (`I2c.cpp:107,134`).
- **SPI.** Master, 8-bit, MSB first, software NSS. Prescaler /128 on SYSCLK,
  giving **1.25 MHz**. GPDMA1 is used (`L/Source/Drivers/STM32U5xx/Spi.cpp:326-369`).
  The mode is set per device.

### 3.2 Bus and device table

| Bus | Pins | Speed | Devices (7-bit address), chip selects, other lines |
|---|---|---|---|
| **I2C1 "main"** (core board) | PB6 / PB7 | 400 kHz | **PCA9420** PMIC `0x61` (`L/Header/Hardware/PCA9420.hpp:27`) · **MAX17262** fuel gauge `0x36` (`MAX17262.hpp:86`) · **STUSB4500** USB-PD `0x29` (`STUSB4500.hpp:27`) · **DRV2625** haptics `0x5A`, broadcast `0x58` (`DRV2625.hpp:30-31`) · display-module **N24S64B** EEPROM `0x50`, special/ID address `0x58` (`N24S64B.hpp:30-31`). The DRV2625 broadcast address collides with the EEPROM ID address; broadcast is never enabled, so do not enable it. The bus may be held low for up to 250 ms by the STUSB4500 after USB unplug (`L/Source/Application/Instances/InstanceI2C.cpp:60-64`). |
| **I2C2 "body"** | PB13 / PB14 | 400 kHz | **PAH8316LS** PPG `0x40` (`PAH8316LS.hpp:48`) · **MS5837** barometer `0x76` (`MS5837.hpp:26`) · module EEPROM `0x50` / `0x58` |
| **I2C3 "sensor1"** (BLE module) | PC0 / PC1 | 1 MHz | Module EEPROM `0x50` / `0x58` only |
| **I2C4 "sensor2"** (GPS module) | PD12 / PD13 | 1 MHz | Module EEPROM `0x50` / `0x58` · **BMI270** `0x68`, alternate `0x69` (`BMI270.hpp:39-40`) · **BMM350** `0x14` (`BMM350.hpp:34`) |
| **SPI1** | PG2 SCK, PG3 MISO, PB5 MOSI (AF5) | 1.25 MHz, **mode 1** (CPOL 0, CPHA 1) | **BlueNRG-2**: CS PA15 (active low), IRQ PA9 (rising), RESET PA8 (active low, 5 ms pulse then 150 ms boot) (`ComponentFactory.cpp:150-161`; `L/Source/Hardware/BlueNRG2/BlueNRG2.cpp:102-111,426-436`). DMA: GPDMA1 channel 0 for RX, channel 1 for TX. |
| SPI2 | PD1 / PC2 / PC3 | – | Unused. Pins are reused as GPIO (section 2.6). |
| SPI3 | PG9 / PG10 / PD6 | – | Unused. Pins are reused as GPIO (section 2.7). |
| **USART3** | PC4 TX, AF7 | 921600 8N1, TX only | Debug log (section 8) |
| **LPUART1** | PA2 TX / PA3 RX, AF8 | 921600, no flow control, MSIK 4 MHz clock | **AG3335M GNSS**. TX on GPDMA1 channel 10. RX on channel 11 as a circular linked list with idle detection. GNSS_EN is PG5, active high; the kernel waits for `$PAIR001,002` after power-up. There is no reset or wake GPIO (`L/Source/Hardware/AG3335/AirohaBase.cpp:586-599,938-972`). |
| USART1 / USART2 | PA9 / PA10, PA2 / PA3 (AF7) | – | Driver exists, but no instance uses it |
| **SDMMC1** | PC12 CK, PD2 CMD, PC8–PC11 D0–D3 (AF12, PU) | 4-bit bus, `ClockDiv` 0 after init, kernel clock PLL1Q 80 MHz | **eMMC** (4 GB, per the `U5A5-04G` example). Power: EMMC_EN PD10. Hardware flow control on, power-save clock on (`Mmc.cpp:255-268,337-379`). |
| **USB_OTG_HS** | PA11 / PA12, internal HS PHY | HS | Device mode, CDC (VCP) + MSC (`main.h:263-266`). PHY reference is HSE 16 MHz. PebbleOS's `drivers/udc/dwc2.c` is the matching IP (inferred). |

Device header citations in the table are under `L/Header/Hardware/`. BMI270,
BMM350 and PAH8316LS sit in subdirectories of the same name.

### 3.3 eMMC layout (needed for updates)

There is no partition table walk in the kernel: fixed sector offsets of 512 B
sectors are used (`L/Header/Components/flash_diskio.h:22-34`;
`L/Source/Components/flash_diskio.cpp:302-326`). Each region holds a FAT
volume, partition 1 (`L/Source/Components/FileSystem/diskio.c:48-58`).

| Disk | Sector offset | Size | Contents |
|---|---|---|---|
| 0 "UPD" | 0 | 100 MB | `0:`: update files. **The bootloader looks here for `*.ota`, `*.lib` and `*.gld` files.** |
| 1 "SYS" | 204800 | 100 MB | `1:`: system resources, `1:/modinfo_core.bin`, `1:/settings` |
| 2 "GUI" | 409600 | 100 MB | Raw GUI assets (not FAT) |
| 3 "USR" | 614400 | Rest of the device | `2:`: user files, exposed over USB MSC |

The current kernel source never reads or writes GUI: its only mention is an
erase disabled with `#if 0` (`main.cpp:437-442`). PebbleOS keeps its storage
there (see {doc}`index`).

### 3.4 What bootloader 0.1.4 does with the eMMC (confirmed)

Read in the disassembly of `UnaBootloader_0.1.4.hex`. The bootloader reaches
the eMMC only through its copy of `flash_diskio` (`L/Source/Components/flash_diskio.cpp`),
and only drives 0 (UPD) and 1 (SYS) of it: it never touches GUI or USR, and
never erases.

| Address | What it is |
| --- | --- |
| BL@0x08003FAC | `flash_diskio_set_flash()`: stores the eMMC object at `0x20072FAC`. The seven literal-pool references to that address, from BL@0x08003FB4 to BL@0x080041E4, are all inside the diskio functions below: nothing else reaches the eMMC object. |
| BL@0x08003FEC, BL@0x0800408C | `flash_diskio_read()` and `flash_diskio_write()`: refuse a drive above 3, then address byte `(drive × 204800 + sector) × 512` (`mov.w r1, #204800` and `mla` at BL@0x0800403E and BL@0x080040DE). |
| BL@0x08004198 | `flash_diskio_sector_count()`: 204800 sectors for drives 0 to 2, the rest of the device for 3 (`cmp.w r0, #614400` at BL@0x080041B6). |
| BL@0x08003AA0–0x08003B70 | FatFs `disk_status/initialize/read/write/ioctl`: refuse a drive above 1 (`cmp r0, #1; bhi`), then map it through the byte table at `0x0801A240` = `{0, 1}`. FatFs only ever reaches UPD and SYS. `disk_ioctl` handles CTRL_SYNC and the three size queries; CTRL_TRIM (4) is refused. |
| BL@0x080041E8–0x08004228 | The USB mass storage callbacks (status, read, write, block size, block count), registered at BL@0x08004238 through `USBD_STORAGE_config(drv, ...)` (BL@0x08004AEC) with `drv` from byte 7 of its object, set from `r2 = 0` by the call at BL@0x080018EC. The host only sees UPD, of 204800 sectors. |
| BL@0x0801A10C, BL@0x0801A12C | The only drive paths in the image: `1:/settings` (deleted by a factory reset) and `0:`. |
| BL@0x080039A8 | Formatting a drive: drives 0 and 1 only (`cmp r1, #1`), with `MKFS_PARM{FM_EXFAT, 2 FATs, 4096-byte clusters}` as the kernel's `FileSystem::format()`; FatFs keeps it within the 204800 sectors the drive reports. |
| BL@0x08014660–0x0801494C | The SDMMC command helpers linked in: CMD0, 2, 3, 6, 7, 8, 9, 12, 13, 16, 17, 18, 24 and 25. ST's `stm32u5xx_ll_sdmmc.c` has the erase commands (CMD32, 33, 35, 36, 38) between CMD25 and CMD12; here CMD25 (BL@0x08014734) is followed by CMD12 (BL@0x08014768), and `SDMMC_MAXERASETIMEOUT` (63000) appears nowhere: the linker dropped them, so the bootloader cannot erase. |

The one literal 409600 (`0x00064000` at `0x0801A0A4`) is a length in a table of
RAM addresses (`0x20000498`, next to clock rates at `0x0801A0B4`), not a sector.

This matters beyond the bootloader: the stock `Mmc::sectorErase()` passes byte
addresses to `HAL_MMC_Erase()`, which takes block numbers on a card addressed
in sectors (`L/Source/Drivers/STM32U5xx/Mmc.cpp:115-149`), so an erase of
sector `n` would erase sector `n × 512`: sectors 800 to 1199 of UPD would land
in GUI. The bootloader does not have that code, and the kernel only calls it
from the disabled block above.

---

## 4. Power

### 4.1 PMIC rails (PCA9420, I2C1 `0x61`)

The rail voltages are set by `main.h:255-259`, overriding the defaults at
`L/Source/Components/Pmic.cpp:20-38`. They are written into all four PMIC
modes (`Pmic.cpp:146-196`). Voltage encoding is at
`L/Source/Hardware/PCA9420.cpp:672-718`.

| Rail | Voltage | Probable load (inferred from names and levels) |
|---|---|---|
| SW1 | 1.20 V | Core or eMMC VCCQ (unclear) |
| SW2 | 1.80 V | MCU VDD / 1.8 V I/O domain, hence "PWR_ON_**1V8**_L" |
| LDO1 | 1.80 V | Always-on. The code calls it "RTC" and it is the only rail kept in Mode 2 (likely MCU VBAT). |
| LDO2 | 3.20 V | 3 V peripherals, likely the display VDD and eMMC VCC |

**PMIC mode configuration:**

- **Modes 0, 1 and 3:** all four rails on, ON_CFG = 1, pin mode-select off.
- **Mode 2 ("off"):** LDO1 only, pin mode-select on (`Pmic.cpp:146-196`).

**There are no GPIO-controlled main rails.** The display, I2C devices and MCU
are on whenever the PMIC is in Mode 0, which is the power-on state. Peripheral
power instead comes from GPIO-enabled load switches and LDOs that the kernel
turns on on demand:

| Load | Enable pin | Active level | Timing |
|---|---|---|---|
| eMMC | PD10 EMMC_EN | High | 35 ms after power-on. Auto-off after 2 s idle unless USB is attached. |
| Barometer LDO (AP7350) | PG0 ALTIMETER_EN | High | Off for 20 ms, then on and wait 10 ms (`L/Source/Components/External/Barometer/BaroMs5837.cpp:554-558`) |
| PPG supply (NCP110) | PF13 | High | Wait 10 ms; keep off at least 20 ms before re-enabling |
| PPG LED supply (NCP705) | PG10 | High | Same timing as the PPG supply |
| GNSS | PG5 | High | Wait for the PAIR ack |
| BLE | PA8 (reset, not power) | Low = reset | 5 ms low, then 150 ms |
| Haptics | PF2 | High | 2 ms |
| Backlight | PF3 | **Low** = on | – |

### 4.2 Boot-time sequence, as done by the stock kernel

**In `main()`:**

1. Enable the SMPS and VddIO2 (`main.cpp:664-672`).
2. Set up the clocks (section 1.5).
3. Start the debug UART.
4. Start IWDG at 30 s.
5. Run `PinsDefaultConfig` (`main.cpp:674-807`). It drives eMMC, haptic,
   buzzer, DAM, barometer, BLE-reset and GNSS-enable all low (off), sets the
   buttons to input with pull-up, and **turns the backlight on** (PF3 low).

**In `StartupTask` (`main.cpp:376-658`):**

1. **PCA9420 init** (`Pmic.cpp:58-212`).
   - It may enter ship mode at once if the battery was just connected with no
     VIN (`Pmic.cpp:82-88,214-250`).
   - It configures the charger: 150 mA fast charge, 4.40 V VBAT_REG, 10 mA
     top-off, NTC off, VIN ILIM about 255 mA, VIN UVLO 3.3 V.
   - It sets ON-key long-press = 16 s (TOP_CNTL3).
   - It programs the rail modes.
   - It unmasks the VIN and CHG_OK interrupts.

   None of this is needed just to light the display, since the rails come up
   at reset defaults. It is needed for charging (inferred from the reset-state
   behaviour of the PCA9420).
2. USB-protection init (STUSB4500 alert mask, NX20P fault EXTI).
3. eMMC power-up and FatFs mount.
4. MAX17262 (EZ model: DesignCap 200 mAh, IChgTerm 14 mA, VEmpty 3.40 / 3.96 V).
5. USB device.
6. Power manager.
7. **Buttons.**
8. Buzzer.
9. DRV2625.
10. **Module scan.** EEPROM probes; the display is created only if the
    view-module EEPROM at I2C1 `0x50` answers.
11. **Backlight on.**
12. **LCD `init()` + `enable()`** (starts VCOM).
13. BLE.
14. GUI.

The minimum for the display is the clocks, the GPIO setup and VCOM. No
PMIC writes are needed (inferred: rails at the PCA9420 reset default are what
the bootloader also runs on).

### 4.3 Power off

| Mechanism | Detail |
|---|---|
| User / low-battery power-off | `Power::hPowerOff` (`Power.cpp:548-577`): wait for PD15 release, set RTC_BKP_DR3 = 1, flush logs, then `Pmic::powerOff()` = `setMode(MODE_2)`. That writes TOP_CNTL3 (`0x0C`) with mode-select bits so the PMIC drops to **Mode 2, LDO1 only** (`Pmic.cpp:288-291`; `PCA9420.cpp:120-157`), retrying in a loop. |
| Wake from "off" | Mode 2 has ON_CFG = 1, so a press of the ON key returns the PMIC to Mode 0. The rails come up, the MCU cold-boots, and the bootloader sees DR3 = 1 and demands a 500 ms hold. (Inferred: PD15 / SW2 is also wired to the PCA9420 ON pin, hence the name; the name and the bootloader behaviour both point this way.) |
| Ship mode | MODECFG_0_0 (`0x22`) bit 7 SHIP_EN (`PCA9420.cpp:103-118`). Only used on first battery connect. Wake is by USB or the ON key. |
| MCU STANDBY / SHUTDOWN | Not used. Idle is STOP2 only. |
| Reboot | `NVIC_SystemReset()` (`System.cpp:210-213`) |

### 4.4 Battery and charging

- **Battery:** 200 mAh, charged to 4.40 V at 150 mA by the stock firmware
  (`main.h:238-253`). Its data sheet is not in the sources; a comment in
  `Pmic.cpp:104` speaks of a 280 mAh cell with a 0.01C cut-off.
- **Charger:** the PCA9420's linear charger, CC/CV up to 315 mA. Confirmed
  from the PCA9420 data sheet (Rev. 3.1, section 9.5):
  - CHG_CNTL0 (`0x10`): CHG_EN bit 0, CHG_TIMER_EN bit 1, NTC_EN bit 2,
    CHG_LOCK bits 7:3. CHG_CNTL1 to CHG_CNTL6 only take writes while
    CHG_LOCK = `10101b` (`0xA8` with the other bits clear).
  - ICHG_CC (`0x11`) 5 mA steps, 0 to 315 mA; ICHG_TOPOFF (`0x12`),
    ICHG_LOW (`0x13`) and ICHG_DEAD (`0x14`) 1 mA steps; VBAT_REG (`0x15`)
    3.60 to 4.60 V in 20 mV steps, accurate to 0.75%, with VBAT_RESTART
    (140 or 240 mV) in bit 6; CHG_CNTL6 (`0x16`) the top-off, pre-qualification
    and fast charge timers; CHG_CNTL7 (`0x17`) THM_REG, the die temperature
    the charge current folds back from. The defaults of most are
    MTP-programmable.
  - CHG_STATUS_0 (`0x18`) bit 5 IN_PWR_OK; CHG_STATUS_2 (`0x1A`) the charge
    state in bits 2:0 (idle, dead, low, fast, top-off, done), valid only with
    input power.
  - VBAT_LOW 2.5 V and VBAT_DEAD 1.9 V are fixed. A watchdog can stop
    charging, but resets the system when it expires.
  - The stock firmware turns NTC_EN off, so the TS pin has no thermistor
    (inferred): the PMIC cannot check the battery temperature itself.
  - TOP_CNTL1 (`0x0A`) bits 5:4, ASYS_INPUT_SEL (data sheet Rev. 4.2,
    section 8.3 and table 18): 00 ASYS from VIN or VBAT, whichever is
    higher (the default), 01 from VBAT only, 10 from VIN only, 11
    disconnected (test only). ASYS feeds the bucks, LDO2, LDO1 (from the
    higher of ASYS and VBAT_BKUP) and the linear charger, so 01 runs the
    whole watch from the battery with VIN connected, and nothing charges.
    Only a PMIC power cycle or reset restores 00; the bootloader does not
    write it. Confirmed on a watch, where TOP_CNTL1 reads `0x82` (VIN UVLO
    3.3 V, as the stock firmware sets it): `0x92` made the gauge read the
    system's draw (15 mA with the USB connected and working) instead of a
    charge, and `0x82` restored the charge. VIN_ILIM has no suspend setting
    (85 mA at least).
- **MAX17262** at `0x36`, internal sense resistor (0.5 mAh, 156.25 uA,
  78.125 uV and 1/256 degree C a bit), in EZ mode: DesignCap `0x0190`,
  IChgTerm `0x0059`, VEmpty `0xAA63`, FullSOCThr `0x5005`, SOCHold `0x1000`,
  ModelCFG `0x0400` (4.4 V cell) (`L/Source/Components/FuelGauge.cpp:66-118`,
  `MAX17262.hpp:261-333`). Its temperature is its own die's. Its ALRT (PC6)
  is unused.
- **STUSB4500** at `0x29`:
  - Runs on its factory NVM; no PDO programming is done.
  - Used for attach, debug-accessory (DAM) and fault detection.
  - Powered from VBUS: it dies at unplug, and read at that moment it can hold
    I2C1 low (`Power.cpp:199-206`).
  - Its ALERT shares PG7 with the PMIC interrupt, so the line can stay low
    while USB is in.
- **NX20P** USB protection switch: fault on PF0.

PebbleOS drives the charger and the gauge as {doc}`index` describes, and
leaves the STUSB4500 alone.

### 4.5 Measured draw

From the battery, on the MAX17262, with ASYS from VBAT only (section 4.4),
PebbleOS idle on a minute-tick face, the USB off: 6 mA with the CPU in Sleep
on PLL1 at 160 MHz, 1.0 mA with it in Stop 2, which leaves about 1 mA to the
board: the PMIC, the panel and its VCOM, the modules in reset or standby,
the eMMC off. The USB OTG HS with its PHY and the HSE adds about 11 mA. The
tethered stub, polling with the USB at high speed and the frontlight on,
draws 31.5 mA. The GNSS receiver searching adds about 9 mA (section
2.6.1). The figures and how they were taken are in {doc}`index`
(Power). With the input normal and charging off, the battery current swings
by several milliamps either way, so the gauge's offset could not be read
on the watch plugged in.

---

## 5. Display

### 5.1 Panel

| Property | Value | Source / confidence |
|---|---|---|
| Part | **Sharp LS012B7DD06A**. The driver class and log prefix use this name; comments say "LPM012M387D" (JDI). The module-info example is `-module view -name LS012`. | `L/Source/Hardware/LS012B7DD06A.cpp:4`; `L/Header/Hardware/LS012B7DD06A.hpp:69-96`; `UtilModuleInfo.cpp:27`. Which vendor is actually fitted is not settled; the signal names are given in both JDI and Sharp forms. |
| Technology | Reflective colour **memory-in-pixel**, with a front/back light on PF3 | Driver comments, `Backlight.cpp` |
| Resolution | **240 × 240** | `LS012B7DD06A.hpp:136-137`; `main.h:322-323`; TouchGFX project `"Resolution": 240×240` |
| Colour depth on the wire | **6 bpp**: 2 bits per R, G and B, so 64 colours | `getDisplayColorDepth()` returns 6 (`LS012B7DD06A.hpp:121-124`) |
| Frame buffer format | 8 bpp, 1 byte per pixel, **ABGR2222**: bits [1:0] = R, [3:2] = G, [5:4] = B, [7:6] = alpha (ignored by the panel) | TouchGFX `LCD8bpp_ABGR2222` (`UnaWatchGUI.touchgfx`); the bit extraction is analysed in section 5.3 |
| Shape | **Round (inferred).** Nothing in the driver says so: it clocks a full 240 × 240 square. But the UI is laid out for a circle: concentric arc widgets with radius 107 at (116, 116), inside 240 px containers; circular scroll-rail arcs; a 147 × 147 round watch dial; `SideBarBig::setCircular(true)` (`TouchGFX-GUI/generated/gui_generated/src/containers/ActivityBarTopBase.cpp:13-24`; `gui/include/gui/containers/ScrollIndicator.hpp:24-67`). Confirm from the datasheet or by looking at the watch. If it is round, the corner pixels are clocked but not visible. | Inferred |
| Orientation | The panel is mounted **a quarter turn round**. The driver rotates each frame before sending (see 5.4). | `LS012B7DD06A.cpp:23-49` |
| Interface | **Parallel JDI / Sharp MIP interface, bit-banged on GPIO.** It is **not** SPI and not the Sharp serial memory-LCD protocol. | Whole driver |

### 5.2 Signals

| Signal | JDI / Sharp name | Pin | Meaning (`LS012B7DD06A.hpp:46-59`) |
|---|---|---|---|
| VST | GSP | PB10 | Vertical (gate) start pulse |
| VCK | GCK | PA4 | Vertical shift clock: two edges per line |
| ENB | GEN | PA7 | Pixel-memory write enable, one pulse per half-line |
| XRST | INTB | PB1 | Driver reset / initialise. High for the duration of a frame. |
| HST | BSP | PB0 | Horizontal (source) start pulse |
| HCK | BCK | PA6 | Horizontal shift clock; data on both edges |
| R1 G1 B1 | | PE0 PE2 PE4 | Data for pixel 2k |
| R2 G2 B2 | | PE8 PE10 PE12 | Data for pixel 2k+1 |
| FRP | VB, = VCOM | PB2 (LPTIM1_CH1 AF1) | Liquid-crystal drive / VCOM ("off" pixel) |
| XFRP | VA | PD14 (LPTIM3_CH1 AF14) | Inverse of FRP ("on" pixel) |
| VDD1 / VDD2 | | – | 3.3 V / 5 V on the Nucleo test setup (`LS012B7DD06A.cpp:72-96`). On the watch they come from PMIC rails with no enable GPIO (inferred). |

### 5.3 Line-update protocol (confirmed, `LS012B7DD06A.cpp:290-396`)

Every update rewrites the whole frame, all 240 lines; there is no partial or
windowed update. The frame is clocked with the scheduler locked
(`osKernelLock()`), at optimisation `-O0`, using `BSRR`/`BRR` writes and
**`LL_GPIO_WriteOutputPort(GPIOE, …)`**.

That last call writes the **whole** `GPIOE->ODR`, which is why every other PE
pin must be unused (inputs with pull-up).

**Frame:**

```
INTB=1; wait 11 µs                     (thsINTB)
GSP=1;  wait 11 µs                     (thsGSP)
GCK=1;  wait 69 µs                     (GCK edge 1, thwGCK)
GCK=0;  send line 0, MSB plane         (GCK edge 2)
GSP=0;  wait 11 µs                     (tlsGSP)
GCK=1;  send line 0, LSB plane         (GCK edge 3)
for line = 1 .. 239:
    GCK=0; send line, MSB plane
    GCK=1; send line, LSB plane
GCK=0; wait 69 µs; GCK=1; wait 69 µs; GCK=0     (GCK edges 482, 483, idle)
INTB=0
```

**Half-line ("plane") transfer, `write_xSB(line, 240, msb)`:**

```
BSP=1; BCK=1; wait 1 µs; BCK=0; BSP=0          (BCK edges 1-2; the 1 µs keeps the first pixels from being lost)
repeat 60x:                                     (BCK edges 3..122, two pixels per edge)
    GPIOE->ODR = halfword(px[2k], px[2k+1]) >> (msb ? 1 : 0); BCK=1
    GPIOE->ODR = halfword(px[2k+2], px[2k+3]) >> (msb ? 1 : 0); BCK=0
BCK=1 (edge 123); GEN=1; BCK=0 (idle)
wait 10 µs (twhGEN); GEN=0; wait 5 µs (thhGCK)
```

**Bit placement.** The line is read as little-endian `uint16_t`, so pixel 2k
is the low byte and pixel 2k+1 the high byte.

- **MSB pass:** shifting right by 1 puts bit 1 (R MSB) of pixel 2k on PE0,
  bit 3 (G MSB) on PE2, bit 5 (B MSB) on PE4. Bits 9, 11 and 13 (the R, G and
  B MSBs of pixel 2k+1) land on PE8, PE10 and PE12.
- **LSB pass:** with no shift, bits 0, 2 and 4 of each pixel land on the same
  pins.

So each half-GCK period carries one bit-plane: the MSB plane while GCK is
low, the LSB plane while GCK is high. That is the standard JDI 2-bit-per-colour
parallel scheme.

**Timings.** The comments give the datasheet minimums: 68.89 µs per GCK half
period, tsRGB / thRGB 220 ns, and BCK pulse 440 ns. The code relies on `-O0`
GPIO code at 160 MHz to meet the 220 ns setup/hold, and on the
`write_xSB` duration plus the explicit waits to meet 68.89 µs.

**Frame time.** That works out to at least 483 × 69 µs ≈ **33 ms per frame**,
which is roughly the minimum practical frame time (inferred).

### 5.4 Rotation

`RotateQuarterTurn` computes `dst[(239 − x) * 240 + y] = src[y * 240 + x]`
(`LS012B7DD06A.cpp:42-49`, enabled by `PANEL_MOUNTED_ROTATED 1`).

This means panel line `L`, pixel `P` shows upright-image pixel
`(x = 239 − L, y = P)`. In other words, the wire frame is the upright image
**rotated 90° counter-clockwise**: the right-hand column of the image is sent
first, as line 0, from top to bottom.

### 5.5 VCOM (FRP / XFRP)

Both signals are LPTIM PWM, clocked from **LSE** (the code comment says LSI,
but `Lptim1ClockSelection` and `Lptim34ClockSelection` are LSE). Code at
`LS012B7DD06A.cpp:467-580`:

| Parameter | Value |
|---|---|
| ARR | `32000 / 30` = 1066 |
| Duty | 50% (`Pulse` = 533) |
| Resulting frequency | 32768 / 1067 ≈ **30.7 Hz** |
| FRP | LPTIM1 CH1, `OCPOLARITY_HIGH` |
| XFRP | LPTIM3 CH1, `OCPOLARITY_LOW`, so it is the exact complement of FRP |

- **Low-power operation.** `__HAL_RCC_LPTIMx_CLKAM_ENABLE()` keeps both timers
  running in STOP2, so the image is retained with the CPU asleep.
- **Enable** (`LS012B7DD06A.cpp:238-264`) resets both counters together so the
  two outputs stay in phase, removes the pull-downs, and starts both PWMs.
- **Disable** (`LS012B7DD06A.cpp:266-283`) stops them and pulls both pins
  down.
- There is no separate EXTCOMIN or VCOM serial bit: the panel's VCOM is driven
  directly by FRP.

### 5.6 Backlight / frontlight

- A single GPIO: PF3 `BACKLIGHT_ON`, **open-drain, active low** (`L/Source/Components/External/Backlight.cpp:16-89`).
- There is no PWM or dimming.
- It draws about 8.2 mA (`main.cpp:635`).
- It is turned on at boot and left on (`main.cpp:616`).

### 5.7 Driver flow

1. `ComponentFactory::scanModules()` detects the display module by its EEPROM
   on I2C1 (`ComponentFactory.cpp:66-114`).
2. `init()` configures LPTIM1 and LPTIM3 (stopped) and the GPIO, and starts
   the DWT counter (`LS012B7DD06A.cpp:196-220`).
3. `enable()` starts VCOM.
4. TouchGFX renders into an 8 bpp buffer.
5. Each `write(buf)` rotates the buffer into a 57.6 KB static scratch buffer,
   then clocks the whole frame.
6. `disable()` stops VCOM.

The panel never needs a "clear" command: writing a frame of zeros clears it.

### 5.8 Can PebbleOS's existing drivers drive it?

**`drivers/display/sharp_memory`: no.**

- It implements the Sharp *serial* memory-LCD protocol: SPI, mode byte `0x01`
  or `0x04`, an 8-bit line address, 1 bpp lines, an EXTCOMIN pulse PWM at
  1/20 duty, and an enable GPIO (`unapebble/drivers/display/sharp_memory/sharp_memory.c:12-127`).
- Its compatible list holds only 1-bit panels. `sharp,ls012b7dd01` there is a
  184×38 strip (`sharp_memory.dtmap.yaml:5-42`).
- The UNA panel has no SPI, no line address and no command bytes, and it uses
  6 bpp.

**`drivers/panel/jdi_mip.c` with `jdi_lpm015m135a.dtmap.yaml` or
`sharp_ls013b7dd02.dtmap.yaml`: not with config changes alone, but this is the
right family.**

These describe exactly this kind of panel: a JDI / Sharp parallel 2-bit RGB
MIP. `sharp,ls013b7dd02` is the 260×260 round panel of the Pebble Round 2
(`unapebble/boards/getafix/getafix.dtsi:45-71`).

`jdi_mip.c` is only the *panel* half. It handles power sequencing of two
regulators (`vlcd`, then `vddp`, 11 ms apart, with at least 500 ms off-time)
and runs one 50% VCOM PWM (`unapebble/drivers/panel/jdi_mip.c:12-115`). The
timing parameters (`panel-timing`, `jdi,enable-start` / `jdi,enable-end`) are
consumed by the **SiFli LCDC hardware JDI-parallel engine**, which works in
RGB332 (`unapebble/drivers/display/sf32lb/lcdc.c:60-125`).

**What differs on UNA:**

1. **No display controller.** The STM32U5's LTDC cannot generate the
   GSP/GCK/BSP/BCK/GEN/INTB waveform with bit-planes split across GCK phases.
   A new `pbl_display` driver for STM32 is needed. It could bit-bang as the
   stock kernel does, or better use a timer-paced GPDMA writing precomputed
   words to `GPIOE->ODR`/`BSRR` and to the control pins' `BSRR`. That last
   option is an inferred design suggestion.
2. **VCOM needs two complementary outputs** on two different LPTIMs (LPTIM1
   CH1 on PB2, LPTIM3 CH1 on PD14) at about 30 Hz. `jdi_mip.c` drives a
   single PWM. PebbleOS has `drivers/timer/stm32_lptim.c` but no STM32 LPTIM
   PWM driver.
3. **No panel supply GPIOs.** `vddp` and `vlcd` would have to be fixed or
   dummy regulators. The power-on sequence (11 ms, 500 ms off-time) can stay
   harmlessly.
4. **Pixel format.** PebbleOS framebuffers are `GColor8`, ARGB2222: red in
   bits [5:4], blue in [1:0]. The UNA wiring expects R in [1:0] and B in
   [5:4], so R and B must be swapped. A 256-entry lookup table merged into the
   rotation pass works.
5. **Rotation by 90°**, done in software or in the DMA descriptor generation.
   `sharp_memory`'s `rotation` only supports 180°.
6. **Geometry.** Not 260×260 or 200×228, but 240×240. If the panel is round,
   a round mask for 240 px is also needed, like
   `pbl_panel_ls013b7dd02_round_mask` (`unapebble/include/pbl/drivers/panel/ls013b7dd02.h`);
   the UNA source does not provide one.

---

## 6. Buttons

- **Wiring.** Four buttons, all **active low**, each with an RC filter. The
  MCU uses internal pull-ups and EXTI on both edges at NVIC priority 5
  (`L/Source/Components/HWButtons.cpp:220-246`).
- **Pre-charge.** At init each pin is briefly driven high as a push-pull
  output to charge the filter capacitor, then switched to an input
  (`HWButtons.cpp:234-239`).
- **Debounce.** 50 ms in software, on both edges (`L/Header/Components/HWButtons.hpp:146`).
- **Semantics.** Mapping from `L/Header/Interfaces/Components/IButtons.hpp:25-40`;
  order passed to the driver at `main.cpp:582-586`:

| Id | Pin | Position | UNA code | Semantics |
|---|---|---|---|---|
| SW1 | PD4 | Top left | L1 | **UP** |
| SW2 | PD15 `PWR_ON_1V8_L` | Top right | R1 | **SELECT**, and the power button |
| SW3 | PA5 | Bottom left | L2 | **DOWN** |
| SW4 | PD8 | Bottom right | R2 | **BACK** |

**Mapping onto Pebble's physical layout.** Pebble places BACK on the left
and UP / SELECT / DOWN on the right. Here BACK is bottom right, SELECT top
right, and UP and DOWN on the left. A PebbleOS `gpio-keys` node should map:

| Key | Pin |
|---|---|
| back | PD8 |
| up | PD4 |
| select | PD15 |
| down | PA5 |

All are `GPIO_ACTIVE_LOW` with pull-up.

**Event thresholds.**

| Event | Timing |
|---|---|
| Click | Under 500 ms |
| Long press | 1–5 s |
| Hold events | 1 s, 5 s, 10 s |

The power-off long-press timing is handled by the Power component.

**Wake from STOP2.** There is no extra configuration. EXTI edge interrupts
stay active in STOP2, so any button press wakes the MCU. The kernel uses no
WKUP pins and no STANDBY mode.

**Power button.** SW2 / PD15 is also the PMIC ON key (inferred from its name;
see 4.3). PMIC long-press reset is set to 16 s (TOP_CNTL3, `Pmic.cpp:97`).
Holding it for 4 s at power-on enters the bootloader menu (README.md:56).

---

## 7. Haptics, buzzer, LEDs

### 7.1 DRV2625 (I2C1 `0x5A`)

**Pins.** EN/NRST is **PF2 `HAPTIC_RST_N`**, push-pull, active high. No
trigger or IN pin is used: the TRIG pin is configured as "use the GO bit".

**Init and probe** (`L/Source/Components/Vibro.cpp:57-94`;
`L/Source/Hardware/DRV2625.cpp:98-149`):

1. Drive PF2 high and wait 2 ms.
2. Check CHIPID = 1 and REV = 3.
3. Run diagnostics: MODE = 2, then GO, then poll register `0x01`.
4. Drive PF2 low.

**Enable** (`Vibro.cpp:137-194`). PF2 is driven high and **stays high**.
Then:

1. **Actuator: ERM** (`0x08` LRA_ERM = 0). `RATED_VOLTAGE` `0x1F` = 123
   (2.7 V), `OD_CLAMP` `0x20` = 137 (2.95 V), FB_BRAKE / LOOP_GAIN in `0x23`,
   AUTO_BRK_INTO_STBY.
2. **Calibration.** Restore from RTC_BKP_DR5, or run auto-calibration (MODE = 3,
   up to 5 s) and save the results to DR5 (`DRV2625.cpp:151-211`;
   `Vibro.cpp:22-40,344-367`).
3. **MODE = 1** (waveform sequencer) and LIB_SEL = 1 (ERM ROM library).

**Play** (`Vibro.cpp:242-321`):

1. Write effect IDs to `0x0F`–`0x16`. A value with bit 7 set means a pause in
   10 ms units.
2. Write loop counts to `0x17`–`0x19`.
3. Write `GO` = 1 to `0x0C`. Writing 0 stops; reading `0x0C` gives busy.

**Differences from PebbleOS `drivers/vibe/drv2604.c`.** The register maps are
incompatible, so a new driver is needed:

| | DRV2625 (UNA) | DRV2604 (PebbleOS) |
|---|---|---|
| MODE register | `0x07` [1:0] | `0x01` [2:0], STANDBY at bit 6 |
| RTP input register | `0x0E` | `0x02` |
| Rated voltage / clamp | `0x1F` / `0x20` | `0x16` / `0x17` |
| Feedback / calibration | `0x08`, `0x21`–`0x23` | `0x1A` single register, `0x18` / `0x19` |
| Actuator | ERM | LRA (`drv2604.c:109-110`) |
| Drive method | ROM-library sequencer started with GO | RTP with signed strength (`drv2604.c:140-155`) |
| Enable pin | Held high while enabled | Toggled around every start and stop (`drv2604.c:157-167`) |
| Calibration persistence | Persisted to an RTC backup register | Not persisted |

For an MVP, a DRV2625 in RTP mode works: MODE = 0 in `0x07`, ERM in `0x08`,
the rated and clamp voltages above, and the amplitude in `0x0E`. That is the
smallest driver (inferred from the TI DRV2625 register map, not from UNA
code).

### 7.2 Buzzer

- **PWM:** TIM4 CH3 on PB8, AF2 (`L/Source/Drivers/STM32U5xx/Pwm.cpp:124-125,197-214`).
- **Enables:** PB9 `BUZZER_EN1` and PD5 `BUZZER_EN2`, push-pull (`main.cpp:592-599`).

### 7.3 LEDs

There are no indicator LEDs and no LED driver. The only "light" is the
backlight on PF3 (section 5.6). The PPG LEDs are powered through VLED_EN on
PG10.

---

## 8. Debug

### 8.1 SWD

| Signal | Pin |
|---|---|
| SWDIO | PA13 |
| SWCLK | PA14 |
| SWO | PB3, configured as asynchronous trace in the `.ioc` |

NRST is not described in the source.

How exposed the pins are on a production watch is unknown. The kernel mentions
a "debug jumper" drawing 270 µA (`main.cpp:634`), which suggests a debug
connector on the core board (inferred).

Flash the whole-image `.hex` (bootloader + libc + kernel) over SWD to recover
a watch.

### 8.2 Early console

- **Port:** **USART3 TX on PC4, AF7.** 921600 baud, 8N1, TX only,
  oversampling 16, kernel clock HSI16 (`main.h:108-118`;
  `L/Header/Drivers/STM32U5xx/LogUart.hpp:115-139`).
- **Polled raw writer.** The kernel brings it up immediately after the clock
  init (`main.cpp:176`). It also uses a polled, HAL-free `WriteRaw()` for
  fault and IRQ contexts (`LogUart.hpp:76`). That is a good model for a
  PebbleOS early console; PebbleOS already has `drivers/uart/stm32.c` (H7).
- **Physical access.** On the watch the debug TX reaches the USB-C connector
  through a switch enabled by **PB15 `USB_DAM`**, when the STUSB4500 reports a
  Type-C **Debug Accessory (DAM)** attach (`L/Source/Components/Power.cpp:355-379`;
  `main.cpp:291-312`).
  - (Inferred) A Type-C debug-accessory adapter (Rd/Rd on both CC lines) with
    a 1.8 V UART on SBU or D± will show the log.
  - To get the console without the PD chip logic, a port could simply drive
    PB15 high once early in boot.
  - (Inferred) Which connector pins carry TX is not in the source.

---

## 9. Firmware packaging and the bootloader's acceptance rules

This section was verified by running the repo's own packer on a test input and
decrypting the result with `openssl enc -d -aes-256-cbc -nopad`. All three
CRCs, the lengths and the plaintext matched.

### 9.1 Build outputs

1. **CubeIDE build.** It links the ELF and objcopies `.hex` and `.bin`
   (`CUBE/.cproject:28-29`).
   - The `.bin` is the raw flash image, starting at `0x08060000`, with the
     vector table first.
   - **The kernel image has no header, magic or CRC section of its own.**
2. **Post-build step** (`CUBE/.cproject:17`):

   ```
   python firmware_packer/post_build.py -n <artifact> -v <version> -d <cfgdir> -o <Exe> -m 2
   ```

   It is in `Software/Utilities/Scripts/firmware_packer/post_build.py`. The
   version comes from `git describe` and is reformatted as, for example,
   `0.1.8-rc1-7-4a72370-dirty`
   (`Software/Utilities/Scripts/cube-kernel-version/cube-kernel-version.py:40-50`).

   | Output | What it is | How it is made |
   |---|---|---|
   | `<name>_<ver>.hex` | Bootloader + libc + kernel merged, for SWD | `post_build.py:175-180,288-295` |
   | `.ota` | Kernel update | `pack(app.bin, key, type=1)` (`post_build.py:369-381`) |
   | `.gld` | Factory "golden" image: the same `.bin`, used by factory reset | `type=2` (`post_build.py:385-397`) |
   | `.lib` | libc + kernel as one binary based at `0x08028000`; only with `--inclibc` | `type=4` (`post_build.py:182-186,343-365`) |

3. **SDK scripts.** `SDK/Utilities/Scripts/build-cube/` holds `find-cube.sh`,
   `una-version.sh` and `build-cube.sh`, but is **unavailable** because the
   submodule is empty. From how they are called, they are headless-CubeIDE
   build wrappers and version helpers (README.md:28-36). Packaging does not
   depend on them.

### 9.2 `.ota` / `.gld` / `.lib` file layout (confirmed)

The file is a 64-byte plaintext header followed by the encrypted payload. All
fields are **little-endian**. The total file size must be a multiple of 16
(BL@0x080020A2).

| Offset | Size | Field | Value / computation |
|---|---|---|---|
| `0x00` | u32 | Format version | 1 (`encrypt_firmware.py:11`). Not checked by the bootloader. |
| `0x04` | u32 | `enc_crc` | CRC32 of the encrypted payload (bytes 64…end) |
| `0x08` | u32 | `enc_len` | Length of the encrypted payload = 16 + padded bin length |
| `0x0C` | u32 | `bin_crc` | CRC32 of the padded plaintext bin. The 16-byte subheader is not included. |
| `0x10` | u8 | User-data version | 1 (`post_build.py:337`) |
| `0x11` | u8 | **Image type** | 1 = `.ota`, 2 = `.gld`, 4 = `.lib` (`post_build.py:338-340`) |
| `0x12` | u8 × 2 | – | 0 |
| `0x14` | u8 × 4 | Firmware version | patch, minor, major, 0. Not checked. |
| `0x18` | char[23] | Version suffix | `rcN` + commit, NUL-padded. Not checked. |
| `0x2F` | u8 | Terminator | 0 |
| `0x30` | u8 × 4, read as u32 | Required libc version | patch, minor, major, 0. **0 means "don't check".** |
| `0x34` | 8 | Reserved | 0 |
| `0x3C` | u32 | Header CRC | CRC32 of bytes `0x00`–`0x3B` (`encrypt_firmware.py:96-99`) |

The user-data struct is `'=BBBBBBBB23sBBBBB8s'` (`post_build.py:369-374`).

**Encrypted payload, once decrypted** (`encrypt_firmware.py:16-46`):

1. A 16-byte subheader `{u32 bin_len_padded, u32 bin_crc, u32 0, u32 0}`.
2. The bin, zero-padded to a multiple of 16.
3. If `(padded_len − 16) % 512 == 0`, **16 more zero bytes**. This works
   around a bug in bootloaders 0.1.1 and earlier. These bytes are included in
   `bin_len` and `bin_crc`.

**CRC.** CRC-32/IEEE as in zlib: reflected polynomial `0xEDB88320`, init
`0xFFFFFFFF`, final XOR `0xFFFFFFFF`. Python's `zlib.crc32` matches. The
bootloader computes it in software at BL@0x08002668. There is **no signature,
MAC or hash**.

### 9.3 Encryption (confirmed)

- **Algorithm:** AES-256-CBC over the subheader plus the padded bin, with no
  padding scheme beyond the zero-fill (`encrypt_firmware.py:73-88`;
  `aespython/cbc_mode.py:29-32`). The 64-byte header is in clear.
- **Key and IV:** one fixed key and IV for all images and all watches
  (README.md:15).
- **Key file:** `una-bootloader/Software/Keys/SecurityKeys.pem` is not a real
  PEM key. It has two armoured blocks:
  - `-----BEGIN KEY-----` holds one base64 line: the 32-byte AES key.
  - `-----BEGIN IV KEY-----` holds one base64 line: the 16-byte IV.

  `getKeys()` base64-decodes the line after each marker with no derivation
  (`encrypt_firmware.py:51-68`). Watch out for two traps:
  - The matching is exact, so CRLF line endings break it.
  - A wrong-length IV is silently replaced by zeros (`aespython/cipher_mode.py:24-26`).
- **Bootloader copy:** the same key and IV are stored in clear in the
  bootloader (IV at `0x0801A12F`, key at `0x0801A13F`). They are passed to
  what looks like ST's CMOX library (BL@0x08001ED8; CMOX is inferred from the
  `0x10000` success code).

### 9.4 What bootloader 0.1.4 does with an image

**Update search (BL@0x08002310).** It runs only if the battery is above about
10% (BL@0x08001630; `Backend.cpp:2566-2573`). It takes the first file on drive
`0:` (the eMMC UPD volume) whose name ends in `.ota`, otherwise `.lib`. The
match is a **case-sensitive** check of the last four characters.

**Validation (BL@0x08002050, all confirmed):**

1. The file size is a multiple of 16.
2. The header CRC at `0x3C` matches.
3. `hdr[0x11]` is the expected type.
4. For types 1 and 2, the libc version at `0x30` is either 0 or equal to the
   installed `sl.x.y.z` (BL@0x08002108).
5. The first decrypted block (the subheader) has `crc == bin_crc` and
   `len == enc_len − 16`.
6. The total ciphertext length equals `enc_len`, and its CRC equals `enc_crc`
   (BL@0x080027CE).
7. `bin_len ≤ flash_size − 0x60000`. Type 4 gets an extra `0x40000`.

The version and suffix are **never checked**, so downgrades are allowed
(README.md:62).

**Install (BL@0x08001EEC):**

1. Erase 8 KB pages from the destination to the end of flash. The destination
   is `0x08060000` (`0x08028000` for type 4).
2. Decrypt in chunks of up to 512 bytes.
3. Write in 128- or 16-byte bursts.
4. Check `bin_crc` over the plaintext. On failure, erase the region again.
5. Delete the `.ota` or `.lib` (BL@0x080035B0).

**Factory reset (BL@0x08002390):**

1. Install the first `*.gld` (type 2).
2. Delete `1:/settings` and every `.ota` and `.lib`.

The `.gld` itself is kept.

**Boot:** see section 1.4. The only check is the initial SP range; nothing
verifies the kernel at boot.

**Ways to deliver an image:**

| Route | How |
|---|---|
| Bootloader menu | Hold the power button for 4 s at power-on. The menu offers USB MSC, with drive `0:` exposed. Copy the `.ota`, eject, choose **Continue** (README.md:53-62). |
| From a running stock kernel | It copies the file to `0:/ble.ota` and reboots (`main.h:286`; `Backend.cpp:2544-2583`). |

**Other bootloader behaviour:**

- It loads the first `0:/*.bmp` (8 bpp, 240 wide) as a **splash screen**
  (BL@0x08002986).
- It has its own display driver (it references GPIOE, LPTIM1 and LPTIM3), its
  own eMMC and FatFs, and USB MSC. It only reaches the UPD and SYS drives of
  the eMMC, and never erases (section 3.4).

**Safety.** Because the bootloader runs first on every boot and only checks
the SP, a broken PebbleOS image is recoverable: hold power for 4 s, then
install the stock `.ota` or `.gld` over USB (inferred from the README flow and
the jump logic).

### 9.5 Recipe for a PebbleOS packer

1. Link PebbleOS with its vector table at **`0x08060000`**. The initial SP
   must be in `[0x20000000, 0x20270000]`. Set VTOR yourself anyway.
2. Run `objcopy -O binary` to produce `pebble.bin`. It must be no larger than
   `flash_size − 384 KB`.
3. Pad `pebble.bin` with zeros to a multiple of 16. If
   `(len − 16) % 512 == 0`, add 16 more zeros. Call the result `P`.
4. Compute `bin_crc = crc32(P)`.
5. Build the plaintext: `subhdr = pack('<IIII', len(P), bin_crc, 0, 0)`, then
   `plain = subhdr + P`.
6. Encrypt: `enc = AES-256-CBC(key, iv).encrypt(plain)`, with no padding.
   Compute `enc_crc = crc32(enc)` and `enc_len = len(enc)`.
7. Build the header, 60 bytes:
   `pack('<IIII', 1, enc_crc, enc_len, bin_crc)` +
   `pack('<BBBBBBBB23sBBBBB8s', 1, 1, 0, 0, patch, minor, major, 0, suffix, 0, 0, 0, 0, 0, b'\0'*8)`.
   Byte `0x11` = 1 for `.ota`, 2 for `.gld`. The libc bytes at `0x30` are 0.
   Append `pack('<I', crc32(header60))`.
8. Write `file = header64 + enc` and give it a lowercase `.ota` name.

Calling the existing `encrypt_firmware.pack()` directly is equivalent.

**Open points for packaging:**

- (Inferred) The validation-pass "done" flag appears unused, so a truncated
  file might skip the final CRCs. A correct packer is unaffected.
- The CMOX mode descriptor was not decoded. CBC is confirmed only by the
  round-trip with the packer.
- Other bootloader versions may differ.
- **Never change the option bytes.** The bootloader manages `FLASH_OPTR`
  bit 21 itself.

### 9.6 Boot path, menu entry and PMIC writes (bootloader 0.1.4, traced)

Traced in the disassembly of `UnaBootloader_0.1.4.hex`; confirmed unless
marked inferred.

**What a running firmware can leave for the bootloader.** Only TAMP BKP3R
(base `0x46007000`, offset `0xD0C`), the power-off flag: written to 1 at
BL@0x08001616 (its own power-off), read at BL@0x080017AC and cleared at
BL@0x080017EC. No other backup register, RTC register or secure alias is
referenced. No RAM word is compared against a magic in the reset handler
(BL@0x08001C44), `main` (BL@0x08001970) or the boot thread (BL@0x08001760);
nothing refers to the kernel's `.boot_flags` at `0x2025E000` or to SRAM4.
The option bytes are only rewritten for the DUALBANK bit (BL@0x08001E34,
BL@0x08001E86). RCC_CSR is read (BL@0x08006338) and its cause stored at
`0x20073F14`, which nothing reads.

**Menu entry, BL@0x08002ECC(this, force).** 64 samples of the buttons at
1 ms, then BL@0x080029F8 asks for the menu only while PD15 (power) is pressed
and PD4, PA5 and PD8 released, continuously for more than 4000 ms
(`cmp #4000` at BL@0x08002A3A, polled every 10 ms). Releasing early or
pressing another button means no menu. The other way in is `force`, set
(BL@0x08001926) when the image's stack pointer at `0x08060000` is outside
`0x20000000`-`0x20270000` (BL@0x08001C98). **There is no flag a firmware can
set to get the menu, or USB mass storage, on the next boot.** Menu results:
0 start (battery check BL@0x08001630, update search BL@0x08002310, jump),
1 power off, 2 factory reset (BL@0x08002390). USB MSC ("UNA Bootloader MSD")
runs only inside the menu loop (BL@0x08002F90-0x08002FE0, start BL@0x08004354,
stop BL@0x08004390) while USB is in. The power-off check (BL@0x080017A6 to
BL@0x080017E0) runs only when BKP3R is set and no charger input: it polls the
button every 10 ms until 500 ms of uptime, and powers off again if it is
released first.

**PMIC writes, every boot.** `Pmic::init`, BL@0x08003D6C, called once at
BL@0x08001786, before the power-off check and the menu, with or without USB.
Register accesses: read BL@0x0800722C, write BL@0x0800727C (4 retries); the
charger registers are unlocked and locked again around each write
(BL@0x080074D4: CHG_CNTL0 bits 7:3 = 10101b, then 0). Each write is a
read-modify-write:

| BL@ | Register | Bits written |
|---|---|---|
| 0x08003D94 | CHG_CNTL0 (`0x10`) | bit 2 NTC_EN = 0 |
| 0x08003D9C (fn 0x08007562) | CHG_CNTL6 (`0x16`) | bit 7 NTC_RES_SEL = 0, bits 6:0 kept |
| 0x08003DA4 (fn 0x0800746A) | TOP_CNTL3 (`0x0C`) | bits 1:0 = 3 (long glitch filter) |
| 0x08003DAC (fn 0x0800750C) | CHG_CNTL1 (`0x11`) | bits 5:0 = 0x14, 100 mA |
| 0x08003DB6 (fn 0x080075AA) | CHG_CNTL5 (`0x15`) | bits 5:0 = (4400 − 3590) / 20 = 0x28, 4.40 V; bit 6 kept |
| then | mode and rail registers | fns 0x08007368, 0x0800739E, 0x080073D4, 0x080076A2 |

CHG_CNTL2 to 4, CHG_CNTL5 bit 6 and CHG_CNTL6 bits 6:0 are never written.
This is what turns PebbleOS's CHG_CNTL5 `0x66` (4.36 V) into `0x68` and its
CHG_CNTL6 `0x95` into `0x15` across a reset
(decision 261127-133657-01). Per the stock driver (`PCA9420.hpp` ~208-280,
`PCA9420.cpp`), CHG_CNTL5 bit 6 is VBAT_RESTART (0: 140 mV, 1: 240 mV
below VBAT_REG), so `0x68` is 4.40 V with PebbleOS's 240 mV threshold kept;
CHG_CNTL6 bits 1:0 top-off timer, 3:2 pre-charge timer, 5:4 fast charge
timer, 6 timers doubled, 7 NTC_RES_SEL (0: 100 kOhm, 1: 10 kOhm), so `0x15`
and `0x95` differ only in the thermistor bias, unused with NTC off.

**Ship mode at boot (BL@0x08003D1E, called at BL@0x08003D7C, before those
writes).** With no charger input (CHG_STATUS_0 `0x18` bit 5 clear, fn
BL@0x0800749A), CHG_CNTL6 bit 7 set, and none of the reset causes latched in
RESET_MON (`0x71`) bits 4:0 (long press, watchdog, software reset, ship
exit, thermal), it enters ship mode (fn BL@0x080072D4: MODECFG_0_0 `0x22`
bit 7) and stops: the watch looks dead until USB is plugged in or the power
button pressed. The bootloader takes NTC_RES_SEL = 1 for a PMIC at its
power-on defaults, since it clears the bit on every boot. PebbleOS's driver
writes CHG_CNTL6 = `0x95`, bit 7 set, so (inferred, not tried) a reset on
battery that the PMIC does not latch, such as an MCU reset from PebbleOS,
may end in ship mode; writing bit 7 as 0 (`0x15`) would avoid it.

---

## 10. Minimum viable bring-up

The goal is the smallest set of steps for a PebbleOS image that boots from the
stock bootloader, shows something on screen and reads the buttons.

1. **Image and packaging.**
   - Link at `0x08060000`, staying within 1664 KB unless you have confirmed a
     4 MB part.
   - Initial SP at or below `0x20270000`.
   - Use only `0x20000000`–`0x2026FFFF` (plus SRAM4 if you like).
   - Package as a type-1 `.ota` with the libc field = 0 (section 9.5).
   - Install through the bootloader menu over USB MSC.
2. **Reset path.**
   - Write VTOR.
   - `cpsie i` once the vectors and NVIC are sane (the bootloader leaves IRQs
     masked).
   - Enable the FPU (CPACR).
   - Turn on the ICACHE if you like.
3. **Power and clocks.**
   - `PWR`: select the SMPS supply, **enable VddIO2** (needed for port G),
     VOS1.
   - RCC: switch SYSCLK to MSIS (48 MHz, range 0), then program PLL1 as
     M = 3, N = 10, R = 1 for 160 MHz.
   - Flash latency 4.
   - Enable LSE in **bypass** mode (an external 32.768 kHz clock on PC14).
   - Running from MSIS 48 MHz alone also works for an MVP.
4. **Early console** (optional but recommended).
   - USART3 TX on PC4, AF7, 921600 baud, clocked from HSI16.
   - Drive PB15 (USB_DAM) high to route it to the connector, and use a Type-C
     debug-accessory adapter. Alternatively use SWD/RTT.
5. **Safe defaults for other GPIOs.** Drive all of these low:
   - PD10 (eMMC off)
   - PF2 (haptic off)
   - PB9, PD5, PB8 (buzzer off)
   - PG0 (barometer LDO off; it must not float)
   - PA8 (BLE in reset)
   - PG5 (GNSS off)

   Leave PF13 and PG10 as inputs (board pull-downs keep the PPG off). PMIC
   writes are not required for the display or buttons (inferred).
6. **Backlight.** PF3 open-drain, drive low for on. This is optional, since
   the panel is reflective.
7. **Display.** This needs a new STM32 JDI-parallel `pbl_display` driver.
   1. Set PA4, PA6, PA7, PB0, PB1, PB10 and PE0/2/4/8/10/12 as push-pull
      outputs, initially low, and the other PE pins as inputs with pull-up.
   2. Clock LPTIM1 and LPTIM3 from LSE. Set ARR 1066 and CCR 533. Put LPTIM1
      CH1 on PB2 (AF1) with high polarity and LPTIM3 CH1 on PD14 (AF14) with
      low polarity. Reset both counters, then start both. Enable `CLKAM` so
      they run in STOP2.
   3. For each frame: convert ARGB2222 to the wire format with R and B
      swapped, rotate 90° counter-clockwise, and clock it with the sequence in
      section 5.3. Hold at least 220 ns per data phase and at least 69 µs per
      GCK half period.
   4. A busy-wait implementation on the DWT is fine for a first light.
8. **Buttons.** `gpio-keys` on:
   - PD8 = back
   - PD4 = up
   - PD15 = select
   - PA5 = down

   All active low with internal pull-up, using EXTI4, EXTI15, EXTI5 and EXTI8
   on both edges. Debounce in software (about 50 ms). Pre-charging the RC
   filters by driving the pins high briefly is optional.
9. **Watchdog.** Not needed if you never start IWDG (inferred: the bootloader
   does not start it). If you enable it, IWDG runs from LSI at 32 kHz.

**Later stages, not needed for first light:**

| Stage | Hardware |
|---|---|
| I2C1 drivers | PCA9420 charging setup, MAX17262, STUSB4500, DRV2625 |
| Storage | SDMMC1 eMMC |
| USB | USB HS (DWC2) |
| BLE | SPI1 BlueNRG-2: an ST host stack, not Nordic or SiFli |
| GNSS | LPUART1 AG3335M |
| Sensors | I2C4 BMI270/BMM350, I2C2 PAH8316/MS5837 |
| Low power | STOP2 tickless idle with LSE SysTick or LPTIM |

**PebbleOS drivers that already exist for STM32 (H7) and probably need U5
adaptation:**

| Driver | Status |
|---|---|
| `drivers/gpio/stm32.c` | Exists |
| `drivers/pinctrl/stm32.c` | Exists |
| `drivers/uart/stm32.c` | Exists |
| `drivers/rtc/stm32.c` | Exists |
| `drivers/watchdog/stm32_iwdg.c` | Exists |
| `drivers/timer/stm32_lptim.c` | Exists |
| `drivers/udc/dwc2.c` | Exists |
| `drivers/input/gpio_keys.c` | Exists |
| STM32U5 RCC / clock driver | **Missing** |
| STM32 I2C | **Missing** |
| STM32 SPI | **Missing** |
| STM32 SDMMC | `drivers/mmc/stm32_sdmmc.c` (built, not yet run) |
| STM32 LPTIM PWM | **Missing** |
| JDI-parallel display driver for STM32 | **Missing** |
| DRV2625 driver | **Missing** |

---

## Open questions

1. **MCU on the actual watch.** Is it the U5A5QJ (4 MB) or the U595QI (2 MB)?
   Read `0x0BFA07A0` and `DBGMCU->IDCODE` on a real unit. Answered: the
   watch has the 4 MB U5A5, and PebbleOS images now use it.
2. **Panel identity and shape.** Is it the Sharp LS012B7DD06A or the JDI
   LPM012M387D, and is it round? Also needed: its datasheet timing and a
   round mask.
3. **Panel and peripheral supply rails.** Which PCA9420 rail powers which
   load is not in the source. This matters for any power sequencing and for
   charging.
4. **Bootloader state at jump.** Which clocks and peripherals are left
   running (LPTIM VCOM, USB, SDMMC), and whether an IWDG option byte is set.
5. **Whether PD15 is really the PCA9420 ON pin,** and the exact Mode 2 wake
   path.
6. **Debug-accessory pinout:** which USB-C pins carry USART3 TX in DAM mode.
7. ~~**Bootloader menu entry and timings** (BL@0x08002ECC).~~ Traced: see
   section 9.6. No software flag; the power button alone for more than 4 s.
8. **The cell.** Its capacity (200 or 280 mAh), its maximum charge voltage
   and current and its charge temperatures, from its data sheet or its label.
9. **Charging while off.** The bootloader sets the charge voltage to
   4.40 V and the current to 100 mA at every boot (section 9.6); whether a
   watch powered off charges as PebbleOS or the bootloader left it.
10. **The TS pin** of the PCA9420: whether a thermistor could be read there.
