#include "lcd.h"

/*
 * HD44780 LCD basic command definitions
 */
#define LCD_CMD_CLEAR_DISPLAY   0x01
#define LCD_CMD_RETURN_HOME     0x02
#define LCD_CMD_ENTRY_MODE_SET  0x06    // Increment cursor, no shift
#define LCD_CMD_DISPLAY_ON      0x0C    // Display ON, cursor OFF, blink OFF
#define LCD_CMD_FUNCTION_SET    0x28    // 4-bit mode, 2 lines, 5x8 font
#define LCD_CMD_SET_DDRAM_ADDR  0x80    // Base address for display RAM

/*
 * Internal function declarations
 */
static void LCD_Write4Bits(uint8_t nibble);
static void LCD_Send(uint8_t value, GPIO_PinState rs);
static void LCD_PulseEnable(void);
static void LCD_Command(uint8_t cmd);
static void LCD_Data(uint8_t data);

/*
 * Sends 4 data bits (D4..D7).
 * We use the fact that HAL treats 0 as GPIO_PIN_RESET
 * and any non-zero as GPIO_PIN_SET, so (nibble & mask)
 * is valid as GPIO_PinState.
 */
static void LCD_Write4Bits(uint8_t nibble)
{
    HAL_GPIO_WritePin(LCD_D4_GPIO_Port, LCD_D4_Pin, (nibble & 0x01));
    HAL_GPIO_WritePin(LCD_D5_GPIO_Port, LCD_D5_Pin, (nibble & 0x02));
    HAL_GPIO_WritePin(LCD_D6_GPIO_Port, LCD_D6_Pin, (nibble & 0x04));
    HAL_GPIO_WritePin(LCD_D7_GPIO_Port, LCD_D7_Pin, (nibble & 0x08));

    LCD_PulseEnable();
}

/*
 * Generate the Enable (E) pulse required by the LCD.
 */
static void LCD_PulseEnable(void)
{
    HAL_GPIO_WritePin(LCD_E_GPIO_Port, LCD_E_Pin, GPIO_PIN_RESET);
    HAL_Delay(1);
    HAL_GPIO_WritePin(LCD_E_GPIO_Port, LCD_E_Pin, GPIO_PIN_SET);
    HAL_Delay(1);
    HAL_GPIO_WritePin(LCD_E_GPIO_Port, LCD_E_Pin, GPIO_PIN_RESET);
    HAL_Delay(1);
}

/*
 * Sends a byte to the LCD (either command or data).
 * rs = GPIO_PIN_RESET → Command
 * rs = GPIO_PIN_SET   → Data
 */
static void LCD_Send(uint8_t value, GPIO_PinState rs)
{
    HAL_GPIO_WritePin(LCD_RS_GPIO_Port, LCD_RS_Pin, rs);

    LCD_Write4Bits(value >> 4);   // upper nibble
    LCD_Write4Bits(value & 0x0F); // lower nibble
}

/*
 * Send command byte
 */
static void LCD_Command(uint8_t cmd)
{
    LCD_Send(cmd, GPIO_PIN_RESET);

    // Clear and Home require a longer delay
    if (cmd == LCD_CMD_CLEAR_DISPLAY || cmd == LCD_CMD_RETURN_HOME)
    {
        HAL_Delay(2);
    }
}

/*
 * Send data byte (character)
 */
static void LCD_Data(uint8_t data)
{
    LCD_Send(data, GPIO_PIN_SET);
}

/*
 * Public API
 */

/*
 * Initialize LCD in 4-bit mode.
 * Follows the timing requirements of the HD44780 datasheet.
 */
void LCD_Init(void)
{
    // Turn on backlight
    HAL_GPIO_WritePin(LCD_BL_GPIO_Port, LCD_BL_Pin, GPIO_PIN_SET);

    HAL_Delay(50); // LCD power-on delay

    // Initialization sequence for 4-bit mode
    LCD_Write4Bits(0x03);
    HAL_Delay(5);
    LCD_Write4Bits(0x03);
    HAL_Delay(5);
    LCD_Write4Bits(0x03);
    HAL_Delay(1);
    LCD_Write4Bits(0x02); // switch to 4-bit mode

    // Configuration commands
    LCD_Command(LCD_CMD_FUNCTION_SET);
    LCD_Command(LCD_CMD_DISPLAY_ON);
    LCD_Command(LCD_CMD_ENTRY_MODE_SET);
    LCD_Command(LCD_CMD_CLEAR_DISPLAY);
}

/*
 * Clear the display
 */
void LCD_Clear(void)
{
    LCD_Command(LCD_CMD_CLEAR_DISPLAY);
}

/*
 * Move cursor to (col, row)
 * LCD has 2 rows: row 0 starts at 0x00, row 1 at 0x40
 */
void LCD_SetCursor(uint8_t col, uint8_t row)
{
    static const uint8_t row_offsets[] = {0x00, 0x40};

    if (row > 1) row = 1;

    LCD_Command(LCD_CMD_SET_DDRAM_ADDR | (col + row_offsets[row]));
}

/*
 * Print a null-terminated string
 */
void LCD_Print(const char *str)
{
    while (*str)
    {
        LCD_Data((uint8_t)*str++);
    }
}

/*
 * Print a single character
 */
void LCD_WriteChar(char c)
{
    LCD_Data((uint8_t)c);
}

void LCD_PrintAt(uint8_t col, uint8_t row, const char *str)
{
    LCD_SetCursor(col, row);
    LCD_Print(str);
}
