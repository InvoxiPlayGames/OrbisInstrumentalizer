/*
    oi_config.c - OrbisInstrumentalizer
    A small key = value settings file, so instruments can be calibrated without rebuilding.
    Licensed under the GNU Lesser General Public License version 2.1, or later.
*/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <GoldHEN/Common.h>
#include <orbis/libkernel.h>

#include "oi_config.h"
#include "oi_log.h"

#define OI_CONFIG_PATH "/data/GoldHEN/OrbisInstrumentalizer.ini"

#define OI_O_RDONLY 0x0000
#define OI_O_WRONLY 0x0001
#define OI_O_CREAT  0x0200
#define OI_O_TRUNC  0x0400

OIConfig oi_config = {
    .gh_tilt_threshold = 27000,
    .rb_tilt_threshold = 0x4000,
    .tilt_hold_ms = 50,
    .whammy_deadzone = 0x30,
    .drum_min_velocity = 10,
    .calibration_log = false,
    .latency_log = false,
};

static const char default_file[] =
    "; OrbisInstrumentalizer - ajustes dos instrumentos\n"
    "; Edite pelo FTP e feche/abra o Rock Band 4 para aplicar.\n"
    "\n"
    "; Guitarra Guitar Hero: valor do acelerometro que conta como inclinada (Overdrive).\n"
    "; Tocando fica entre ~12000 e ~24000, apontando para cima chega a 32767.\n"
    "; Menor = ativa com menos inclinacao.\n"
    "gh_tilt_threshold = 27000\n"
    "\n"
    "; Guitarra Rock Band: idem, para o sensor de tilt dela.\n"
    "rb_tilt_threshold = 16384\n"
    "\n"
    "; Tempo (ms) que a guitarra precisa ficar inclinada para ativar o Overdrive.\n"
    "; Evita ativar sem querer ao palhetar forte. 0 = instantaneo.\n"
    "tilt_hold_ms = 50\n"
    "\n"
    "; Alavanca (whammy): valores abaixo disso (0-255) contam como solta.\n"
    "; Parada, a sua oscila entre 0 e ~38.\n"
    "whammy_deadzone = 48\n"
    "\n"
    "; Bateria Guitar Hero: batidas mais fracas que isso (1-127) sao ignoradas.\n"
    "; Aumente se aparecerem notas fantasmas ao bater em pads vizinhos.\n"
    "drum_min_velocity = 10\n"
    "\n"
    "; Registra no log os valores brutos de tilt, whammy e forca das batidas (0 ou 1).\n"
    "calibration_log = 0\n"
    "\n"
    "; Registra no log a latencia da entrada (0 ou 1).\n"
    "latency_log = 0\n";

static int Clamp(int value, int min, int max) {
    return value < min ? min : value > max ? max : value;
}

static void Apply(const char *key, int value) {
    if (strcmp(key, "gh_tilt_threshold") == 0) oi_config.gh_tilt_threshold = Clamp(value, 0, 32767);
    else if (strcmp(key, "rb_tilt_threshold") == 0) oi_config.rb_tilt_threshold = Clamp(value, 0, 32767);
    else if (strcmp(key, "tilt_hold_ms") == 0) oi_config.tilt_hold_ms = Clamp(value, 0, 1000);
    else if (strcmp(key, "whammy_deadzone") == 0) oi_config.whammy_deadzone = Clamp(value, 0, 255);
    else if (strcmp(key, "drum_min_velocity") == 0) oi_config.drum_min_velocity = Clamp(value, 1, 127);
    else if (strcmp(key, "calibration_log") == 0) oi_config.calibration_log = value != 0;
    else if (strcmp(key, "latency_log") == 0) oi_config.latency_log = value != 0;
    else {
        OILog("Config: unknown setting %s\n", key);
        return;
    }
    OILog("Config: %s = %d\n", key, value);
}

static void Parse(char *text) {
    char *line = text;
    while (line != NULL && *line != '\0') {
        char *next = strchr(line, '\n');
        if (next != NULL)
            *next++ = '\0';
        char *comment = strpbrk(line, ";#");
        if (comment != NULL)
            *comment = '\0';
        char *equals = strchr(line, '=');
        if (equals != NULL) {
            *equals = '\0';
            char key[48];
            int k = 0;
            for (char *c = line; *c != '\0' && k < (int)sizeof(key) - 1; c++) {
                if (*c != ' ' && *c != '\t' && *c != '\r')
                    key[k++] = *c;
            }
            key[k] = '\0';
            char *end = NULL;
            long value = strtol(equals + 1, &end, 0);
            if (k > 0 && end != equals + 1)
                Apply(key, (int)value);
        }
        line = next;
    }
}

void OIConfigLoad(void) {
    int fd = sceKernelOpen(OI_CONFIG_PATH, OI_O_RDONLY, 0);
    if (fd < 0) {
        OILog("Config: no settings file, writing the defaults\n");
        fd = sceKernelOpen(OI_CONFIG_PATH, OI_O_WRONLY | OI_O_CREAT | OI_O_TRUNC, 0666);
        if (fd >= 0) {
            sceKernelWrite(fd, default_file, sizeof(default_file) - 1);
            sceKernelClose(fd);
        }
        return;
    }
    static char text[4096];
    int length = (int)sceKernelRead(fd, text, sizeof(text) - 1);
    sceKernelClose(fd);
    if (length <= 0)
        return;
    text[length] = '\0';
    Parse(text);
}
