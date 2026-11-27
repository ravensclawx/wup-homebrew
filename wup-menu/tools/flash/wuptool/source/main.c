// WUP Tool - Comprehensive Wii U GamePad Tool
//
// Commands:
//   'I' - Info: send flash ID (4 bytes)
//   'R' - Read flash: recv addr(4), len(4), send data
//   'W' - Write flash: recv addr(4), len(4), recv data
//   'E' - Erase sector: recv addr(4)
//   'B' - Get boot partition: send 1 byte
//   'S' - Set boot partition: recv 1 byte
//   'M' - Memory read: recv addr(4), len(4), send data
//   'N' - Memory write: recv addr(4), len(4), recv data
//   'C' - Config (service mode): recv cmd(1), send/recv data
//   'L' - LED: recv led(1), brightness(1)
//   'U' - Rumble: recv on/off(1)
//   'H' - Hardware ID: send 4 bytes
//   'D' - EEPROM read: recv addr(2), len(2), send data
//   'F' - EEPROM write: recv addr(2), len(2), recv data, commit
//   'P' - Power off
//   'Q' - Quit (stay on)

#include <wup/wup.h>

// Flash layout
#define FLASH_SIZE      0x2000000   // 32MB
#define SECTOR_SIZE     0x10000     // 64KB
#define PAGE_SIZE       0x100       // 256 bytes

#define BANK_ADDR       0x000F000   // Boot partition selector

// UIC Commands
#define UIC_CMD_WRITE_EEPROM    0x02
#define UIC_CMD_READ_EEPROM     0x03
#define UIC_CMD_EEPROM_COMMIT   0x04
#define UIC_CMD_EEPROM_UNLOCK   0x06

// EEPROM addresses for board config
#define EEPROM_BOARD_CONFIG1     0x1206
#define EEPROM_BOARD_CONFIG1_CRC 0x1207
#define EEPROM_BOARD_CONFIG2     0x1286
#define EEPROM_BOARD_CONFIG2_CRC 0x1287

static u8 page_buf[PAGE_SIZE];

static u16 recv_u16(void)
{
    u16 val = 0;
    val |= UART_RecvByte();
    val |= UART_RecvByte() << 8;
    return val;
}

static u32 recv_u32(void)
{
    u32 val = 0;
    val |= UART_RecvByte();
    val |= UART_RecvByte() << 8;
    val |= UART_RecvByte() << 16;
    val |= UART_RecvByte() << 24;
    return val;
}

static void send_u32(u32 val)
{
    UART_SendByte(val & 0xFF);
    UART_SendByte((val >> 8) & 0xFF);
    UART_SendByte((val >> 16) & 0xFF);
    UART_SendByte((val >> 24) & 0xFF);
}

// CRC16 for EEPROM (reflected CRC-16-CCITT)
static u16 crc16(u8 *data, u16 len)
{
    u16 crc = 0xFFFF;
    for (u16 i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (u8 j = 0; j < 8; j++)
            crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : crc >> 1;
    }
    return crc;
}

static u8 uic_read_eeprom(u16 addr)
{
    u8 buf[3] = { (addr >> 8) & 0xFF, addr & 0xFF, 1 };
    u8 data = 0;
    UIC_SendCommand(UIC_CMD_READ_EEPROM, buf, 3, &data, 1);
    return data;
}

static void uic_write_eeprom(u16 addr, u8 val)
{
    u8 buf[4] = { (addr >> 8) & 0xFF, addr & 0xFF, 1, val };
    UIC_SendCommand(UIC_CMD_WRITE_EEPROM, buf, 4, NULL, 0);
    WUP_DelayUS(100);
}

static void write_config_with_crc(u16 addr, u16 crc_addr, u8 value)
{
    u16 crc = crc16(&value, 1);
    uic_write_eeprom(addr, value);
    uic_write_eeprom(crc_addr, crc & 0xFF);
    uic_write_eeprom(crc_addr + 1, (crc >> 8) & 0xFF);
}

void main(void)
{
    UIC_SetLED(0, 255);

    UART_Print("\r\n\r\n");
    UART_Print("=== WUP Tool v1.0 ===\r\n");

    // Read and display flash ID
    u8 id[4];
    Flash_ReadID(id, 4);
    UART_Print("Flash: ");
    UART_PrintHex8(id[0]);
    UART_Print(" ");
    UART_PrintHex8(id[1]);
    UART_Print(" ");
    UART_PrintHex8(id[2]);
    UART_Print("\r\n");

    UART_Print("WUPTOOL\r\n");

    while (1)
    {
        u8 cmd = UART_RecvByte();

        switch (cmd)
        {
        case 'I':  // Flash Info
        {
            UART_Print("OK\r\n");
            Flash_ReadID(id, 4);
            UART_SendByte(id[0]);
            UART_SendByte(id[1]);
            UART_SendByte(id[2]);
            UART_SendByte(id[3]);
            break;
        }

        case 'R':  // Read Flash
        {
            u32 addr = recv_u32();
            u32 len = recv_u32();

            if (addr >= 0x1000000 || (addr + len) > 0x1000000)
                Flash_Set4ByteAddr(1);
            else
                Flash_Set4ByteAddr(0);

            UART_Print("OK\r\n");

            u32 remaining = len;
            u32 pos = addr;
            while (remaining > 0)
            {
                u32 chunk = (remaining > PAGE_SIZE) ? PAGE_SIZE : remaining;
                Flash_Read(pos, page_buf, chunk);
                for (u32 i = 0; i < chunk; i++)
                    UART_SendByte(page_buf[i]);
                pos += chunk;
                remaining -= chunk;
            }

            Flash_Set4ByteAddr(0);
            break;
        }

        case 'W':  // Write Flash
        {
            u32 addr = recv_u32();
            u32 len = recv_u32();

            if (len == 0 || (addr + len) > FLASH_SIZE)
            {
                UART_Print("ERR\r\n");
                break;
            }

            if (addr >= 0x1000000 || (addr + len) > 0x1000000)
                Flash_Set4ByteAddr(1);
            else
                Flash_Set4ByteAddr(0);

            // Erase sectors
            u32 start_sector = addr & ~(SECTOR_SIZE - 1);
            u32 end_addr = addr + len;
            u32 num_sectors = ((end_addr - start_sector) + SECTOR_SIZE - 1) / SECTOR_SIZE;

            UART_Print("ERASE ");
            UART_PrintHex32(num_sectors);
            UART_Print("\r\n");

            for (u32 i = 0; i < num_sectors; i++)
                Flash_EraseSector(start_sector + i * SECTOR_SIZE);

            UART_Print("GO\r\n");

            // Write data
            u32 remaining = len;
            u32 pos = addr;

            while (remaining > 0)
            {
                u32 page_len = (remaining > PAGE_SIZE) ? PAGE_SIZE : remaining;

                for (u32 i = 0; i < page_len; i++)
                    page_buf[i] = UART_RecvByte();

                Flash_PageProgram(pos, page_buf, page_len);

                pos += page_len;
                remaining -= page_len;

                if ((pos & 0xFFFF) == 0)
                    UART_Print(".");
            }

            Flash_Set4ByteAddr(0);
            UART_Print("\r\nDONE\r\n");
            break;
        }

        case 'w':  // Write Flash by 4 KB subsectors (addr and len 4 KB aligned): only those erased
        {
            u32 addr = recv_u32();
            u32 len = recv_u32();
            if (len == 0 || (addr | len) & 0xFFF || (addr + len) > FLASH_SIZE) { UART_Print("ERR\r\n"); break; }
            Flash_Set4ByteAddr(addr >= 0x1000000 || (addr + len) > 0x1000000);
            for (u32 o = 0; o < len; o += 0x1000) Flash_EraseSubsector(addr + o);
            UART_Print("GO\r\n");
            for (u32 pos = addr; pos < addr + len; pos += PAGE_SIZE)
            {
                for (u32 i = 0; i < PAGE_SIZE; i++) page_buf[i] = UART_RecvByte();
                Flash_PageProgram(pos, page_buf, PAGE_SIZE);
            }
            Flash_Set4ByteAddr(0);
            UART_Print("DONE\r\n");
            break;
        }

        case 'K':  // CRC-32 (zlib's) of a flash range: checking without reading it all back
        {
            u32 addr = recv_u32();
            u32 len = recv_u32();
            Flash_Set4ByteAddr(addr >= 0x1000000 || (addr + len) > 0x1000000);
            u32 crc = 0xFFFFFFFF;
            for (u32 pos = addr; pos < addr + len; )
            {
                u32 chunk = (addr + len - pos > PAGE_SIZE) ? PAGE_SIZE : addr + len - pos;
                Flash_Read(pos, page_buf, chunk);
                for (u32 i = 0; i < chunk; i++)
                {
                    crc ^= page_buf[i];
                    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
                }
                pos += chunk;
            }
            Flash_Set4ByteAddr(0);
            UART_Print("OK\r\n");
            crc = ~crc;
            UART_SendByte(crc); UART_SendByte(crc >> 8); UART_SendByte(crc >> 16); UART_SendByte(crc >> 24);
            break;
        }

        case 'E':  // Erase Sector
        {
            u32 addr = recv_u32();

            if (addr >= 0x1000000)
                Flash_Set4ByteAddr(1);
            else
                Flash_Set4ByteAddr(0);

            Flash_EraseSector(addr);
            Flash_Set4ByteAddr(0);
            UART_Print("OK\r\n");
            break;
        }

        case 'B':  // Get Boot Partition
        {
            u8 boot_byte;
            Flash_Set4ByteAddr(0);
            Flash_Read(BANK_ADDR, &boot_byte, 1);
            UART_Print("OK\r\n");
            UART_SendByte(boot_byte);
            break;
        }

        case 'S':  // Set Boot Partition
        {
            u8 partition = UART_RecvByte();
            if (partition > 1)
            {
                UART_Print("ERR\r\n");
                break;
            }

            Flash_Set4ByteAddr(0);
            Flash_EraseSubsector(BANK_ADDR);
            Flash_PageProgram(BANK_ADDR, &partition, 1);
            UART_Print("OK\r\n");
            break;
        }

        case 'M':  // Memory Read (RAM/registers)
        {
            u32 addr = recv_u32();
            u32 len = recv_u32();

            UART_Print("OK\r\n");

            for (u32 i = 0; i < len; i++)
                UART_SendByte(*(vu8*)(addr + i));
            break;
        }

        case 'N':  // Memory Write (RAM/registers)
        {
            u32 addr = recv_u32();
            u32 len = recv_u32();

            for (u32 i = 0; i < len; i++)
                *(vu8*)(addr + i) = UART_RecvByte();

            UART_Print("OK\r\n");
            break;
        }

        case 'C':  // Config (Service Mode)
        {
            u8 subcmd = UART_RecvByte();

            if (subcmd == 0)  // Read config
            {
                u8 cfg = uic_read_eeprom(EEPROM_BOARD_CONFIG1);
                UART_Print("OK\r\n");
                UART_SendByte(cfg);
            }
            else if (subcmd == 1)  // Enable service mode
            {
                u8 cfg = uic_read_eeprom(EEPROM_BOARD_CONFIG1);
                u8 new_cfg = (cfg & 0xFC) | 0x00;  // Clear bits 0-1

                UIC_SendCommand(UIC_CMD_EEPROM_UNLOCK, NULL, 0, NULL, 0);
                WUP_DelayMS(10);

                write_config_with_crc(EEPROM_BOARD_CONFIG1, EEPROM_BOARD_CONFIG1_CRC, new_cfg);
                write_config_with_crc(EEPROM_BOARD_CONFIG2, EEPROM_BOARD_CONFIG2_CRC, new_cfg);

                UIC_SendCommand(UIC_CMD_EEPROM_COMMIT, NULL, 0, NULL, 0);
                WUP_DelayMS(150);

                u8 verify = uic_read_eeprom(EEPROM_BOARD_CONFIG1);
                UART_Print("OK\r\n");
                UART_SendByte(verify);
            }
            else if (subcmd == 2)  // Restore retail mode
            {
                UIC_SendCommand(UIC_CMD_EEPROM_UNLOCK, NULL, 0, NULL, 0);
                WUP_DelayMS(10);

                write_config_with_crc(EEPROM_BOARD_CONFIG1, EEPROM_BOARD_CONFIG1_CRC, 0x15);
                write_config_with_crc(EEPROM_BOARD_CONFIG2, EEPROM_BOARD_CONFIG2_CRC, 0x15);

                UIC_SendCommand(UIC_CMD_EEPROM_COMMIT, NULL, 0, NULL, 0);
                WUP_DelayMS(150);

                u8 verify = uic_read_eeprom(EEPROM_BOARD_CONFIG1);
                UART_Print("OK\r\n");
                UART_SendByte(verify);
            }
            else
            {
                UART_Print("ERR\r\n");
            }
            break;
        }

        case 'L':  // LED Control
        {
            u8 led = UART_RecvByte();
            u8 brightness = UART_RecvByte();
            UIC_SetLED(led, brightness);
            UART_Print("OK\r\n");
            break;
        }

        case 'U':  // Rumble
        {
            u8 on = UART_RecvByte();
            if (on)
                REG_GPIO_RUMBLE = GPIO_OUTPUT_HIGH;
            else
                REG_GPIO_RUMBLE = GPIO_OUTPUT_LOW;
            UART_Print("OK\r\n");
            break;
        }

        case 'H':  // Hardware ID
        {
            u32 hwid = REG_HARDWARE_ID;
            UART_Print("OK\r\n");
            send_u32(hwid);
            break;
        }

        case 'D':  // EEPROM Read
        {
            u16 addr = recv_u16();
            u16 len = recv_u16();

            UART_Print("OK\r\n");

            for (u16 i = 0; i < len; i++)
            {
                u8 val = uic_read_eeprom(addr + i);
                UART_SendByte(val);
                // Small delay to prevent UART overflow
                WUP_DelayUS(100);
            }
            break;
        }

        case 'F':  // EEPROM Write
        {
            u16 addr = recv_u16();
            u16 len = recv_u16();

            UIC_SendCommand(UIC_CMD_EEPROM_UNLOCK, NULL, 0, NULL, 0);
            WUP_DelayMS(10);

            for (u16 i = 0; i < len; i++)
            {
                u8 val = UART_RecvByte();
                uic_write_eeprom(addr + i, val);
            }

            UIC_SendCommand(UIC_CMD_EEPROM_COMMIT, NULL, 0, NULL, 0);
            WUP_DelayMS(150);

            UART_Print("OK\r\n");
            break;
        }

        case 'P':  // Power Off
        {
            UART_Print("POWEROFF\r\n");
            UIC_SetLED(0, 0);
            WUP_DelayMS(100);
            UIC_Shutdown();
            for(;;);
        }

        case 'Q':  // Quit (stay on)
        {
            UART_Print("BYE\r\n");
            UIC_SetLED(0, 0);
            for(;;);
        }

        default:
            UART_Print("?\r\n");
            break;
        }
    }
}
