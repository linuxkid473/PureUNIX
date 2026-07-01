#include <pureunix/io.h>
#include <pureunix/time.h>

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static uint8_t cmos_read(uint8_t reg)
{
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static int bcd_to_bin(uint8_t v)
{
    return ((v >> 4) * 10) + (v & 0x0F);
}

/* Howard Hinnant's days_from_civil, the forward counterpart of the
 * civil_from_days algorithm already used by shell/builtins.c's
 * unix_to_civil() to render timestamps. */
static int32_t days_from_civil(int32_t y, uint32_t m, uint32_t d)
{
    y -= (m <= 2) ? 1 : 0;
    int32_t era = (y >= 0 ? y : y - 399) / 400;
    uint32_t yoe = (uint32_t)(y - era * 400);
    uint32_t doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1u;
    uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

uint32_t time_now(void)
{
    uint8_t status_b = cmos_read(0x0B);
    bool binary = status_b & 0x04;

    int sec = cmos_read(0x00), min = cmos_read(0x02), hour = cmos_read(0x04);
    int day = cmos_read(0x07), mon = cmos_read(0x08), year = cmos_read(0x09);
    if (!binary) {
        sec = bcd_to_bin(sec); min = bcd_to_bin(min); hour = bcd_to_bin(hour);
        day = bcd_to_bin(day); mon = bcd_to_bin(mon); year = bcd_to_bin(year);
    }

    int32_t full_year = 2000 + year;
    int32_t days = days_from_civil(full_year, (uint32_t)mon, (uint32_t)day);
    return (uint32_t)days * 86400u + (uint32_t)hour * 3600u + (uint32_t)min * 60u + (uint32_t)sec;
}
