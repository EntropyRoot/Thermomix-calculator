/* units.c - unit conversion table used by conv(x, from, to) */
#include "calc.h"

#include <string.h>
#include <strings.h>

enum {
    U_LEN, U_MASS, U_TIME, U_TEMP, U_AREA, U_VOL, U_SPEED, U_DATA, U_ENERGY,
    U_POWER, U_PRESS, U_FORCE, U_FREQ, U_ANGLE, U_NCAT
};

static const char *const cat_name[U_NCAT] = {
    "length", "mass", "time", "temperature", "area", "volume", "speed", "data",
    "energy", "power", "pressure", "force", "frequency", "angle",
};

typedef struct {
    const char *name;
    int cat;
    num f, off;          /* SI value = x * f + off */
} Unit;

static const Unit units[] = {
    {"m", U_LEN, 1, 0}, {"km", U_LEN, 1e3, 0}, {"dm", U_LEN, 0.1, 0}, {"cm", U_LEN, 1e-2, 0},
    {"mm", U_LEN, 1e-3, 0}, {"um", U_LEN, 1e-6, 0}, {"nm", U_LEN, 1e-9, 0},
    {"mi", U_LEN, 1609.344, 0}, {"yd", U_LEN, 0.9144, 0}, {"ft", U_LEN, 0.3048, 0},
    {"in", U_LEN, 0.0254, 0}, {"nmi", U_LEN, 1852, 0}, {"au", U_LEN, 149597870700.0, 0},
    {"ly", U_LEN, 9460730472580800.0, 0}, {"pc", U_LEN, 3.0856775814913673e16, 0},

    {"kg", U_MASS, 1, 0}, {"g", U_MASS, 1e-3, 0}, {"mg", U_MASS, 1e-6, 0}, {"ug", U_MASS, 1e-9, 0},
    {"t", U_MASS, 1e3, 0}, {"lb", U_MASS, 0.45359237, 0}, {"oz", U_MASS, 0.028349523125, 0},
    {"st", U_MASS, 6.35029318, 0}, {"ct", U_MASS, 2e-4, 0},

    {"s", U_TIME, 1, 0}, {"ms", U_TIME, 1e-3, 0}, {"us", U_TIME, 1e-6, 0}, {"ns", U_TIME, 1e-9, 0},
    {"min", U_TIME, 60, 0}, {"h", U_TIME, 3600, 0}, {"day", U_TIME, 86400, 0},
    {"week", U_TIME, 604800, 0}, {"month", U_TIME, 2629746, 0}, {"year", U_TIME, 31556952, 0},

    {"K", U_TEMP, 1, 0}, {"C", U_TEMP, 1, 273.15}, {"F", U_TEMP, 5.0 / 9, 273.15 - 32 * 5.0 / 9},

    {"m2", U_AREA, 1, 0}, {"km2", U_AREA, 1e6, 0}, {"cm2", U_AREA, 1e-4, 0}, {"mm2", U_AREA, 1e-6, 0},
    {"ha", U_AREA, 1e4, 0}, {"acre", U_AREA, 4046.8564224, 0}, {"ft2", U_AREA, 0.09290304, 0},
    {"in2", U_AREA, 0.00064516, 0}, {"mi2", U_AREA, 2589988.110336, 0},

    {"m3", U_VOL, 1, 0}, {"l", U_VOL, 1e-3, 0}, {"L", U_VOL, 1e-3, 0}, {"ml", U_VOL, 1e-6, 0},
    {"mL", U_VOL, 1e-6, 0}, {"cm3", U_VOL, 1e-6, 0}, {"gal", U_VOL, 3.785411784e-3, 0},
    {"qt", U_VOL, 9.46352946e-4, 0}, {"pt", U_VOL, 4.73176473e-4, 0}, {"cup", U_VOL, 2.365882365e-4, 0},
    {"floz", U_VOL, 2.95735295625e-5, 0}, {"tbsp", U_VOL, 1.478676478125e-5, 0},
    {"tsp", U_VOL, 4.92892159375e-6, 0},

    {"mps", U_SPEED, 1, 0}, {"kmh", U_SPEED, 1 / 3.6, 0}, {"mph", U_SPEED, 0.44704, 0},
    {"knot", U_SPEED, 1852.0 / 3600, 0}, {"fps", U_SPEED, 0.3048, 0},

    {"bit", U_DATA, 0.125, 0}, {"B", U_DATA, 1, 0}, {"KB", U_DATA, 1e3, 0}, {"MB", U_DATA, 1e6, 0},
    {"GB", U_DATA, 1e9, 0}, {"TB", U_DATA, 1e12, 0}, {"KiB", U_DATA, 1024, 0},
    {"MiB", U_DATA, 1048576, 0}, {"GiB", U_DATA, 1073741824.0, 0}, {"TiB", U_DATA, 1099511627776.0, 0},
    {"kbit", U_DATA, 125, 0}, {"Mbit", U_DATA, 125000, 0}, {"Gbit", U_DATA, 1.25e8, 0},

    {"J", U_ENERGY, 1, 0}, {"kJ", U_ENERGY, 1e3, 0}, {"MJ", U_ENERGY, 1e6, 0}, {"cal", U_ENERGY, 4.184, 0},
    {"kcal", U_ENERGY, 4184, 0}, {"Wh", U_ENERGY, 3600, 0}, {"kWh", U_ENERGY, 3.6e6, 0},
    {"eV", U_ENERGY, 1.602176634e-19, 0}, {"BTU", U_ENERGY, 1055.05585262, 0},

    {"W", U_POWER, 1, 0}, {"kW", U_POWER, 1e3, 0}, {"MW", U_POWER, 1e6, 0},
    {"hp", U_POWER, 745.69987158227022, 0},

    {"Pa", U_PRESS, 1, 0}, {"hPa", U_PRESS, 100, 0}, {"kPa", U_PRESS, 1e3, 0}, {"MPa", U_PRESS, 1e6, 0},
    {"bar", U_PRESS, 1e5, 0}, {"atm", U_PRESS, 101325, 0}, {"psi", U_PRESS, 6894.757293168361, 0},
    {"mmHg", U_PRESS, 133.322387415, 0}, {"torr", U_PRESS, 101325.0 / 760, 0},

    {"N", U_FORCE, 1, 0}, {"kN", U_FORCE, 1e3, 0}, {"lbf", U_FORCE, 4.4482216152605, 0},
    {"kgf", U_FORCE, 9.80665, 0}, {"dyn", U_FORCE, 1e-5, 0},

    {"Hz", U_FREQ, 1, 0}, {"kHz", U_FREQ, 1e3, 0}, {"MHz", U_FREQ, 1e6, 0}, {"GHz", U_FREQ, 1e9, 0},
    {"rpm", U_FREQ, 1.0 / 60, 0},

    {"rad", U_ANGLE, 1, 0}, {"deg", U_ANGLE, C_PI / 180, 0}, {"grad", U_ANGLE, C_PI / 200, 0},
    {"turn", U_ANGLE, 2 * C_PI, 0}, {"arcmin", U_ANGLE, C_PI / 10800, 0},
    {"arcsec", U_ANGLE, C_PI / 648000, 0},
};

#define NUNITS ((int)(sizeof units / sizeof units[0]))

int unit_lookup(const char *name, int len)
{
    int found = -1;
    for (int i = 0; i < NUNITS; i++)
        if (!strncmp(units[i].name, name, (size_t)len) && units[i].name[len] == 0) return i;
    for (int i = 0; i < NUNITS; i++)       /* unique case-insensitive match: kb, c, pa */
        if (!strncasecmp(units[i].name, name, (size_t)len) && units[i].name[len] == 0) {
            if (found >= 0) return -1;
            found = i;
        }
    return found;
}

const char *unit_check(int from, int to)
{
    static char msg[96];
    if (units[from].cat == units[to].cat) return NULL;
    snprintf(msg, sizeof msg, "cannot convert %s (%s) to %s (%s)", units[from].name,
             cat_name[units[from].cat], units[to].name, cat_name[units[to].cat]);
    return msg;
}

num unit_convert(num v, int from, int to)
{
    if (from == to) return v;
    return (v * units[from].f + units[from].off - units[to].off) / units[to].f;
}

void units_help(Buf *out)
{
    for (int c = 0; c < U_NCAT; c++) {
        buf_printf(out, "  %-12s", cat_name[c]);
        for (int i = 0; i < NUNITS; i++)
            if (units[i].cat == c) buf_printf(out, " %s", units[i].name);
        buf_putc(out, '\n');
    }
}
